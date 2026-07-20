/*
  ============================================================
   core/ota_manager.h  (GATEWAY)  -- firmware update control
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Two ways firmware gets updated:

   1. SELF-UPDATE: the gateway downloads new firmware for ITSELF over
      HTTPS, checks its SHA-256, and reboots into it.

   2. RELAY: the gateway tells a SENSOR box to update itself. It does
      NOT contact the sensor directly here -- it QUEUES a command
      (via drivers/EspNow.h). The command is delivered the next time
      that sensor checks in, and the sensor then does its own HTTPS
      download (see core/ota_remote.cpp).

  WHERE IT IS USED
  ----------------
  core/tasks.cpp calls these from the MQTT command handler.

  SAFETY RULES (checked before anything happens)
  -----------------------------------------------
   * URL must start with "https://".
   * SHA-256 must be 64 lowercase hex characters.
*/

#pragma once

#include <Arduino.h>


// Self-OTA. The base64 RSA signature is fetched over HTTPS from
// "<url>.sig" and checked against the OTA public key in certs.h. On
// success the gateway reboots (never returns). On any failure -- including
// a bad signature -- it returns false and the OLD firmware keeps running.
bool ota_self_update_https(const String& url, const String& sha256_hex);


// Queue an OTA command for a sensor box (by 4-hex device id, e.g. "A3F1").
// The signature is NOT relayed (it does not fit in one ESP-NOW frame); the
// sensor fetches "<url>.sig" itself over HTTPS. Returns true if accepted.
bool ota_send_to_sensor(const String& device_id_hex,
                        const String& url,
                        const String& sha256_hex);


// Queue a simple one-word command for a sensor box: "PING"/"REBOOT"/"PAIR".
bool ota_relay_simple(const String& device_id_hex, const char* cmd);
