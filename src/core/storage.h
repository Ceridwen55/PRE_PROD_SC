/*
  ============================================================
   core/storage.h -- "try again later" buffer for a failed send
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  If a sensor sends a reading but the gateway does not reply (so we
  don't know it arrived), we save that one reading into flash (NVS).
  On the next wake we try to send it again, so a reading isn't lost
  just because the gateway was briefly unreachable.

  WHERE IT IS USED
  ----------------
  main.cpp (run_sensor_mode + retry_pending_payload).

  FLOW
  ----
    1. send not delivered          -> storage_save_pending(payload)
    2. next wake                   -> storage_has_pending() == true
    3. retry the saved payload     -> if delivered, storage_clear_pending()
    4. after STORAGE_MAX_RETRIES   -> drop it so we don't loop forever.

  Note: this holds ONLY the retry slot. Commissioning data (house, box,
  wifi) lives in a separate NVS namespace -- see commission.h.
*/

#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "payload.h"


// Max retry attempts before discarding a stuck payload.
#define STORAGE_MAX_RETRIES  5


bool    storage_init();
void    storage_save_pending(const AchasPayload& payload);
bool    storage_has_pending();
bool    storage_load_pending(AchasPayload& out);
uint8_t storage_increment_retry();
uint8_t storage_get_retry_count();
void    storage_clear_pending();
