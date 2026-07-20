/* drivers/SHT41.h — SHT41 Temperature & Humidity Sensor Driver

Sensor: Sensirion SHT41-AD1F
Interface: I2C, address 0x44
Provides: temperature (°C) and relative humidity (%)

NOTE: This file is IDENTICAL to the sensor box version.
      Same hardware, same driver. */

#pragma once

#include <Arduino.h>
#include <Adafruit_SHT4x.h>


// Data returned by the sensor
struct SHT41_Data {
    float temperature;    // degrees Celsius
    float humidity;       // relative humidity, 0.0 - 100.0 %
    bool  valid;          // true = data is good, false = sensor error
};


// Start the sensor. Call once during setup.
// Also powers the SHT41 on via its P-channel MOSFET gate (sensor PCB).
bool sht41_init();

// Read temperature and humidity. Returns a struct with a 'valid' flag.
SHT41_Data sht41_read();

// Cut power to the SHT41 (gate HIGH = MOSFET off). Call before deep sleep
// on the sensor so the SHT41 does not drain the battery while asleep.
// On the gateway PCB the gate pin is unconnected, so this is a no-op.
void sht41_power_off();
