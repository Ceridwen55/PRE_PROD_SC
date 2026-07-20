/*
  core/wifi_manager.h  (GATEWAY)
  Always-on WiFi for the gateway. The dongle SSID + password come from
  NVS commissioning (see commission.h). Auth mode is verified WPA2 or
  stronger by wifi_policy.cpp before any traffic is sent.

  Simple state machine:
    boot     -> wifi_connect()
    forever  -> wifi_ensure_connected() in the publisher task
*/

#pragma once

#include <Arduino.h>


// Connect to the dongle WiFi using credentials from commission NVS.
// Blocks until connected or timeout. Returns true on success.
// (Also runs the WPA2-or-better policy check via wifi_policy.cpp.)
bool wifi_connect();

bool   wifi_is_connected();
bool   wifi_ensure_connected();
String wifi_get_ip();
void   wifi_disconnect();


// Sync system clock from NTP servers and set the timezone. Must be
// called AFTER wifi_connect() succeeds. Returns true once time is
// plausibly correct (year > 2023).
bool ntp_sync();
