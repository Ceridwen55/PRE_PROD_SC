/*
  ============================================================
   core/seq_counter.h  -- anti-replay sequence that survives reboots
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Every reading carries a "seq" number. The backend (Node-RED) DROPS any
  message whose seq is <= the last seq it already saw for that device --
  that is how it rejects replayed packets. So seq MUST keep going UP for
  the whole life of the device, INCLUDING across reboots and power loss.

  THE PROBLEM THIS SOLVES
  -----------------------
  A plain counter in RAM resets to 0 on every reboot (OTA, crash, power
  blip). After that, the device's seq would be LOWER than what the backend
  already saw, so the backend would silently drop all its real data as a
  false "replay" -- sometimes for hours, sometimes forever.

  HOW IT WORKS
  ------------
  We keep a small "boot counter" in NVS and bump it by one whenever the RAM
  counter has been lost. We mix it into the high half of the 32-bit seq:

        seq = [ 16-bit boot count ][ 16-bit per-boot message counter ]

  Because the boot count only ever increases, the seq always moves forward
  across reboots. NVS is written at most ONCE per boot (cheap, low wear).

  LIMITS (both are generous in practice)
  --------------------------------------
    * up to 65535 reboots over the device's life,
    * up to 65535 messages between reboots (~91 days at a 2-min interval).

  WHERE IT IS USED
  ----------------
    * gateway own data : core/tasks.cpp  (self_seq starts at seq_boot_base())
    * sensor data      : main.cpp        (only when the RTC counter was wiped
                                          by a full power loss)
*/

#pragma once

#include <stdint.h>


// Bump the persistent boot counter and return a fresh base for the message
// counter, i.e. (boot_count << 16). Call this ONCE per boot for an
// always-on gateway, or only after a power loss for a deep-sleeping sensor.
uint32_t seq_boot_base();
