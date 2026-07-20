/*
  ============================================================
   core/tasks.h  -- the GATEWAY's "always running" background jobs
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  The gateway never sleeps. It runs three small jobs (FreeRTOS tasks)
  side by side, forever:

    1. ESP-NOW Receiver -- listens for sensor packets, decrypts them,
                           replies (command or ACK), and queues the data.
    2. Sensor Reader    -- reads the gateway's OWN SHT41 + battery and
                           queues that too.
    3. Publisher        -- takes whatever is in the queue, wraps it in an
                           encrypted MQTT message, and sends it to the cloud.

  WHERE IT IS USED
  ----------------
  main.cpp (run_gateway_mode) calls tasks_init_hardware() then tasks_start().

  ORDER OF USE
  ------------
    1. tasks_init_hardware()  -> set up sensors, ESP-NOW, the queue, the watchdog
    2. tasks_start()          -> launch the three jobs above
*/

#pragma once

#include <Arduino.h>


bool   tasks_init_hardware();   // returns true if ESP-NOW came up OK
void   tasks_start();           // launch the three background jobs
String tasks_device_id();       // human-readable name, e.g. "STAYCOOL-0042-GW-A3F1"
