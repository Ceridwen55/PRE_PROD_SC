/*
  ============================================================
   core/identity.h  -- this chip's unique id (from its MAC)
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Every ESP32-C3 has a unique factory MAC address. We take the lower
  16 bits of it as this box's "device_id" (shown as 4 hex chars, e.g.
  "A3F1"). It identifies the box in logs, MQTT, and ESP-NOW replies.

  WHERE IT IS USED
  ----------------
  Almost everywhere: payloads, BLE name, MQTT client id, ESP-NOW
  command targeting. Call identity_init() once at boot first.
*/

#pragma once

#include <Arduino.h>
#include <stdint.h>

void        identity_init();
uint16_t    identity_id();
const char* identity_id_hex();
const char* identity_mac_str();
void        identity_print_banner();
