// core/wifi_manager.cpp  (GATEWAY)
//
// Thin wrapper that pulls SSID + password from NVS commissioning and
// then delegates to wifi_policy_connect_secure() so the WPA2 check is
// applied uniformly across the firmware (gateway + sensor box).

#include "wifi_manager.h"
#include "log.h"
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "commission.h"
#include "wifi_policy.h"


bool wifi_connect() {

    if (WiFi.status() == WL_CONNECTED) return true;

    CommissionData cd;
    commission_load(cd);
    if (cd.wifi_ssid[0] == '\0') {
        LOG.println("[WiFi] ERROR: no SSID in NVS or secrets default.");
        return false;
    }

    return wifi_connect_secure(cd.wifi_ssid, cd.wifi_pass,
                               WIFI_CONNECT_TIMEOUT_MS);
}


bool wifi_is_connected() {
    return (WiFi.status() == WL_CONNECTED);
}


bool wifi_ensure_connected() {

    if (WiFi.status() == WL_CONNECTED) return true;

    LOG.println("[WiFi] link down -- attempting reconnect.");
    WiFi.disconnect(true);
    delay(1000);

    bool ok = wifi_connect();
    if (ok) ntp_sync();
    return ok;
}


String wifi_get_ip() {
    if (WiFi.status() != WL_CONNECTED) return "0.0.0.0";
    return WiFi.localIP().toString();
}


void wifi_disconnect() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    LOG.println("[WiFi] Disconnected and turned off.");
}


bool ntp_sync() {

    LOG.printf("[NTP] Syncing time from %s (TZ=%s)...\n",
                  NTP_SERVER_PRIMARY, TIMEZONE_TZ_STRING);

    configTzTime(TIMEZONE_TZ_STRING,
                 NTP_SERVER_PRIMARY,
                 NTP_SERVER_SECONDARY,
                 NTP_SERVER_TERTIARY);

    const uint32_t timeout_ms = 5000;
    uint32_t start = millis();
    struct tm tinfo;
    while (!getLocalTime(&tinfo, 200)) {
        if (millis() - start > timeout_ms) {
            LOG.println("[NTP] WARNING: first sync timed out (will retry in BG).");
            return false;
        }
        LOG.print(".");
    }

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S SGT", &tinfo);
    LOG.println();
    LOG.printf("[NTP] Time synced: %s\n", buf);
    return true;
}
