/*
  ============================================================
   core/payload.h  -- the tiny 13-byte "reading" packet
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Defines AchasPayload: the exact 13 bytes that travel over ESP-NOW
  from a sensor box to the gateway. Keeping it small + fixed-size means
  it fits easily in one ESP-NOW packet and is cheap to encrypt.

  WHERE IT IS USED
  ----------------
   * SENSOR  -> payload_build() makes one, then it is encrypted + sent.
   * GATEWAY -> payload_validate() checks a received one before use;
                payload_to_json() turns it into JSON for MQTT.

  The gateway also builds its OWN payload (FLAG_IS_GATEWAY set) so its
  local readings show up alongside the sensors'.
*/

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>
#include "config.h"


#define FLAG_BATTERY_LOW    (1 << 0)
#define FLAG_SENSOR_ERROR   (1 << 1)
#define FLAG_FRESH_OTA      (1 << 2)
#define FLAG_IS_GATEWAY     (1 << 3)


#define PAYLOAD_TA_INVALID    INT16_MIN
#define PAYLOAD_RH_INVALID    INT16_MIN
#define PAYLOAD_BAT_INVALID   ((uint16_t)0)


struct __attribute__((packed)) AchasPayload {
    uint8_t  version;
    uint16_t house_id;
    uint16_t device_id;
    uint8_t  box_id;
    int16_t  ta_x100;
    int16_t  rh_x100;
    uint16_t battery_mv;
    uint8_t  flags;
    uint32_t seq;        // anti-replay: a number that only ever goes UP
    uint16_t wake_ms;    // sensor's PREVIOUS wake duration (ms); 0 on gateway
};
// wake_ms is appended at the END, so byte offsets 0..16 are unchanged: a new
// 19-byte gateway still parses a legacy 17-byte payload correctly (wake_ms then
// reads 0 from the zeroed decrypt buffer). payload_validate() accepts both
// lengths so a rolling reflash never drops data. PAYLOAD_VERSION stays 5.
static_assert(sizeof(AchasPayload) == 19,
              "AchasPayload must be exactly 19 bytes on the wire (v5 + wake_ms)");


struct SensorReadings {
    float    temperature;
    float    humidity;
    bool     sht41_valid;
    float    battery_voltage;
    uint16_t battery_mv;
    bool     battery_valid;
};


// Build the wire-format struct from raw readings. Works for both
// runtime roles (gateway + sensor). `seq` is the anti-replay counter
// (caller keeps it increasing across sends).
AchasPayload payload_build(const SensorReadings& readings,
                           uint16_t house_id,
                           uint16_t device_id,
                           uint8_t  box_id,
                           bool     is_gateway,
                           uint32_t seq,
                           uint16_t wake_ms = 0);   // sensor's last wake ms (0 = gateway/unknown)


// Pretty-print a decoded payload to Serial. Debug only.
void payload_print(const AchasPayload& p);


// Sanity-check raw bytes received over ESP-NOW (size, version, range).
bool payload_validate(const uint8_t* data, size_t length);


// Convert a payload to compact JSON for the MQTT publish step.
// `rssi` / `snr` should be 0 for the gateway's own data (no radio hop).
String payload_to_json(const AchasPayload& payload, float rssi, float snr);
