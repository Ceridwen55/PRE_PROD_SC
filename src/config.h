/*
  ============================================================
   config.h  -- all the "settings" of the firmware in one place
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Every tunable number lives here: pin numbers, timings, battery
  limits, WiFi/MQTT settings, etc. If you want to change behaviour,
  you almost always change a value HERE, not deep inside the code.

  WHERE IT IS USED
  ----------------
  Nearly every .cpp file does  #include "config.h"  so these names
  are available everywhere.

  HARDWARE: ESP32-C3 chip. Wireless link = ESP-NOW (built into WiFi).
  There is NO LoRa radio in this project anymore.
*/

#pragma once


// ------------------------------------------------------------------
// FIRMWARE / SCHEMA VERSIONS
// ------------------------------------------------------------------
#define FIRMWARE_VERSION         "4.4.2"   // serial debug: print wake_ms + room name per reading
#define PAYLOAD_VERSION          5         // v5 adds the anti-replay seq counter


// ==================================================================
//  PIN ASSIGNMENTS  (ESP32-C3)
// ==================================================================
//  ESP32-C3 has fewer pins than the old ESP32-S3, and a few pins are
//  "special" and should be left alone:
//     GPIO 2, 8, 9   -> strapping pins (affect how the chip boots)
//     GPIO 11..17    -> used by the internal flash (not usable)
//     GPIO 18, 19    -> USB data lines
//     GPIO 20, 21    -> default Serial (UART0) TX/RX for the monitor
//
//  The pins below avoid all of those, so they are safe on virtually
//  every ESP32-C3 board. If YOUR board routes things differently,
//  this is the ONE place you change it.
// ------------------------------------------------------------------

// --- I2C bus for the SHT41 temperature/humidity sensor ---
// Present on BOTH the gateway PCB and the sensor PCB (same pins).
#define I2C_PIN_SDA     4
#define I2C_PIN_SCL     5
#define I2C_ADDR_SHT41  0x44

// SHT41 power gate (SENSOR PCB only): a P-channel MOSFET feeds VCC to the
// SHT41. Gate LOW = MOSFET ON = sensor powered; gate HIGH = OFF. The
// firmware powers it on to measure, then cuts it before deep sleep so it
// does not drain the battery while asleep.
// On the GATEWAY PCB this pin is unconnected (the SHT41 is powered
// directly), so driving it there is a harmless no-op -- this is what lets
// one firmware serve both PCBs.
#define PIN_SHT41_POWER 3

// --- Battery voltage measurement (SENSOR PCB only) ---
// Must be an ADC1 pin (GPIO0..GPIO4 on the C3). ADC2 is unreliable
// while WiFi is on, and we use WiFi for ESP-NOW, so we stick to ADC1.
// The gateway PCB has no battery circuit -- there it reads a floating
// pin, battery_read() rejects the out-of-range value, and the battery
// field is simply left empty (gateway is mains/USB powered anyway).
#define PIN_BATTERY_ADC 1

// Battery "drain" gate (SENSOR PCB only). Almost certainly the MOSFET
// that connects the voltage divider only while measuring (so it does
// not bleed the battery continuously). NOTE: the firmware does NOT yet
// drive this pin -- Battery.cpp reads the ADC directly. Wire this up
// once its exact behaviour is confirmed.
#define PIN_BATTERY_DRAIN 0

// --- Status LEDs (both PCBs) ---
// Power LED  : one short blink at boot/wake ("device alive").
// Status LED : short blinks on an event (see drivers/Led.h).
// Driven only in SHORT pulses to save battery -- never left on.
#define PIN_LED_POWER   6
#define PIN_LED_STATUS  7

// LED drive polarity. Most indicator LEDs are wired GPIO -> resistor ->
// LED -> GND, which is "active HIGH" (GPIO HIGH = lit). If yours are wired
// from VCC instead, change this to LOW.
#define LED_ACTIVE_LEVEL  HIGH

// --- Pairing button (hold at boot to enter BLE pairing) ---
// On both PCBs. Not a strapping pin (C3 strapping pins are GPIO2/8/9),
// so holding it at power-on is safe and won't drop the chip into the
// USB download mode.
#define PIN_BUTTON      10


// ==================================================================
//  ESP-NOW  (the wireless link between sensor boxes and the gateway)
// ==================================================================
//  ESP-NOW only works between two chips on the SAME WiFi channel.
//  The gateway's channel is decided by the WiFi router/dongle it
//  joins, and a sleeping sensor cannot know it. So the sensor simply
//  sends its packet on EVERY channel in turn (1..13) and listens for
//  a reply on each one. See drivers/EspNow.h for the full story.
// ------------------------------------------------------------------
#define ESPNOW_CHANNEL_MIN          1
#define ESPNOW_CHANNEL_MAX          13

// How long the sensor listens for the gateway's reply on each channel
// before moving to the next. The gateway's ACK round-trip is only a few ms,
// so 60 ms has ample margin. This bounds the WORST case (gateway unreachable
// => full sweep) at 13 x 60 ms = ~0.8 s instead of ~2 s, and speeds the first
// cold-cache sweep that discovers the gateway's channel. After one successful
// reply the channel is cached (see EspNow.cpp), so the common case is a single
// ~60 ms hit regardless of this bound.
#define ESPNOW_LISTEN_PER_CHANNEL_MS  60


// ------------------------------------------------------------------
// TIMING
// ------------------------------------------------------------------
// Gateway: how often it reads its OWN sensors (it never sleeps).
#define SELF_SENSOR_INTERVAL_MS    900000UL    // 15 minutes

// ===== TESTING TOGGLE =============================================
// Set to 1 for a fast ~1-minute sensor wake cycle while bench-testing (so you
// don't wait 15 min between wakes), then set back to 0 for the real production
// reporting cycle BEFORE shipping. Flip this ONE line only.
#define SENSOR_FAST_TEST_CYCLE   0
// ==================================================================
#if SENSOR_FAST_TEST_CYCLE
  // TESTING: ~1-minute cycle, no jitter (predictable timing for the bench).
  #define DEEP_SLEEP_DURATION_US   60000000ULL   // 60 s base (~1 min cycle)
  #define DEEP_SLEEP_JITTER_US     0ULL          // jitter off while testing
#else
  // PRODUCTION: base 895 s + ~5 s awake => ~15 min reporting cycle.
  #define DEEP_SLEEP_DURATION_US   895000000ULL  // ~895 s base (~15 min cycle)
  // Anti-collision jitter: each cycle we add a RANDOM 0..this on top of the
  // base sleep, so multiple sensors in one house never stay phase-locked --
  // their ESP-NOW transmissions drift apart instead of hitting the gateway at
  // the same instant. Set to 0 to disable.
  #define DEEP_SLEEP_JITTER_US     30000000ULL   // up to +30 s random per cycle
#endif


// ------------------------------------------------------------------
// BATTERY  (voltage divider on the battery line -- SENSOR PCB only)
// ------------------------------------------------------------------
//  VBAT --- R16(1M) ---+--- R17(1M) --- IO0 (BatDrain)
//                      |
//                      +---> IO1 (PIN_BATTERY_ADC, "BatVoltage")
//
//  The bottom of the divider is NOT tied to GND -- it goes to IO0. The
//  firmware pulls IO0 LOW only while measuring (so the divider has a
//  ground path and IO1 reads VBatt/2), then lets IO0 float (Hi-Z) so the
//  divider stops draining the battery between reads.
//
//  Ratio = R17 / (R16 + R17) = 1M / (1M + 1M) = 0.5.
#define BAT_VOLTAGE_DIVIDER_RATIO  (1.0f / (1.0f + 1.0f))
#define BAT_VOLTAGE_FULL           4.20f
#define BAT_VOLTAGE_LOW            3.50f
#define BAT_VOLTAGE_CRITICAL       3.20f


// ------------------------------------------------------------------
// WIFI  (gateway uplink to the internet, always on)
// ------------------------------------------------------------------
// SSID + password come from NVS commissioning (see commission.h),
// not from this file. WPA2-PSK (or stronger) is enforced before any
// traffic is sent -- see wifi_policy.cpp.
#define WIFI_CONNECT_TIMEOUT_MS    30000
#define WIFI_RECONNECT_DELAY_MS    5000


// ------------------------------------------------------------------
// TIMEZONE / NTP  (so timestamps on the data are correct)
// ------------------------------------------------------------------
#define TIMEZONE_TZ_STRING   "SGT-8"
#define NTP_SERVER_PRIMARY   "sg.pool.ntp.org"
#define NTP_SERVER_SECONDARY "time.google.com"
#define NTP_SERVER_TERTIARY  "pool.ntp.org"


// ------------------------------------------------------------------
// MQTT  (mutual-TLS on port 8883 — broker-agnostic)
// ------------------------------------------------------------------
// MQTT_BROKER_HOST / MQTT_BROKER_PORT and this device's certificate +
// private key live in src/secrets.h (not committed). The broker's
// Root CA + OTA signing key live in src/certs.h.
#define MQTT_CLIENT_PREFIX       "achas-gw"
#define MQTT_TOPIC_BASE          "achas"
#define MQTT_KEEPALIVE_SEC       60
#define MQTT_RECONNECT_DELAY_MS  5000


// ------------------------------------------------------------------
// OTA -- over-the-air firmware update (download new firmware safely)
// ------------------------------------------------------------------
// Rules enforced in core/ota_manager.cpp and core/ota_remote.cpp:
//   * URL must start with "https://".
//   * Binary size must be <= OTA_MAX_FIRMWARE_BYTES.
//   * SHA-256 of the download must match before it is committed.
#define OTA_HTTP_TIMEOUT_MS    60000
#define OTA_WIFI_TIMEOUT_MS    30000
#define OTA_STREAM_CHUNK_SIZE  1024
#define OTA_MAX_FIRMWARE_BYTES (2 * 1024 * 1024)
#define OTA_CHECK_INTERVAL_MS  3600000UL      // 1 hour


// ------------------------------------------------------------------
// PRE-PRODUCTION: web-portal provisioning (no app)
// ------------------------------------------------------------------
// An uncommissioned box opens a Wi-Fi hotspot "ACHAS-PROV-<id>" with a
// web form; it then enrols itself against the Fleet API over HTTPS.
// FLEET_API_BASE must be the HTTPS front of the Fleet API (via Caddy on
// the VPS). The device uses setInsecure(), so any valid HTTPS endpoint
// works, but it MUST be https:// (token + house key travel here).
#define FLEET_API_BASE      "https://achas-iot.sustainablelivinglab.org:8443"
#define PORTAL_TIMEOUT_MS   600000UL    // 10 min portal window, then reboot & reopen


// ------------------------------------------------------------------
// BLE COMMISSIONING / IDENTIFICATION
// ------------------------------------------------------------------
// Pairing (where the device accepts its house/role config over BLE)
// is entered by holding PIN_BUTTON at boot.
#define BLE_PAIRING_TIMEOUT_MS    300000UL   // 5 min auto-exit
#define BLE_ADV_WAKE_WINDOW_MS    5000


// ------------------------------------------------------------------
// CRYPTO  (AES-128-GCM)
// ------------------------------------------------------------------
// The per-house key is generated by the backend server and written to
// each device over BLE during commissioning (stored in NVS). There is
// NO hardcoded key in the firmware.
#include "secrets.h"


// ------------------------------------------------------------------
// DATA QUEUE  (gateway: how many readings can wait to be published)
// ------------------------------------------------------------------
#define DATA_QUEUE_SIZE     20


// ------------------------------------------------------------------
// SERIAL DEBUG OUTPUT  (ONE switch for all firmware Serial prints)
// ------------------------------------------------------------------
//   1 = every firmware Serial print is emitted (development / bench testing).
//   0 = all firmware prints compiled to no-ops (production): the USB-CDC
//       warm-up delay and the pre-sleep flush are skipped too, so a sensor
//       wake is a touch shorter and draws a little less power.
// NOTE: this controls the FIRMWARE's own prints. The ESP-IDF framework's
// [I]/[E] log lines are separate -- set CORE_DEBUG_LEVEL=0 in platformio.ini
// for a fully silent production build.
#ifndef SERIAL_DEBUG
#define SERIAL_DEBUG 1
#endif
// Pulls in LOG (the print target chosen by SERIAL_DEBUG). Kept at the very
// end so SERIAL_DEBUG is already defined when log.h reads it.
#include "log.h"
