/*
  core/ble_pairing.h
  BLE identity + commissioning service shared by unified firmware.

  - gateway mode uses ble_advertise_forever()
  - sensor mode uses ble_advertise_window() + ble_shutdown() before sleep
*/

#pragma once

#include <Arduino.h>
#include <stdint.h>


struct BleSnapshot {
    float temperature_c;     // NaN means "no reading"
    float humidity_pct;
    uint16_t battery_mv;
    uint32_t uptime_s;
    int8_t   last_rssi;      // RSSI of the most recent ESP-NOW RX (0 on sensors)
};


bool ble_init();
void ble_set_snapshot(const BleSnapshot& s);


// Start advertising and keep it on (gateway mode).
void ble_advertise_forever();

// Start advertising for a fixed window (sensor wake cycle).
void ble_advertise_window(uint32_t duration_ms);

// Free BLE memory, used before deep sleep.
void ble_shutdown();

// Time-limited pairing window with the commission-write characteristic
// enabled. Returns when a valid commission JSON arrives (the device
// reboots automatically) or after timeout_ms.
void ble_pairing_run(uint32_t timeout_ms = 0);
