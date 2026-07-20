/*
  ============================================================
   main.cpp  -- where the firmware starts (setup) and decides what to be
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  This is the entry point. On every boot it figures out WHICH ROLE this
  box should play, then runs that role:
     - GATEWAY (box_id = 0)   -> always on; receives + uploads data.
     - SENSOR  (box_id = 1..3) -> wakes, measures, sends, sleeps.

  HARDWARE: ESP32-C3.  Wireless link between boxes = ESP-NOW (no LoRa).

  ORDER OF EVENTS AT BOOT (setup())
  ---------------------------------
    1. Start Serial + print a banner.
    2. Read this chip's identity (from its MAC address).
    3. Load the commissioning profile from NVS (house, role, wifi, key).
    4. If the pairing button is held -> enter BLE pairing.
    5. If not commissioned yet (house_id == 0) -> enter BLE pairing.
    6. Set up encryption (AES-128-GCM) using the per-house key.
    7. Branch: run_gateway_mode()  OR  run_sensor_mode().
*/

#include <Arduino.h>
#include "log.h"
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_random.h>
#include <strings.h>

#include "config.h"
#include "core/identity.h"
#include "core/commission.h"
#include "core/crypto.h"
#include "core/device_cert.h"
#include "core/payload.h"
#include "core/seq_counter.h"
#include "core/tasks.h"
#include "core/wifi_manager.h"
#include "core/mqtt_handler.h"
#include "core/ble_pairing.h"
#include "core/storage.h"
#include "core/ota_remote.h"
#include "core/selftest.h"          // PRE-PROD: power-on hardware self-test
#include "core/portal_provision.h"  // PRE-PROD: app-free web provisioning
#include "drivers/SHT41.h"
#include "drivers/Battery.h"
#include "drivers/EspNow.h"
#include "drivers/Led.h"


// true once the gateway's background jobs are running (keeps loop() alive).
static bool g_gateway_runtime = false;

// Anti-replay counter for this sensor. RTC_DATA_ATTR keeps it through
// deep sleep (it only resets on a full power loss). It must only ever
// go UP so the backend can reject replayed packets.
RTC_DATA_ATTR static uint32_t g_sensor_seq = 0;

// This sensor's PREVIOUS wake duration (ms). Captured just before deep sleep,
// survives sleep in RTC memory, and is sent in the NEXT reading's payload so
// the backend can graph awake time (battery health) per device. 0 on a gateway
// / first wake after power loss.
RTC_DATA_ATTR static uint16_t g_last_awake_ms = 0;


// ==================================================================
//  Small boot-time helpers
// ==================================================================

static void print_banner() {
    LOG.println();
    LOG.println("=============================================");
    LOG.println("   STAYCOOL UNIFIED BOX -- booting...");
    LOG.println("=============================================");
    LOG.printf("   Firmware   : %s\n", FIRMWARE_VERSION);
    LOG.printf("   Payload v  : %d\n", PAYLOAD_VERSION);
    LOG.printf("   Link       : ESP-NOW (ESP32-C3)\n");
    LOG.printf("   Broker     : %s:%d (mTLS)\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    LOG.println("=============================================");
}


static void print_wake_reason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    LOG.print("[Boot] Wake reason: ");
    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER: LOG.println("TIMER (deep sleep)"); break;
        case ESP_SLEEP_WAKEUP_EXT0:  LOG.println("EXTERNAL PIN");       break;
        default:                     LOG.println("POWER ON / RESET");   break;
    }
}


static void deep_sleep_now() {
    // Add a random 0..JITTER offset so several sensors in the same house do not
    // stay in lock-step and collide on ESP-NOW. esp_random() is a true hardware
    // RNG here (the radio is on during the wake), so the phase decorrelates.
    uint64_t jitter_us = (DEEP_SLEEP_JITTER_US > 0)
                           ? ((uint64_t)esp_random() % DEEP_SLEEP_JITTER_US)
                           : 0;
    uint64_t sleep_us = DEEP_SLEEP_DURATION_US + jitter_us;
    LOG.printf("[Power] Deep sleep for ~%.0f s (base %.0f + jitter %.0f).\n",
                  sleep_us / 1000000.0,
                  DEEP_SLEEP_DURATION_US / 1000000.0,
                  jitter_us / 1000000.0);

    // Remember this wake's total awake time (millis() resets each wake) so the
    // NEXT wake reports it in its payload. Cap at uint16 max (~65 s).
    uint32_t awake = millis();
    g_last_awake_ms = (awake > 65535UL) ? 65535 : (uint16_t)awake;

    // Bench diagnostic: total awake time this wake. Uses raw Serial (not LOG)
    // so the number still prints while SERIAL_DEBUG=0 silences everything else,
    // letting you A/B the wake window. millis() ~= the full awake duration.
    // (With ARDUINO_USB_CDC_ON_BOOT the CDC is up even without Serial.begin.)
    Serial.printf("[AWAKE] total %lu ms\n", (unsigned long)awake);   // raw: prints even when SERIAL_DEBUG=0
#if SERIAL_DEBUG
    Serial.flush();
#endif
    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
}


// Open BLE pairing for a while; if nothing arrives, sleep and try later.
static void run_pairing_window_and_fallback() {
    ble_init();
    ble_pairing_run();
    ble_shutdown();
    LOG.println("[Boot] Pairing timeout. Sleeping before retry.");
    deep_sleep_now();
}


// Button-at-boot has three hold zones. We decide on RELEASE so a longer
// hold can reach the second threshold (acting at 3 s would fire pairing
// before the installer ever reaches the 10 s reset). The STATUS LED marks
// each zone as it is crossed so the installer knows when to let go:
//     < 3 s   -> normal boot
//     3-10 s  -> BLE pairing (unchanged)
//     >= 10 s -> wipe commissioning + reboot into the web portal
// Only the "commission" NVS namespace is cleared -- the device's private
// key + certificate ("devcert") are kept, so re-provisioning does not mint
// a new certificate.
//
// RESETTING A SLEEPING SENSOR: on the ESP32-C3 only GPIO0..5 can wake the
// chip from deep sleep (SOC_GPIO_DEEP_SLEEP_WAKEUP_VALID_GPIO_MASK), and the
// button is on GPIO10 -- so a press cannot wake a sleeping sensor instantly.
// Instead this runs at the START of every wake, so the fix is: PRESS AND HOLD
// the button continuously; at the sensor's next scheduled wake (<= the deep-
// sleep period) it lands here with the button held, blinks 3x at 10 s, and on
// release wipes + reboots into the portal. No USB needed.
static void handle_boot_button() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    if (digitalRead(PIN_BUTTON) != LOW) return;

    LOG.println("[Boot] Button held -- keep holding: 3s = pairing, 10s = reset provisioning");
    uint32_t start = millis();
    bool hit_pair = false, hit_reset = false;

    while (digitalRead(PIN_BUTTON) == LOW) {
        uint32_t held = millis() - start;
        if (!hit_pair && held >= 3000) {
            hit_pair = true;
            led_status_blink(1);   // 1 blink = pairing zone; release now for BLE pairing
            LOG.println("[Boot] >=3s reached -- release now for BLE PAIRING.");
        }
        if (!hit_reset && held >= 10000) {
            hit_reset = true;
            led_status_blink(3);   // 3 blinks = reset zone; release now to wipe provisioning
            LOG.println("[Boot] >=10s reached -- release now to RESET provisioning.");
        }
        delay(20);
    }

    uint32_t held = millis() - start;
    if (held >= 10000) {
        LOG.println("[Boot] RESET: wiping commissioning, rebooting into the web portal.");
        commission_wipe();
        delay(300);
        ESP.restart();            // reboots uncommissioned -> self-test + portal (see setup())
    } else if (held >= 3000) {
        LOG.println("[Boot] BLE pairing mode.");
        run_pairing_window_and_fallback();
    } else {
        LOG.println("[Boot] Button released early -- normal boot.");
    }
}


// ==================================================================
//  SENSOR MODE helpers
// ==================================================================

// Encrypt one reading and send it over ESP-NOW. Returns the result
// (was it delivered? did the gateway send a command back?).
static SendResult send_encrypted(const AchasPayload& payload) {

    SendResult res;
    res.delivered        = false;
    res.downlink.command = Command::NONE;
    res.downlink.valid   = false;

    uint8_t envelope[sizeof(AchasPayload) + AES_ENVELOPE_OVERHEAD];
    size_t  envelope_len = crypto_encrypt(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload),
        envelope, sizeof(envelope));

    if (envelope_len == 0) {
        LOG.println("[Sensor] ERROR: crypto_encrypt failed.");
        return res;
    }
    // Pass this reading's seq so any command in the reply can be authenticated
    // (the gateway signs commands with HMAC(house_key, seq || cmd)).
    return espnow_send_payload(envelope, envelope_len, payload.seq);
}


// On wake, first try to re-send anything that failed last time.
static void retry_pending_payload() {

    if (!storage_has_pending()) {
        LOG.println("[Sensor] No pending payload in NVS.");
        return;
    }

    AchasPayload pending;
    if (!storage_load_pending(pending)) {
        LOG.println("[Sensor] Pending payload unreadable -- clearing.");
        storage_clear_pending();
        return;
    }
    if (storage_get_retry_count() >= STORAGE_MAX_RETRIES) {
        LOG.println("[Sensor] Max retries reached -- dropping payload.");
        storage_clear_pending();
        return;
    }

    SendResult r = send_encrypted(pending);
    if (r.delivered) storage_clear_pending();
    else             storage_increment_retry();
}


// Act on a command the gateway sent back (OTA / PING / REBOOT / PAIR).
// Note: ESP-NOW/WiFi is already OFF here, so BLE / OTA can use the radio.
static void handle_downlink(const Downlink& dl) {
    if (!dl.valid) return;

    switch (dl.command) {
        case Command::PING:
            LOG.println("[Sensor] Downlink: PING");
            break;
        case Command::REBOOT:
            LOG.println("[Sensor] Downlink: REBOOT");
            delay(200);
            ESP.restart();
            break;
        case Command::PAIR:
            LOG.println("[Sensor] Downlink: PAIR");
            ble_pairing_run();   // starts BLE itself
            ble_shutdown();
            ESP.restart();
            break;
        case Command::OTA: {
            LOG.println("[Sensor] Downlink: OTA");
            OtaResult res = ota_remote_run(dl.ota_url, dl.ota_sha256);
            LOG.printf("[Sensor] OTA result: %s\n", ota_result_str(res));
            break;
        }
        default:
            break;
    }
}


// ==================================================================
//  SENSOR MODE  (wake -> measure -> send -> sleep)
// ==================================================================
static void run_sensor_mode(const CommissionData& commission) {
    LOG.println("[Mode] SENSOR mode");

    // 1. Storage (for retrying failed sends).
    storage_init();

    // 2. Start the I2C bus (the sensor is always powered now).
    Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL);
    Wire.setTimeOut(50);
    Wire.setClock(400000);   // SHT41 handles 400 kHz fine; shorter awake time

    // 3. Start the sensors.
    if (!sht41_init()) LOG.println("[Sensor] WARN: SHT41 init failed.");
    battery_init();

    // 4. Bring up the ESP-NOW radio (as a sensor).
    bool radio_ok = espnow_init(false);
    if (!radio_ok) LOG.println("[Sensor] WARN: ESP-NOW init failed.");

    // 5. Re-send anything stuck from a previous failed wake.
    if (radio_ok) retry_pending_payload();

    // 6. Take a fresh reading.
    SHT41_Data   sht = sht41_read();
    Battery_Data bat = battery_read();
    SensorReadings r;
    r.temperature     = sht.temperature;
    r.humidity        = sht.humidity;
    r.sht41_valid     = sht.valid;
    r.battery_voltage = bat.voltage;
    r.battery_mv      = bat.millivolts;
    r.battery_valid   = bat.valid;

    // A full power loss wipes the RTC counter back to 0. When that happens,
    // pull a fresh persistent base from NVS so seq still moves forward and the
    // backend doesn't drop our data as a false "replay". Normal deep-sleep
    // wakes keep the RTC value, so NVS is only touched after real power loss.
    if (g_sensor_seq == 0) {
        g_sensor_seq = seq_boot_base();
    }
    g_sensor_seq++;   // next packet gets a higher number than the last
    AchasPayload payload = payload_build(r,
                                         commission.house_id,
                                         identity_id(),
                                         commission.box_id,
                                         false,
                                         g_sensor_seq,
                                         g_last_awake_ms);   // previous wake's duration
    payload_print(payload);
    LOG.printf("  Room       : %s  (this box's location)\n",
                  commission.logical_name[0] ? commission.logical_name : "(unset)");

    // 7. Send it. If it wasn't delivered, save it to retry next wake.
    Downlink downlink;
    downlink.valid = false;
    if (radio_ok) {
        SendResult send = send_encrypted(payload);
        if (send.delivered) {
            storage_clear_pending();
            led_status_blink(2);   // 2 quick blinks = reading delivered
        } else {
            storage_save_pending(payload);
        }
        downlink = send.downlink;
    } else {
        storage_save_pending(payload);
    }

    // 8. Turn the WiFi/ESP-NOW radio OFF so BLE (and OTA) can use it.
    espnow_deinit();

    // 9. Act on any command the gateway replied with.
    handle_downlink(downlink);

    // 10. No BLE advertising here anymore: the fixed 5 s window dominated
    //     the awake time (~10.5 s -> ~2 s without it) and drained the
    //     battery every cycle. Live values are still reachable over BLE
    //     via the boot-button pairing path (hold 3 s at wake).

    // 11. Cut power to the SHT41 (P-channel MOSFET off) so it does not
    //     drain the battery during deep sleep.
    sht41_power_off();

    // 12. Go to sleep until the next cycle.
    deep_sleep_now();
}


// ==================================================================
//  GATEWAY MODE  (always on)
// ==================================================================
static void run_gateway_mode(const CommissionData& commission) {
    (void)commission;
    LOG.println("[Mode] GATEWAY mode");

    LOG.println("[Boot] Initializing hardware...");
    bool espnow_ok = tasks_init_hardware();
    LOG.printf("[Boot] ESP-NOW init: %s\n", espnow_ok ? "OK" : "FAIL");

    LOG.println("[Boot] Connecting to WiFi...");
    bool wifi_ok = wifi_connect();
    if (wifi_ok) {
        LOG.printf("[Boot] WiFi connected. IP=%s\n", wifi_get_ip().c_str());
        ntp_sync();
        LOG.println("[Boot] Connecting to MQTT broker...");
        if (mqtt_connect()) {
            LOG.println("[Boot] MQTT connected.");
            mqtt_publish_status("online");
        } else {
            LOG.println("[Boot] MQTT connect failed -- background retry.");
        }
    } else {
        LOG.println("[Boot] WiFi connect failed -- background retry.");
    }

    // Joining the AP above tore down ESP-NOW's send state (RX still works, but
    // esp_now_send() would return NOT_INIT, so ACKs/commands to sensors never
    // transmit -> sensors retry forever and never cache a channel). Re-init now
    // that WiFi is settled on its channel, BEFORE the RX task starts replying.
    espnow_gateway_reinit();

    // We do NOT advertise over BLE during normal gateway operation:
    // running BLE + WiFi at the same time on a single-core chip is
    // fragile, and the gateway doesn't need it. Pairing still works via
    // the boot-button path, which runs BEFORE WiFi starts.
    tasks_start();
    g_gateway_runtime = true;

    LOG.println("=============================================");
    LOG.println("   GATEWAY RUNTIME ACTIVE");
    LOG.println("=============================================");
}


// ==================================================================
//  setup()  -- runs once at boot
// ==================================================================
void setup() {
#if SERIAL_DEBUG
    Serial.begin(115200);
    delay(10);   // brief USB-CDC warm-up (dev only); skipped when SERIAL_DEBUG=0
#endif

    print_banner();
    print_wake_reason();

    identity_init();
    identity_print_banner();

    // Power LED: one short blink = "this device just powered up / woke".
    // (On a sensor, setup() runs on every wake, so this blinks each cycle.)
    led_init();
    led_power_pulse();

    // Make sure this chip has its own private key (generated once, on the
    // very first boot, and stored in NVS). This is what gives every device
    // a unique X.509 identity from a single shared firmware binary.
    device_cert_init();

    if (!commission_init()) {
        LOG.println("[Boot] FATAL: commission NVS unavailable.");
        deep_sleep_now();
        return;
    }

    CommissionData commission;
    commission_load(commission);
    commission_print(commission);

    handle_boot_button();

    if (commission.house_id == 0) {
        LOG.println("[Boot] Device is uncommissioned (house_id=0).");
        // PRE-PROD path: run the hardware self-test (so the manufacturer sees
        // module health from the LEDs), then open the app-free web portal.
        // BLE app pairing is still available by holding the button at boot.
        selftest_run();
        portal_provision_run();   // reboots into role on success; never returns
        return;
    }

    commission_autofill(commission);

    // Keep the gateway's own device id in sync; reject a sensor profile
    // that points at itself as the gateway.
    if (commission_is_gateway(commission)) {
        if (strcasecmp(commission.gateway_device_id, identity_id_hex()) != 0) {
            strncpy(commission.gateway_device_id, identity_id_hex(),
                    sizeof(commission.gateway_device_id) - 1);
            commission.gateway_device_id[sizeof(commission.gateway_device_id) - 1] = '\0';
            commission_save(commission);
        }
    } else {
        if (strcasecmp(commission.gateway_device_id, identity_id_hex()) == 0) {
            LOG.println("[Boot] Invalid profile: sensor points to itself as gateway.");
            portal_provision_run();   // PRE-PROD: re-provision via web portal
            return;
        }
    }

    char err[96];
    if (!commission_validate(commission, err, sizeof(err))) {
        LOG.printf("[Boot] Invalid commissioning data: %s\n", err);
        portal_provision_run();   // PRE-PROD: re-provision via web portal
        return;
    }

    // SAFETY NET: a GATEWAY needs its X.509 certificate to reach the broker
    // (mTLS). If it is "commissioned" (house_id set) but the certificate is
    // missing -- e.g. a previous pairing wrote the commission profile but the
    // cert write failed/was interrupted, or NVS lost it -- then connecting to
    // MQTT would fail forever and the unit would be stuck with no way out.
    // Instead, go BACK to BLE pairing so the app can re-issue the cert.
    // (Sensors don't talk to the broker, so they don't need this check.)
    if (commission_is_gateway(commission) && !device_cert_has_cert()) {
        LOG.println("[Boot] Gateway is commissioned but has NO certificate "
                       "-- re-provisioning via web portal so it can be re-issued.");
        portal_provision_run();   // PRE-PROD: re-provision via web portal
        return;
    }

    // Set up encryption from the per-house key.
    if (!crypto_init(commission.house_id) || !crypto_self_test()) {
        LOG.println("[Boot] FATAL: crypto init/self-test failed.");
        deep_sleep_now();
        return;
    }

    // Finally: be a gateway or a sensor.
    if (commission_is_gateway(commission)) run_gateway_mode(commission);
    else                                   run_sensor_mode(commission);
}


// ==================================================================
//  loop()  -- the gateway idles here; a sensor never reaches it
//  (run_sensor_mode always ends in deep sleep).
// ==================================================================
void loop() {
    if (g_gateway_runtime) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    delay(1000);
    deep_sleep_now();
}
