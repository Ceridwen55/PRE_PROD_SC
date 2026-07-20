// ============================================================
//  core/tasks.cpp  (GATEWAY)  -- the three always-running jobs
// ============================================================
//  See tasks.h for the overview. Reading order of this file:
//    1. shared state         (the queue every job talks through)
//    2. on_mqtt_command()    (what to do when the cloud sends a command)
//    3. task_espnow_receiver (job 1: receive sensor data)
//    4. task_sensor_reader   (job 2: read our own sensors)
//    5. task_publisher       (job 3: send everything to MQTT)
//    6. tasks_init_hardware  + tasks_start  (setup, called from main.cpp)

#include "tasks.h"
#include "log.h"

#include <Wire.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "config.h"
#include "identity.h"
#include "commission.h"
#include "crypto.h"
#include "payload.h"
#include "drivers/Battery.h"
#include "drivers/EspNow.h"
#include "drivers/SHT41.h"
#include "drivers/Led.h"
#include "mqtt_handler.h"
#include "ota_manager.h"
#include "seq_counter.h"
#include "wifi_manager.h"


// ==================================================================
// 1. Shared state
// ==================================================================
// One reading (from a sensor box OR from the gateway itself) waiting
// to be published. The three jobs hand data to each other through
// g_data_queue.
struct QueueItem {
    AchasPayload payload;
    float        rssi;     // signal strength (0 for the gateway's own data)
    float        snr;      // not used by ESP-NOW -> always 0
};

static QueueHandle_t g_data_queue    = nullptr;
static bool          g_espnow_ready  = false;
static uint16_t      g_our_house_id  = 0;

static const uint32_t TASK_WDT_TIMEOUT_S = 60;


// ==================================================================
// Human-readable device tag used in logs.
// ==================================================================
String tasks_device_id() {
    char id[28];
    snprintf(id, sizeof(id), "STAYCOOL-%04u-GW-%s", g_our_house_id, identity_id_hex());
    return String(id);
}


// ==================================================================
// 2. MQTT command handler
//    Called by mqtt_handler.cpp for every command message.
//    target_box == 0 -> the gateway itself.  1..3 -> a sensor box.
// ==================================================================
static void on_mqtt_command(const String& cmd,
                            int target_box,
                            const String& args_json) {

    LOG.printf("[Command] cmd=%s target_box=%d\n", cmd.c_str(), target_box);

    // ---------- Command for the GATEWAY itself ----------
    if (target_box == 0) {
        if (cmd == "OTA") {
            // Pull "url" and "sha256" out of the JSON args.
            int us = args_json.indexOf("\"url\"");
            int hs = args_json.indexOf("\"sha256\"");
            if (us < 0 || hs < 0) {
                LOG.println("[Command] OTA missing url/sha256.");
                mqtt_publish_status("ota_failed:missing_args");
                return;
            }
            int uq1 = args_json.indexOf('"', args_json.indexOf(':', us));
            int uq2 = args_json.indexOf('"', uq1 + 1);
            int hq1 = args_json.indexOf('"', args_json.indexOf(':', hs));
            int hq2 = args_json.indexOf('"', hq1 + 1);
            if (uq1<0||uq2<0||hq1<0||hq2<0) { mqtt_publish_status("ota_failed:parse"); return; }
            String url    = args_json.substring(uq1+1, uq2);
            String sha256 = args_json.substring(hq1+1, hq2);

            // The signature is fetched by the device from "<url>.sig" --
            // the MQTT command only needs {url, sha256}.
            mqtt_publish_status("ota_started");
            if (!ota_self_update_https(url, sha256)) mqtt_publish_status("ota_failed");

        } else if (cmd == "REBOOT") {
            mqtt_publish_status("rebooting");
            delay(500);
            ESP.restart();
        } else if (cmd == "PING") {
            mqtt_publish_status("pong");
        } else if (cmd == "PAIR") {
            mqtt_publish_status("pairing_via_button_only");
        } else {
            LOG.printf("[Command] Unknown gateway cmd: %s\n", cmd.c_str());
        }
        return;
    }

    // ---------- Command for a SENSOR box (relayed over ESP-NOW) ----------
    // The MQTT message must include "target_dev" = the sensor's 4-hex
    // device id. We don't send it now -- we QUEUE it, and it goes out
    // the moment that sensor next checks in (see task_espnow_receiver).
    int td = args_json.indexOf("\"target_dev\"");
    if (td < 0) {
        LOG.println("[Command] relay missing target_dev.");
        return;
    }
    int dq1 = args_json.indexOf('"', args_json.indexOf(':', td));
    int dq2 = args_json.indexOf('"', dq1 + 1);
    if (dq1<0||dq2<0) return;
    String dev_hex = args_json.substring(dq1+1, dq2);

    if (cmd == "OTA") {
        int us = args_json.indexOf("\"url\"");
        int hs = args_json.indexOf("\"sha256\"");
        if (us<0||hs<0) return;
        int uq1 = args_json.indexOf('"', args_json.indexOf(':', us));
        int uq2 = args_json.indexOf('"', uq1 + 1);
        int hq1 = args_json.indexOf('"', args_json.indexOf(':', hs));
        int hq2 = args_json.indexOf('"', hq1 + 1);
        if (uq1<0||uq2<0||hq1<0||hq2<0) return;
        String url    = args_json.substring(uq1+1, uq2);
        String sha256 = args_json.substring(hq1+1, hq2);

        // No "sig" here -- the sensor downloads "<url>.sig" over HTTPS.
        ota_send_to_sensor(dev_hex, url, sha256);
    } else if (cmd == "PING")   ota_relay_simple(dev_hex, "PING");
    else if (cmd == "REBOOT")   ota_relay_simple(dev_hex, "REBOOT");
    else if (cmd == "PAIR")     ota_relay_simple(dev_hex, "PAIR");
    else LOG.printf("[Command] Unknown sensor cmd: %s\n", cmd.c_str());
}


// ==================================================================
// 3. Job 1: ESP-NOW Receiver
//    Waits for sensor packets, checks them, replies, queues the data.
// ==================================================================
static void task_espnow_receiver(void* /*param*/) {

    LOG.println("[RX] ESP-NOW receiver task started.");
    esp_task_wdt_add(nullptr);

    uint32_t cycle = 0, ok = 0, bad_schema = 0, wrong_house = 0, auth_fail = 0;

    while (true) {
        esp_task_wdt_reset();
        cycle++;
        if (cycle % 60 == 0) {
            LOG.printf("[RX] heartbeat | ok=%lu schema=%lu house=%lu auth=%lu\n",
                          ok, bad_schema, wrong_house, auth_fail);
        }

        // Wait up to 1 s for the next packet.
        EspNowPacket pkt = espnow_receive(1000);
        if (!pkt.valid) continue;

        // Step RX-1: decrypt. Zero the buffer first so a legacy 17-byte payload
        // (sensor not yet reflashed with wake_ms) leaves the trailing wake_ms
        // field reading 0 instead of stack garbage.
        uint8_t plain[64] = {0};
        size_t  plain_len = crypto_decrypt(pkt.data, pkt.length, plain, sizeof(plain));
        if (plain_len == 0) {
            auth_fail++;
            LOG.println("[RX] decrypt failed -- dropping.");
            continue;
        }

        // Step RX-2: check the layout is a valid reading.
        if (!payload_validate(plain, plain_len)) { bad_schema++; continue; }
        const AchasPayload* p = reinterpret_cast<const AchasPayload*>(plain);

        // Step RX-3: ignore readings from a different house.
        if (p->house_id != g_our_house_id) {
            wrong_house++;
            LOG.printf("[RX] foreign house %u (we are %u) -- drop\n",
                          p->house_id, g_our_house_id);
            continue;
        }

        ok++;
        LOG.printf("[RX] >>> VALID from device %04X (box %u) RSSI=%d  total=%lu\n",
                      p->device_id, p->box_id, pkt.rssi, ok);
        payload_print(*p);

        // Step RX-4: reply so the sensor knows it was heard. Send a real
        // command if one is waiting for this box, otherwise a plain ACK.
        // A real command is AUTHENTICATED: we append an HMAC tag over
        // (this reading's seq || command) keyed by the house key, so a
        // bystander in radio range cannot forge or replay REBOOT/PAIR/OTA.
        // A plain ACK needs no tag (it triggers no action on the sensor).
        String reply;
        if (espnow_take_pending(p->device_id, (uint16_t)(p->seq >> 16), reply)) {
            char tag[DOWNLINK_TAG_HEX_LEN + 1];
            if (crypto_downlink_tag(p->seq, reply.c_str(), tag, sizeof(tag))) {
                reply += "|";
                reply += tag;
            } else {
                LOG.println("[RX] WARN: could not sign command -- dropping it.");
                reply = "";   // fall through to a plain ACK below
            }
        }
        if (reply.length() == 0) {
            char ack[24];
            snprintf(ack, sizeof(ack), "ACHAS|%04X|ACK", p->device_id);
            reply = ack;
        }
        espnow_reply(pkt.mac, reply);

        // Step RX-5: hand the reading to the publisher job.
        QueueItem item;
        item.payload = *p;
        item.rssi    = (float)pkt.rssi;
        item.snr     = 0.0f;
        if (xQueueSend(g_data_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
            LOG.println("[RX] queue full -- dropping.");
        }
    }
}


// ==================================================================
// 4. Job 2: read the gateway's OWN SHT41 + battery
// ==================================================================
static void task_sensor_reader(void* /*param*/) {

    LOG.println("[Self] Sensor reader task started.");
    esp_task_wdt_add(nullptr);

    // Anti-replay counter for the gateway's own data. Start from a persistent
    // per-boot base so it never goes backwards after a reboot (otherwise the
    // backend would drop our data as a false "replay"). See seq_counter.h.
    uint32_t self_seq = seq_boot_base();

    // Cache our own room name (NVS) so each reading can print it for debug.
    CommissionData self_cd;
    commission_load(self_cd);

    while (true) {
        esp_task_wdt_reset();

        SHT41_Data   sht = sht41_read();
        Battery_Data bat = battery_read();

        SensorReadings r;
        r.temperature     = sht.temperature;
        r.humidity        = sht.humidity;
        r.sht41_valid     = sht.valid;
        r.battery_voltage = bat.voltage;
        r.battery_mv      = bat.millivolts;
        r.battery_valid   = bat.valid;

        AchasPayload p = payload_build(r, g_our_house_id, identity_id(), 0, true, ++self_seq);
        payload_print(p);
        LOG.printf("  Room       : %s  (gateway location)\n",
                      self_cd.logical_name[0] ? self_cd.logical_name : "(unset)");

        QueueItem item{};
        item.payload = p;
        item.rssi    = 0.0f;
        item.snr     = 0.0f;
        if (xQueueSend(g_data_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
            LOG.println("[Self] queue full -- dropping own data.");
        }

        // Sleep in small steps so we can keep petting the watchdog.
        uint32_t left = SELF_SENSOR_INTERVAL_MS;
        const uint32_t step = 10000;
        while (left > 0) {
            uint32_t d = (left > step) ? step : left;
            vTaskDelay(pdMS_TO_TICKS(d));
            left -= d;
            esp_task_wdt_reset();
        }
    }
}


// ==================================================================
// 5. Job 3: drain the queue and publish to MQTT
// ==================================================================
static void task_publisher(void* /*param*/) {

    LOG.println("[Pub] Publisher task started.");
    esp_task_wdt_add(nullptr);

    while (true) {
        esp_task_wdt_reset();

        if (!wifi_ensure_connected()) {
            LOG.println("[Pub] WiFi down -- retrying.");
            vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));
            continue;
        }
        if (!mqtt_ensure_connected()) {
            LOG.println("[Pub] MQTT down -- retrying.");
            vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
            continue;
        }
        mqtt_loop();

        QueueItem item;
        while (xQueueReceive(g_data_queue, &item, 0) == pdTRUE) {
            if (mqtt_publish_data(item.payload, item.rssi, item.snr)) {
                led_status_blink(3);   // 3 blinks = published to MQTT
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            mqtt_loop();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


// ==================================================================
// 6. Setup
// ==================================================================
bool tasks_init_hardware() {

    // Remember our house id so the RX job can filter packets quickly.
    CommissionData cd;
    commission_load(cd);
    g_our_house_id = cd.house_id;

    // Start the I2C bus (the sensor is always powered now).
    Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL);
    Wire.setTimeOut(50);

    sht41_init();
    battery_init();

    // Bring up the ESP-NOW link (as the gateway).
    g_espnow_ready = espnow_init(true);
    if (!g_espnow_ready) LOG.println("[Init] ERROR: ESP-NOW init failed.");

    // The queue the three jobs share.
    g_data_queue = xQueueCreate(DATA_QUEUE_SIZE, sizeof(QueueItem));
    if (g_data_queue == nullptr) {
        LOG.println("[Init] ERROR: data queue alloc failed.");
        return false;
    }

    // Watchdog: reboot if any task gets stuck for 60 s.
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms     = TASK_WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;
    cfg.trigger_panic  = true;
    esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(nullptr);

    mqtt_set_command_callback(on_mqtt_command);
    return g_espnow_ready;
}


void tasks_start() {
    LOG.println("[Tasks] Starting the three background jobs...");
    xTaskCreate(task_espnow_receiver, "espnow_rx",     6144, nullptr, 3, nullptr);
    xTaskCreate(task_sensor_reader,   "sensor_self",   4096, nullptr, 1, nullptr);
    xTaskCreate(task_publisher,       "publisher",     8192, nullptr, 2, nullptr);
    LOG.println("[Tasks] All jobs started.");
}
