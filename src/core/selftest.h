/*
  ============================================================
   core/selftest.h  -- Power-On Self-Test (POST) for manufacturing
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Right after the manufacturer flashes a board, it is still
  "uncommissioned". On that first boot we run a quick hardware check so a
  bad module is obvious from the LEDs -- no serial cable needed on the
  line. (Adapted from the "Sensor Gateway V5" bench test.)

  WHAT IT CHECKS (Status LED = blue)
  ----------------------------------
    Test 1 : SHT41 over I2C   -> pass = 1 blink
    Test 2 : WiFi radio (scan)-> pass = 2 blinks
    Test 3 : BLE radio (scan) -> pass = 3 blinks
    Test 4 : Battery (>=3.5V) -> pass = 4 blinks (never locks)

  ON FAILURE (tests 1-3)
  ----------------------
  The Power LED blinks steadily and the Status LED repeats N blinks
  (N = failed test number), then the code HALTS. The bench operator reads
  N to know which module failed. Test 4 never halts (no/low battery is OK).

  WHERE IT IS USED
  ----------------
  main.cpp calls selftest_run() ONCE, only on an uncommissioned boot,
  before the provisioning portal opens. Commissioned units in the field
  do NOT run it (saves battery).
*/

#pragma once

// Run the full power-on self-test. Returns true if the critical modules
// (1-3) pass. On a hard failure it does NOT return -- it halts, blinking
// the failed test number forever so the operator sees the bad module.
bool selftest_run();
