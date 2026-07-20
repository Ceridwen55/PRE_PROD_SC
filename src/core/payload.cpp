// core/payload.cpp  (GATEWAY) -- v4 builder + decoder + validator + JSON.

#include "payload.h"
#include "log.h"
#include "commission.h"   // gateway reads its own room name (logical_name) for "loc"
#include <math.h>
#include <time.h>


// ==================================================================
// payload_build -- gateway's own sensor readings -> wire struct
// ==================================================================
AchasPayload payload_build(const SensorReadings& r,
                           uint16_t house_id,
                           uint16_t device_id,
                           uint8_t  box_id,
                           bool     is_gateway,
                           uint32_t seq,
                           uint16_t wake_ms) {

    AchasPayload p;
    memset(&p, 0, sizeof(AchasPayload));

    p.version   = PAYLOAD_VERSION;
    p.house_id  = house_id;
    p.device_id = device_id;
    p.box_id    = box_id;
    p.seq       = seq;
    p.wake_ms   = wake_ms;
    p.flags     = is_gateway ? FLAG_IS_GATEWAY : 0;

    if (r.sht41_valid) {
        p.ta_x100 = (int16_t)lroundf(r.temperature * 100.0f);
        p.rh_x100 = (int16_t)lroundf(r.humidity    * 100.0f);
    } else {
        p.ta_x100 = PAYLOAD_TA_INVALID;
        p.rh_x100 = PAYLOAD_RH_INVALID;
        p.flags  |= FLAG_SENSOR_ERROR;
    }

    if (r.battery_valid) {
        p.battery_mv = r.battery_mv;
        if (r.battery_voltage > 0.1f &&
            r.battery_voltage < BAT_VOLTAGE_LOW) {
            p.flags |= FLAG_BATTERY_LOW;
        }
    } else {
        p.battery_mv = PAYLOAD_BAT_INVALID;
        p.flags     |= FLAG_SENSOR_ERROR;
    }
    return p;
}


// ==================================================================
// payload_print -- decoded readable lines for serial debug
// ==================================================================
void payload_print(const AchasPayload& p) {

    LOG.println("---------- Payload v4 (decoded) -----");
    LOG.printf("  Version    : %d\n", p.version);
    LOG.printf("  House ID   : %u\n", p.house_id);
    LOG.printf("  Device ID  : %04X  (from MAC)\n", p.device_id);
    LOG.printf("  Box ID     : %u  (%s)\n", p.box_id,
                  (p.flags & FLAG_IS_GATEWAY) ? "gateway" : "sensor");
    LOG.printf("  Seq        : %lu  (anti-replay)\n", (unsigned long)p.seq);
    LOG.printf("  Wake time  : %u ms  (previous wake)\n", p.wake_ms);

    if (p.ta_x100 == PAYLOAD_TA_INVALID) LOG.println("  Temp       : [INVALID]");
    else                                  LOG.printf ("  Temp       : %.2f C\n",  p.ta_x100 / 100.0f);
    if (p.rh_x100 == PAYLOAD_RH_INVALID) LOG.println("  Humidity   : [INVALID]");
    else                                  LOG.printf ("  Humidity   : %.2f %%\n", p.rh_x100 / 100.0f);

    if (p.battery_mv == PAYLOAD_BAT_INVALID) {
        LOG.println("  Battery    : [INVALID]");
    } else {
        float v   = p.battery_mv / 1000.0f;
        float pct = (v - BAT_VOLTAGE_CRITICAL) /
                    (BAT_VOLTAGE_FULL - BAT_VOLTAGE_CRITICAL) * 100.0f;
        if (pct < 0.0f)   pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        LOG.printf("  Battery    : %.3f V  (~%.0f%%)\n", v, pct);
    }

    LOG.printf("  Flags      : 0x%02X  [bat_low=%d sens_err=%d "
                  "fresh_ota=%d gw=%d]\n",
                  p.flags,
                  (p.flags & FLAG_BATTERY_LOW)  ? 1 : 0,
                  (p.flags & FLAG_SENSOR_ERROR) ? 1 : 0,
                  (p.flags & FLAG_FRESH_OTA)    ? 1 : 0,
                  (p.flags & FLAG_IS_GATEWAY)   ? 1 : 0);
    LOG.printf("  Wire size  : %d bytes\n", (int)sizeof(AchasPayload));
    LOG.println("-------------------------------------");
}


// ==================================================================
// payload_validate -- raw-byte sanity check before casting
// ==================================================================
bool payload_validate(const uint8_t* data, size_t length) {

    // Accept BOTH the new 19-byte payload (v5 + wake_ms) and the legacy
    // 17-byte one (a sensor not yet reflashed). For a 17-byte payload the
    // caller's decrypt buffer is zeroed, so the trailing wake_ms reads 0.
    // This lets a fleet be reflashed one box at a time without dropping data.
    if (length != sizeof(AchasPayload) && length != sizeof(AchasPayload) - 2) {
        LOG.printf("[Payload] Bad size: got %d, expected %d or %d\n",
                      (int)length, (int)sizeof(AchasPayload),
                      (int)sizeof(AchasPayload) - 2);
        return false;
    }

    const AchasPayload* p = reinterpret_cast<const AchasPayload*>(data);

    if (p->version != PAYLOAD_VERSION) {
        LOG.printf("[Payload] Bad version: got %d, expected %d\n",
                      p->version, PAYLOAD_VERSION);
        return false;
    }
    if (p->house_id == 0) {
        LOG.printf("[Payload] Bad house_id: 0 (sender uncommissioned)\n");
        return false;
    }
    if (p->box_id > 3) {
        LOG.printf("[Payload] Bad box_id: %d\n", p->box_id);
        return false;
    }
    return true;
}


// ==================================================================
// payload_to_json -- compact JSON for the MQTT envelope
//
//   ts    ISO 8601 timestamp in SGT
//   u     unix epoch seconds
//   h     house_id
//   d     device_id (4 hex chars)
//   b     box_id
//   v     payload version
//   ta    temperature (deg C)   -- null on sensor error
//   rh    humidity (%)          -- null on sensor error
//   bat   battery (V)           -- null on read fail
//   f     flags byte
//   bl    battery low (0/1)
//   se    sensor error (0/1)
//   fo    fresh OTA (0/1)
//   gw    is_gateway (0/1)
//   rssi  ESP-NOW RSSI (dBm)    -- null for box 0 (gateway's own data)
//   snr   not used by ESP-NOW   -- always null
//   dn    derived device name    ("H<house>-GW" / "H<house>-S<box>")
//   rt    role string            ("gateway" / "sensor")
// ==================================================================
String payload_to_json(const AchasPayload& p, float rssi, float snr) {

    JsonDocument doc;

    time_t now_unix = time(nullptr);
    if (now_unix > 1700000000) {
        struct tm tm_sgt;
        localtime_r(&now_unix, &tm_sgt);
        char iso[32];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S+08:00", &tm_sgt);
        doc["ts"] = iso;
        doc["u"]  = (uint32_t)now_unix;
    } else {
        doc["ts"] = (char*)nullptr;
        doc["u"]  = (char*)nullptr;
    }

    doc["h"] = p.house_id;
    char dhex[8];
    snprintf(dhex, sizeof(dhex), "%04X", p.device_id);
    doc["d"] = dhex;
    doc["b"]   = p.box_id;
    doc["v"]   = p.version;
    doc["seq"] = p.seq;        // anti-replay: backend rejects non-increasing seq
    doc["wt"]  = p.wake_ms;    // sensor's previous wake duration (ms); 0 on gateway

    if (p.ta_x100 == PAYLOAD_TA_INVALID) doc["ta"] = (char*)nullptr;
    else                                  doc["ta"] = p.ta_x100 / 100.0f;
    if (p.rh_x100 == PAYLOAD_RH_INVALID) doc["rh"] = (char*)nullptr;
    else                                  doc["rh"] = p.rh_x100 / 100.0f;

    if (p.battery_mv == PAYLOAD_BAT_INVALID) doc["bat"] = (char*)nullptr;
    else                                      doc["bat"] = p.battery_mv / 1000.0f;
    doc["f"]  = p.flags;
    doc["bl"] = (p.flags & FLAG_BATTERY_LOW)  ? 1 : 0;
    doc["se"] = (p.flags & FLAG_SENSOR_ERROR) ? 1 : 0;
    doc["fo"] = (p.flags & FLAG_FRESH_OTA)    ? 1 : 0;
    doc["gw"] = (p.flags & FLAG_IS_GATEWAY)   ? 1 : 0;

    char device_name[24];
    if (p.flags & FLAG_IS_GATEWAY) {
        snprintf(device_name, sizeof(device_name), "H%u-GW", p.house_id);
        doc["rt"] = "gateway";
    } else {
        snprintf(device_name, sizeof(device_name), "H%u-S%u", p.house_id, p.box_id);
        doc["rt"] = "sensor";
    }
    doc["dn"] = device_name;

    // Location / room name ("loc"). The GATEWAY attaches its own room here
    // (read from NVS). A sensor's room is not in the 19-byte wire payload, so
    // for sensor readings "loc" is null and Node-RED fills it from the enrol
    // mapping (device_id -> logical_name sent to the Fleet API at provisioning).
    if (p.flags & FLAG_IS_GATEWAY) {
        CommissionData cd;
        commission_load(cd);
        doc["loc"] = cd.logical_name[0] ? cd.logical_name : (const char*)nullptr;
    } else {
        doc["loc"] = (char*)nullptr;
    }

    if (p.flags & FLAG_IS_GATEWAY) {
        doc["rssi"] = (char*)nullptr;
        doc["snr"]  = (char*)nullptr;
    } else {
        doc["rssi"] = rssi;
        doc["snr"]  = snr;
    }

    String json;
    serializeJson(doc, json);
    return json;
}
