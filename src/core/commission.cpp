// core/commission.cpp

#include "commission.h"
#include "log.h"
#include "secrets.h"

#include <Preferences.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>


static const char* NS = "commission";
static Preferences prefs;

static const char* KEY_HOUSE     = "house_id";
static const char* KEY_BOX       = "box_id";
static const char* KEY_ROLE      = "role";
static const char* KEY_LABEL     = "house_lbl";
static const char* KEY_LOGICAL   = "logical_nm";
static const char* KEY_GATEWAY   = "gw_devid";
static const char* KEY_SSID      = "wifi_ssid";
static const char* KEY_PASS      = "wifi_pass";
static const char* KEY_HOUSE_KEY = "house_key";


static void set_err(char* err, size_t err_len, const char* msg) {
    if (err == nullptr || err_len == 0) return;
    snprintf(err, err_len, "%s", msg);
}


static bool is_hex4(const char* s) {
    if (s == nullptr) return false;
    if (strlen(s) != 4) return false;
    for (int i = 0; i < 4; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}


const char* commission_role_name(const CommissionData& c) {
    if (c.role == COMMISSION_ROLE_GATEWAY) return "gateway";
    if (c.role == COMMISSION_ROLE_SENSOR)  return "sensor";
    return "unset";
}


bool commission_is_gateway(const CommissionData& c) {
    if (c.role == COMMISSION_ROLE_GATEWAY) return true;
    if (c.role == COMMISSION_ROLE_SENSOR)  return false;
    return c.box_id == 0;
}


bool commission_is_sensor(const CommissionData& c) {
    return !commission_is_gateway(c);
}


void commission_autofill(CommissionData& c) {

    if (c.role != COMMISSION_ROLE_GATEWAY && c.role != COMMISSION_ROLE_SENSOR) {
        c.role = (c.box_id == 0) ? COMMISSION_ROLE_GATEWAY : COMMISSION_ROLE_SENSOR;
    }

    if (commission_is_gateway(c)) c.box_id = 0;
    else if (c.box_id == 0 || c.box_id > 3) c.box_id = 1;

    if (c.house_label[0] == '\0' && c.house_id != 0) {
        snprintf(c.house_label, sizeof(c.house_label), "HOUSE-%u", c.house_id);
    }

    if (c.logical_name[0] == '\0' && c.house_id != 0) {
        if (commission_is_gateway(c)) {
            snprintf(c.logical_name, sizeof(c.logical_name), "H%u-GW", c.house_id);
        } else {
            snprintf(c.logical_name, sizeof(c.logical_name), "H%u-S%u", c.house_id, c.box_id);
        }
    }
}


bool commission_validate(const CommissionData& c, char* err, size_t err_len) {

    if (c.house_id == 0) {
        set_err(err, err_len, "house_id must be 1..65535");
        return false;
    }
    if (!c.house_key_set) {
        set_err(err, err_len, "house_key not set -- provision from backend server");
        return false;
    }
    if (c.role != COMMISSION_ROLE_GATEWAY && c.role != COMMISSION_ROLE_SENSOR) {
        set_err(err, err_len, "role must be 'gateway' or 'sensor'");
        return false;
    }

    if (commission_is_gateway(c) && c.box_id != 0) {
        set_err(err, err_len, "gateway role requires box_id=0");
        return false;
    }
    if (commission_is_sensor(c) && (c.box_id < 1 || c.box_id > 3)) {
        set_err(err, err_len, "sensor role requires box_id=1..3");
        return false;
    }

    if (strlen(c.house_label) > COMMISSION_HOUSE_LABEL_MAX) {
        set_err(err, err_len, "house_label too long");
        return false;
    }
    if (strlen(c.logical_name) > COMMISSION_LOGICAL_NAME_MAX) {
        set_err(err, err_len, "logical_name too long");
        return false;
    }
    if (strlen(c.wifi_ssid) > COMMISSION_SSID_MAX) {
        set_err(err, err_len, "wifi_ssid too long");
        return false;
    }
    if (strlen(c.wifi_pass) > COMMISSION_PASS_MAX) {
        set_err(err, err_len, "wifi_pass too long");
        return false;
    }

    if (commission_is_sensor(c) && !is_hex4(c.gateway_device_id)) {
        set_err(err, err_len, "sensor role requires gateway_device_id (4 hex chars)");
        return false;
    }
    if (c.gateway_device_id[0] != '\0' && !is_hex4(c.gateway_device_id)) {
        set_err(err, err_len, "gateway_device_id must be 4 hex chars");
        return false;
    }

    set_err(err, err_len, "");
    return true;
}


bool commission_init() {
    if (!prefs.begin(NS, false)) {
        LOG.println("[Commission] ERROR: cannot open NVS namespace.");
        return false;
    }
    prefs.end();
    return true;
}


bool commission_load(CommissionData& out) {
    memset(&out, 0, sizeof(out));

    if (!prefs.begin(NS, true)) {
        LOG.println("[Commission] ERROR: cannot open NVS for read.");
        return false;
    }

    out.house_id = prefs.getUShort(KEY_HOUSE, 0);
    out.box_id   = prefs.getUChar(KEY_BOX, 0);
    out.role     = prefs.getUChar(KEY_ROLE, COMMISSION_ROLE_UNSET);

    String lbl   = prefs.getString(KEY_LABEL, "");
    String name  = prefs.getString(KEY_LOGICAL, "");
    String gwid  = prefs.getString(KEY_GATEWAY, "");
    String ssid  = prefs.getString(KEY_SSID, "");
    String pass  = prefs.getString(KEY_PASS, "");

    memset(out.house_key, 0, COMMISSION_HOUSE_KEY_SIZE);
    size_t key_bytes = prefs.getBytes(KEY_HOUSE_KEY, out.house_key, COMMISSION_HOUSE_KEY_SIZE);
    out.house_key_set = (key_bytes == COMMISSION_HOUSE_KEY_SIZE);

    prefs.end();

    if (ssid.length() == 0) ssid = DEFAULT_WIFI_SSID;
    if (pass.length() == 0) pass = DEFAULT_WIFI_PASSWORD;

    strncpy(out.house_label, lbl.c_str(), COMMISSION_HOUSE_LABEL_MAX);
    out.house_label[COMMISSION_HOUSE_LABEL_MAX] = '\0';

    strncpy(out.logical_name, name.c_str(), COMMISSION_LOGICAL_NAME_MAX);
    out.logical_name[COMMISSION_LOGICAL_NAME_MAX] = '\0';

    strncpy(out.gateway_device_id, gwid.c_str(), sizeof(out.gateway_device_id) - 1);
    out.gateway_device_id[sizeof(out.gateway_device_id) - 1] = '\0';

    strncpy(out.wifi_ssid, ssid.c_str(), COMMISSION_SSID_MAX);
    out.wifi_ssid[COMMISSION_SSID_MAX] = '\0';

    strncpy(out.wifi_pass, pass.c_str(), COMMISSION_PASS_MAX);
    out.wifi_pass[COMMISSION_PASS_MAX] = '\0';

    commission_autofill(out);
    return true;
}


bool commission_save(const CommissionData& in) {
    CommissionData tmp = in;
    commission_autofill(tmp);

    char err[96];
    if (!commission_validate(tmp, err, sizeof(err))) {
        LOG.printf("[Commission] ERROR: %s\n", err);
        return false;
    }

    if (!prefs.begin(NS, false)) {
        LOG.println("[Commission] ERROR: cannot open NVS for write.");
        return false;
    }

    bool ok = true;
    ok &= (prefs.putUShort(KEY_HOUSE, tmp.house_id) > 0);
    ok &= (prefs.putUChar(KEY_BOX, tmp.box_id) > 0);
    ok &= (prefs.putUChar(KEY_ROLE, tmp.role) > 0);
    ok &= (prefs.putString(KEY_LABEL, tmp.house_label) >= 0);
    ok &= (prefs.putString(KEY_LOGICAL, tmp.logical_name) >= 0);
    ok &= (prefs.putString(KEY_GATEWAY, tmp.gateway_device_id) >= 0);
    ok &= (prefs.putString(KEY_SSID, tmp.wifi_ssid) >= 0);
    ok &= (prefs.putString(KEY_PASS, tmp.wifi_pass) >= 0);
    ok &= (prefs.putBytes(KEY_HOUSE_KEY, tmp.house_key, COMMISSION_HOUSE_KEY_SIZE) == COMMISSION_HOUSE_KEY_SIZE);
    prefs.end();

    if (ok) LOG.println("[Commission] saved to NVS.");
    else    LOG.println("[Commission] ERROR: NVS write reported failure.");
    return ok;
}


bool commission_is_set() {
    if (!prefs.begin(NS, true)) return false;
    uint16_t h = prefs.getUShort(KEY_HOUSE, 0);
    prefs.end();
    return (h != 0);
}


bool commission_wipe() {
    if (!prefs.begin(NS, false)) return false;
    bool ok = prefs.clear();
    prefs.end();
    if (ok) LOG.println("[Commission] wiped NVS namespace.");
    return ok;
}


void commission_print(const CommissionData& c) {
    LOG.println("---------- COMMISSIONING -----");
    LOG.printf("  House ID          : %u%s\n",
                  c.house_id,
                  c.house_id == 0 ? "  (uncommissioned)" : "");
    LOG.printf("  House Label       : %s\n",
                  c.house_label[0] ? c.house_label : "(not set)");
    LOG.printf("  Role              : %s\n", commission_role_name(c));
    LOG.printf("  Box Slot          : %u\n", c.box_id);
    LOG.printf("  Logical Name      : %s\n",
                  c.logical_name[0] ? c.logical_name : "(auto)");
    LOG.printf("  Gateway Device ID : %s\n",
                  c.gateway_device_id[0] ? c.gateway_device_id : "(not set)");
    LOG.printf("  WiFi SSID         : %s\n",
                  c.wifi_ssid[0] ? c.wifi_ssid : "(not set)");
    LOG.printf("  WiFi Pass         : %s\n",
                  c.wifi_pass[0] ? "******** (set)" : "(not set)");
    LOG.printf("  House Key         : %s\n",
                  c.house_key_set ? "(set -- server provisioned)" : "(NOT SET -- provision from server)");
    LOG.println("------------------------------");
}
