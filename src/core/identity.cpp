// core/identity.cpp  (GATEWAY) -- mirror of the sensor box identity module.

#include "identity.h"
#include "log.h"

#include <esp_system.h>
#include <esp_mac.h>
#include <string.h>


static uint16_t s_id      = 0;
static char     s_id_hex[5]  = {0};
static char     s_mac_str[18] = {0};
static bool     s_ready   = false;


void identity_init() {

    if (s_ready) return;

    uint8_t mac[6] = {0};
    esp_err_t rc = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (rc != ESP_OK) {
        uint64_t fallback = ESP.getEfuseMac();
        mac[0] = (uint8_t)((fallback >> 40) & 0xFF);
        mac[1] = (uint8_t)((fallback >> 32) & 0xFF);
        mac[2] = (uint8_t)((fallback >> 24) & 0xFF);
        mac[3] = (uint8_t)((fallback >> 16) & 0xFF);
        mac[4] = (uint8_t)((fallback >> 8)  & 0xFF);
        mac[5] = (uint8_t)( fallback        & 0xFF);
    }

    s_id = ((uint16_t)mac[4] << 8) | (uint16_t)mac[5];
    snprintf(s_id_hex,  sizeof(s_id_hex),  "%04X", s_id);
    snprintf(s_mac_str, sizeof(s_mac_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_ready = true;
}

uint16_t    identity_id()      { return s_id; }
const char* identity_id_hex()  { return s_id_hex; }
const char* identity_mac_str() { return s_mac_str; }


void identity_print_banner() {
    LOG.println();
    LOG.println("---------- IDENTITY ----------");
    LOG.printf( "  Device ID  : %s   (lower 16 bits of MAC)\n", s_id_hex);
    LOG.printf( "  Full MAC   : %s\n", s_mac_str);
    LOG.println("------------------------------");
}
