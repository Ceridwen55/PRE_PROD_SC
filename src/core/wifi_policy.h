/*
  core/wifi_policy.h  (GATEWAY)
  WiFi connect helper that ENFORCES WPA2-PSK or stronger. See the
  sensor box's wifi_policy.h for the rationale.
*/

#pragma once

#include <Arduino.h>
#include <stdint.h>


bool wifi_connect_secure(const char* ssid, const char* pass,
                         uint32_t timeout_ms);
void wifi_off();
