/*
  ============================================================
   drivers/Battery.h  -- read the battery voltage
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Measures the battery voltage using the ESP32-C3's built-in ADC.
  A resistor divider drops the battery voltage into the ADC's safe
  range; we multiply it back up in software.

  WHERE IT IS USED
  ----------------
  Sensor PCB: the reading goes into the payload (battery_mv).
  Gateway PCB: no battery circuit -- the pin floats, the reading fails the
  range check, and the battery field is simply left empty.

  Wiring (sensor PCB):
      VBAT --- R16(1M) ---+--- R17(1M) --- IO0 (drain gate, see config.h)
                          |
                          +---> PIN_BATTERY_ADC   (see config.h)

  IO0 is pulled LOW only while measuring and floated (Hi-Z) otherwise, so
  the divider does not drain the battery between reads.
*/

#pragma once

#include <Arduino.h>


struct Battery_Data {
    float    voltage;     // Volts at VBAT
    uint16_t millivolts;  // Same value in millivolts (for the payload)
    bool     valid;       // true = reading is good
};


void         battery_init();
Battery_Data battery_read();
