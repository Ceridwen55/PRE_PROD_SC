/*
  ============================================================
   log.h  -- ONE switch for all firmware Serial output
  ============================================================

  WHAT THIS IS
  ------------
  SERIAL_DEBUG (in config.h) decides at COMPILE TIME whether the firmware's
  own prints are emitted:

     SERIAL_DEBUG = 1  -> LOG forwards to the real USB-CDC Serial (dev/bench).
     SERIAL_DEBUG = 0  -> LOG discards everything (production): no USB traffic,
                          no blocking, no pre-sleep flush cost -> shorter wake.

  HOW IT IS USED
  --------------
  Every ".print / .printf / .println" in the firmware goes through LOG instead
  of Serial. (Serial.begin / Serial.flush live only in main.cpp and are gated
  there directly by SERIAL_DEBUG.)

  NOTE: this controls the FIRMWARE's prints. The ESP-IDF framework's own
  [I]/[E] log lines are separate -- lower CORE_DEBUG_LEVEL in platformio.ini
  to silence those for a fully quiet production build.
*/
#pragma once

#include <Arduino.h>
#include "config.h"   // for SERIAL_DEBUG (pragma-once safe; config.h includes us)

#if SERIAL_DEBUG
  // Development: LOG is a reference to the real Serial, used unchanged.
  extern Print& LOG;
#else
  // Production: a Print sink that throws every byte away. print/println/printf
  // still compile (they come from Print), but write() is a no-op.
  class NullSerial : public Print {
  public:
      size_t write(uint8_t) override                  { return 1; }
      size_t write(const uint8_t*, size_t n) override { return n; }
  };
  extern NullSerial LOG;
#endif
