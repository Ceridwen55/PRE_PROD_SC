// core/wifi_policy.cpp  (GATEWAY)
// Same code as the sensor box. Connect -> verify scan auth mode -> fail
// closed if anything below WPA2-PSK was joined.

#include "wifi_policy.h"
#include "log.h"
#include <WiFi.h>


static bool is_strong_auth_mode(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA2_ENTERPRISE:
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return true;
        default:
            return false;
    }
}


static const char* auth_mode_name(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
        default:                        return "UNKNOWN";
    }
}


bool wifi_connect_secure(const char* ssid, const char* pass,
                         uint32_t timeout_ms) {

    if (ssid == nullptr || ssid[0] == '\0') {
        LOG.println("[WiFi] ERROR: SSID is empty.");
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(true, true);
    delay(100);

    LOG.printf("[WiFi] Connecting to SSID '%s' (WPA2 required)...\n", ssid);
    WiFi.begin(ssid, pass);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeout_ms) {
            LOG.println("[WiFi] ERROR: connect timeout.");
            WiFi.disconnect(true, true);
            return false;
        }
        delay(250);
    }
    LOG.printf("[WiFi] Linked. IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());

    int n = WiFi.scanNetworks(false, false, false, 200);
    wifi_auth_mode_t mode = WIFI_AUTH_OPEN;
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == ssid) {
            mode = WiFi.encryptionType(i);
            found = true;
            break;
        }
    }
    WiFi.scanDelete();

    if (!found) {
        LOG.println("[WiFi] ERROR: could not verify auth mode (SSID not in scan).");
        WiFi.disconnect(true, true);
        return false;
    }
    LOG.printf("[WiFi] Auth mode: %s\n", auth_mode_name(mode));
    if (!is_strong_auth_mode(mode)) {
        LOG.println("[WiFi] REJECT: network is weaker than WPA2-PSK.");
        WiFi.disconnect(true, true);
        return false;
    }
    return true;
}


void wifi_off() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
}
