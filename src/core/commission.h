/*
  core/commission.h
  NVS-backed commissioning profile shared by BOTH runtime roles.

  One firmware binary runs all ACHAS boxes. Role and grouping metadata
  are written at pairing time from the mobile app.
*/

#pragma once

#include <Arduino.h>
#include <stdint.h>


#define COMMISSION_SSID_MAX          32
#define COMMISSION_PASS_MAX          63
#define COMMISSION_HOUSE_LABEL_MAX   24
#define COMMISSION_LOGICAL_NAME_MAX  32
#define COMMISSION_HOUSE_KEY_SIZE    16

enum : uint8_t {
    COMMISSION_ROLE_UNSET   = 0,
    COMMISSION_ROLE_SENSOR  = 1,
    COMMISSION_ROLE_GATEWAY = 2
};


struct CommissionData {
    uint16_t house_id;
    uint8_t  box_id;                              // gateway=0, sensors=1..3
    uint8_t  role;                               // sensor/gateway runtime role
    char     house_label[COMMISSION_HOUSE_LABEL_MAX + 1];
    char     logical_name[COMMISSION_LOGICAL_NAME_MAX + 1];
    char     gateway_device_id[5];               // 4-hex chars, e.g. "A3F1"
    char     wifi_ssid[COMMISSION_SSID_MAX + 1];
    char     wifi_pass[COMMISSION_PASS_MAX + 1];
    uint8_t  house_key[COMMISSION_HOUSE_KEY_SIZE]; // provisioned by backend server via BLE
    bool     house_key_set;
};


bool commission_init();
bool commission_load(CommissionData& out);
bool commission_save(const CommissionData& in);
bool commission_is_set();
bool commission_wipe();
void commission_print(const CommissionData& c);

void commission_autofill(CommissionData& c);
bool commission_validate(const CommissionData& c, char* err, size_t err_len);

bool commission_is_gateway(const CommissionData& c);
bool commission_is_sensor(const CommissionData& c);
const char* commission_role_name(const CommissionData& c);
