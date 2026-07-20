// core/ble_pairing.cpp
// BLE identity + commissioning for unified gateway/sensor firmware.

#include "ble_pairing.h"
#include "log.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_bt.h>
#include <math.h>
#include <string.h>
#include <mbedtls/base64.h>

#include "config.h"
#include "identity.h"
#include "commission.h"
#include "device_cert.h"
#include "prov_session.h"
#include "drivers/Led.h"


static const char* SERVICE_UUID         = "0000acha-0000-1000-8000-00805f9b34fb";
static const char* CHAR_DEVICE_ID_UUID  = "0000aca1-0000-1000-8000-00805f9b34fb";
static const char* CHAR_MAC_UUID        = "0000aca2-0000-1000-8000-00805f9b34fb";
static const char* CHAR_INFO_UUID       = "0000aca3-0000-1000-8000-00805f9b34fb";
static const char* CHAR_LIVE_UUID       = "0000aca4-0000-1000-8000-00805f9b34fb";
static const char* CHAR_COMMISSION_UUID = "0000aca5-0000-1000-8000-00805f9b34fb";
static const char* CHAR_CSR_UUID        = "0000aca6-0000-1000-8000-00805f9b34fb"; // phone READS the CSR
static const char* CHAR_CERT_UUID       = "0000aca7-0000-1000-8000-00805f9b34fb"; // phone WRITES the signed cert
static const char* CHAR_PUBKEY_UUID     = "0000aca8-0000-1000-8000-00805f9b34fb"; // phone READS our public key (for ECDH)
static const char* CHAR_SESSION_UUID    = "0000aca9-0000-1000-8000-00805f9b34fb"; // phone WRITES its public key (start secure session)


static BLEServer*         s_server = nullptr;
static BLECharacteristic* s_char_live = nullptr;
static BLECharacteristic* s_char_commission = nullptr;
static BLECharacteristic* s_char_csr  = nullptr;
static BLECharacteristic* s_char_cert = nullptr;
static BLECharacteristic* s_char_pubkey  = nullptr;
static BLECharacteristic* s_char_session = nullptr;
static BleSnapshot        s_snap = { NAN, NAN, 0, 0, 0 };
static bool               s_inited = false;
static bool               s_accept_commission = false;
static volatile bool      s_commission_received = false;
// The commissioning payload is now an ENCRYPTED blob, base64-encoded, so
// it is larger than the old plaintext JSON -- give it more room.
static char               s_pending_payload[768] = {0};
static volatile bool      s_payload_pending = false;

// The phone's public key (base64) lands here when it writes char aca9.
// The main task picks it up and derives the secure session key.
static char               s_peerpub_b64[160] = {0};
static volatile bool      s_session_pending = false;

// The signed certificate can be bigger than one BLE write, so the phone
// sends it in pieces. We append each piece here and process it once the
// PEM end-marker has arrived.
static String             s_cert_accum;
static char               s_cert_payload[2048] = {0};
static volatile bool      s_cert_pending = false;


static String live_json() {
    char buf[180];
    char t[16], h[16], b[16], rssi[8];
    if (isnan(s_snap.temperature_c)) strcpy(t, "null");
    else snprintf(t, sizeof(t), "%.2f", s_snap.temperature_c);
    if (isnan(s_snap.humidity_pct)) strcpy(h, "null");
    else snprintf(h, sizeof(h), "%.2f", s_snap.humidity_pct);
    if (s_snap.battery_mv == 0) strcpy(b, "null");
    else snprintf(b, sizeof(b), "%u", (unsigned)s_snap.battery_mv);
    if (s_snap.last_rssi == 0) strcpy(rssi, "null");
    else snprintf(rssi, sizeof(rssi), "%d", s_snap.last_rssi);
    snprintf(buf, sizeof(buf),
             "{\"temp_c\":%s,\"hum_pct\":%s,\"batt_mv\":%s,"
             "\"uptime_s\":%lu,\"rssi\":%s}",
             t, h, b, (unsigned long)s_snap.uptime_s, rssi);
    return String(buf);
}


static String info_json() {
    CommissionData c;
    commission_load(c);
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"role\":\"%s\",\"box\":%u,\"house\":%u,\"house_label\":\"%s\","
             "\"logical_name\":\"%s\",\"gateway_device_id\":\"%s\",\"fw\":\"%s\","
             "\"commissioned\":%s}",
             commission_role_name(c),
             (unsigned)c.box_id, (unsigned)c.house_id,
             c.house_label,
             c.logical_name,
             c.gateway_device_id,
             FIRMWARE_VERSION,
             (c.house_id != 0) ? "true" : "false");
    return String(buf);
}


// Tiny JSON extractors keep this module lightweight.
static bool extract_string(const String& src, const char* key, String& out) {
    int k = src.indexOf(String("\"") + key + "\"");
    if (k < 0) return false;
    int c = src.indexOf(':', k);              if (c < 0) return false;
    int q1 = src.indexOf('"', c);             if (q1 < 0) return false;
    int q2 = src.indexOf('"', q1 + 1);        if (q2 < 0) return false;
    out = src.substring(q1 + 1, q2);
    return true;
}

static bool extract_int(const String& src, const char* key, long& out) {
    int k = src.indexOf(String("\"") + key + "\"");
    if (k < 0) return false;
    int c = src.indexOf(':', k);              if (c < 0) return false;
    int e = c + 1;
    while (e < (int)src.length() && (src[e] == ' ' || src[e] == '\t')) e++;
    int start = e;
    while (e < (int)src.length() && (isdigit(src[e]) || src[e] == '-')) e++;
    if (e == start) return false;
    out = src.substring(start, e).toInt();
    return true;
}

static bool parse_role(const String& value, uint8_t& role) {
    if (value.equalsIgnoreCase("gateway")) {
        role = COMMISSION_ROLE_GATEWAY;
        return true;
    }
    if (value.equalsIgnoreCase("sensor")) {
        role = COMMISSION_ROLE_SENSOR;
        return true;
    }
    return false;
}


class CommissionWriteCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        if (!s_accept_commission) {
            c->setValue("ERR not in pairing mode");
            c->notify();
            return;
        }
        // The value is now an ENCRYPTED, base64-encoded blob (see
        // prov_session.h). Keep the BLE task lightweight: just buffer it,
        // the main task decrypts + parses it.
        std::string val = c->getValue();
        strncpy(s_pending_payload, val.c_str(), sizeof(s_pending_payload) - 1);
        s_pending_payload[sizeof(s_pending_payload) - 1] = '\0';
        s_payload_pending = true;
    }
};


// The phone writes its (throw-away) public key here to open the encrypted
// channel. We only buffer it; the main task runs the ECDH/HKDF maths.
class SessionWriteCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        if (!s_accept_commission) {
            c->setValue("ERR not in pairing mode");
            c->notify();
            return;
        }
        std::string val = c->getValue();
        strncpy(s_peerpub_b64, val.c_str(), sizeof(s_peerpub_b64) - 1);
        s_peerpub_b64[sizeof(s_peerpub_b64) - 1] = '\0';
        s_session_pending = true;
    }
};


// Receives the signed certificate from the phone, possibly in several
// BLE writes. We append each chunk and flag the main task once the PEM
// end-marker shows up. Heavy validation/parsing happens in the main task.
class CertWriteCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        if (!s_accept_commission) {
            c->setValue("ERR not in pairing mode");
            c->notify();
            return;
        }
        std::string val = c->getValue();
        String chunk = String(val.c_str());

        // A fresh "-----BEGIN CERTIFICATE-----" starts a new transfer.
        if (chunk.indexOf("-----BEGIN CERTIFICATE-----") >= 0) s_cert_accum = "";

        if (s_cert_accum.length() + chunk.length() < sizeof(s_cert_payload)) {
            s_cert_accum += chunk;
        }

        if (s_cert_accum.indexOf("-----END CERTIFICATE-----") >= 0) {
            strncpy(s_cert_payload, s_cert_accum.c_str(), sizeof(s_cert_payload) - 1);
            s_cert_payload[sizeof(s_cert_payload) - 1] = '\0';
            s_cert_pending = true;
        }
    }
};


// Runs on the MAIN task: validate + store the certificate, reply over BLE.
static void process_cert_write(const char* pem) {
    LOG.println("[BLE] Certificate received -- validating...");
    bool ok = device_cert_store_cert(pem);
    if (s_char_cert) {
        s_char_cert->setValue(ok ? "OK cert stored" : "ERR cert invalid");
        s_char_cert->notify();
    }
    s_cert_accum = "";
}


// Runs on the MAIN task: turn the phone's public key into a shared AES
// session key (heavy ECDH/HKDF maths), then tell the phone if it worked.
static void process_session_start(const char* peer_pub_b64) {
    LOG.println("[BLE] Public key received -- deriving secure session...");

    uint8_t peer_pub[80];
    size_t  peer_len = 0;
    int rc = mbedtls_base64_decode(peer_pub, sizeof(peer_pub), &peer_len,
                                   (const unsigned char*)peer_pub_b64,
                                   strlen(peer_pub_b64));

    bool ok = (rc == 0) && prov_session_start(peer_pub, peer_len);

    if (s_char_session) {
        s_char_session->setValue(ok ? "OK secure session ready"
                                    : "ERR could not start session");
        s_char_session->notify();
    }
}


// Runs on the MAIN task: decrypt the commissioning blob, then apply it.
// `raw_b64` = base64( IV(12) | ciphertext | tag(16) ), encrypted under
// the secure session key. Secrets (house_key, wifi_pass) therefore never
// appear in the clear on the BLE link.
static void process_commission_write(const char* raw_b64) {

    auto reply = [](const char* msg) {
        LOG.printf("[BLE] Commission: %s\n", msg);
        if (!s_char_commission) return;
        s_char_commission->setValue(msg);
        s_char_commission->notify();
    };

    // A secure session MUST exist first -- we refuse plaintext secrets.
    if (!prov_session_active()) {
        reply("ERR no secure session (write public key to aca9 first)");
        return;
    }

    // 1. base64 -> encrypted bytes.
    uint8_t enc[576];
    size_t  enc_len = 0;
    int rc = mbedtls_base64_decode(enc, sizeof(enc), &enc_len,
                                   (const unsigned char*)raw_b64, strlen(raw_b64));
    if (rc != 0) {
        reply("ERR commission blob is not valid base64");
        return;
    }

    // 2. decrypt -> plaintext JSON (fails closed on a wrong key / tamper).
    char plain[512];
    size_t plain_len = prov_session_open(enc, enc_len,
                                         (uint8_t*)plain, sizeof(plain) - 1);
    if (plain_len == 0) {
        reply("ERR commission decrypt failed");
        return;
    }
    plain[plain_len] = '\0';

    // 3. parse + apply the (now decrypted) commissioning JSON.
    LOG.println("[BLE] Commission decrypted OK -- applying.");

    String payload = String(plain);
    CommissionData cd;
    commission_load(cd);

    long house = 0, box = -1;
    String role_str, ssid, pass, house_label, logical_name, gateway_dev, house_key_hex;

    bool got_house  = extract_int(payload, "house_id", house);
    bool got_box    = extract_int(payload, "box_id", box);
    bool got_role   = extract_string(payload, "role", role_str);
    bool got_ssid   = extract_string(payload, "wifi_ssid", ssid);
    bool got_pass   = extract_string(payload, "wifi_pass", pass);
    bool got_hlabel = extract_string(payload, "house_label", house_label);
    bool got_lname  = extract_string(payload, "logical_name", logical_name);
    bool got_gw     = extract_string(payload, "gateway_device_id", gateway_dev);
    bool got_hkey   = extract_string(payload, "house_key", house_key_hex);

    auto send_err = [](const char* msg) {
        LOG.printf("[BLE] Commission ERR: %s\n", msg);
        if (!s_char_commission) return;
        char out[128];
        snprintf(out, sizeof(out), "ERR %s", msg);
        s_char_commission->setValue(out);
        s_char_commission->notify();
    };

    if (!got_house || house <= 0 || house > 65535) {
        send_err("house_id missing or out of range");
        return;
    }
    cd.house_id = (uint16_t)house;

    if (got_box && (box < 0 || box > 3)) {
        send_err("box_id must be 0..3");
        return;
    }
    if (got_box) cd.box_id = (uint8_t)box;

    if (got_role) {
        uint8_t role = COMMISSION_ROLE_UNSET;
        if (!parse_role(role_str, role)) {
            send_err("role must be gateway/sensor");
            return;
        }
        cd.role = role;
    }

    if (got_hlabel) {
        if (house_label.length() > COMMISSION_HOUSE_LABEL_MAX) {
            send_err("house_label too long");
            return;
        }
        strncpy(cd.house_label, house_label.c_str(), COMMISSION_HOUSE_LABEL_MAX);
        cd.house_label[COMMISSION_HOUSE_LABEL_MAX] = '\0';
    }

    if (got_lname) {
        if (logical_name.length() > COMMISSION_LOGICAL_NAME_MAX) {
            send_err("logical_name too long");
            return;
        }
        strncpy(cd.logical_name, logical_name.c_str(), COMMISSION_LOGICAL_NAME_MAX);
        cd.logical_name[COMMISSION_LOGICAL_NAME_MAX] = '\0';
    }

    if (got_gw) {
        gateway_dev.toUpperCase();
        if (gateway_dev.length() != 4) {
            send_err("gateway_device_id must be 4 hex chars");
            return;
        }
        strncpy(cd.gateway_device_id, gateway_dev.c_str(), sizeof(cd.gateway_device_id) - 1);
        cd.gateway_device_id[sizeof(cd.gateway_device_id) - 1] = '\0';
    }

    if (got_ssid) {
        if (ssid.length() > COMMISSION_SSID_MAX) {
            send_err("wifi_ssid too long");
            return;
        }
        strncpy(cd.wifi_ssid, ssid.c_str(), COMMISSION_SSID_MAX);
        cd.wifi_ssid[COMMISSION_SSID_MAX] = '\0';
    }
    if (got_pass) {
        if (pass.length() > COMMISSION_PASS_MAX) {
            send_err("wifi_pass too long");
            return;
        }
        strncpy(cd.wifi_pass, pass.c_str(), COMMISSION_PASS_MAX);
        cd.wifi_pass[COMMISSION_PASS_MAX] = '\0';
    }

    if (got_hkey) {
        if (house_key_hex.length() != 32) {
            send_err("house_key must be 32 hex chars (16 bytes)");
            return;
        }
        house_key_hex.toLowerCase();
        bool hex_ok = true;
        for (int i = 0; i < 16; i++) {
            char hi = house_key_hex[i * 2];
            char lo = house_key_hex[i * 2 + 1];
            if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo)) {
                hex_ok = false;
                break;
            }
            char byte_str[3] = { hi, lo, 0 };
            cd.house_key[i] = (uint8_t)strtoul(byte_str, nullptr, 16);
        }
        if (!hex_ok) {
            send_err("house_key contains invalid hex characters");
            return;
        }
        cd.house_key_set = true;
    }

    commission_autofill(cd);

    if (commission_is_gateway(cd) && cd.gateway_device_id[0] == '\0') {
        strncpy(cd.gateway_device_id, identity_id_hex(), sizeof(cd.gateway_device_id) - 1);
        cd.gateway_device_id[sizeof(cd.gateway_device_id) - 1] = '\0';
    }

    char err[96];
    if (!commission_validate(cd, err, sizeof(err))) {
        char out[120];
        snprintf(out, sizeof(out), "ERR %s", err);
        if (s_char_commission) {
            s_char_commission->setValue(out);
            s_char_commission->notify();
        }
        return;
    }

    if (!commission_save(cd)) {
        if (s_char_commission) {
            s_char_commission->setValue("ERR NVS write failed");
            s_char_commission->notify();
        }
        return;
    }

    if (s_char_commission) {
        s_char_commission->setValue("OK rebooting");
        s_char_commission->notify();
    }
    s_commission_received = true;
}


static void build_gatt() {

    BLEService* svc = s_server->createService(BLEUUID(SERVICE_UUID), 32);

    BLECharacteristic* ch_id = svc->createCharacteristic(
        CHAR_DEVICE_ID_UUID, BLECharacteristic::PROPERTY_READ);
    ch_id->setValue((char*)identity_id_hex());

    BLECharacteristic* ch_mac = svc->createCharacteristic(
        CHAR_MAC_UUID, BLECharacteristic::PROPERTY_READ);
    ch_mac->setValue((char*)identity_mac_str());

    BLECharacteristic* ch_info = svc->createCharacteristic(
        CHAR_INFO_UUID, BLECharacteristic::PROPERTY_READ);
    String s = info_json();
    ch_info->setValue(s.c_str());

    s_char_live = svc->createCharacteristic(
        CHAR_LIVE_UUID, BLECharacteristic::PROPERTY_READ);
    String l = live_json();
    s_char_live->setValue(l.c_str());

    s_char_commission = svc->createCharacteristic(
        CHAR_COMMISSION_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY);
    s_char_commission->addDescriptor(new BLE2902());
    s_char_commission->setCallbacks(new CommissionWriteCB());

    // CSR: the phone reads this to get "the device's request for a cert".
    // It is left empty until pairing starts (see ble_pairing_run), so we
    // don't spend CPU building a CSR on every sensor wake.
    s_char_csr = svc->createCharacteristic(
        CHAR_CSR_UUID, BLECharacteristic::PROPERTY_READ);
    s_char_csr->setValue("");

    // CERT: the phone writes the backend-signed certificate back here.
    s_char_cert = svc->createCharacteristic(
        CHAR_CERT_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY);
    s_char_cert->addDescriptor(new BLE2902());
    s_char_cert->setCallbacks(new CertWriteCB());

    // PUBKEY: the phone reads our public key to set up the encrypted
    // channel (ECDH). Filled in when pairing starts (see ble_pairing_run).
    s_char_pubkey = svc->createCharacteristic(
        CHAR_PUBKEY_UUID, BLECharacteristic::PROPERTY_READ);
    s_char_pubkey->setValue("");

    // SESSION: the phone writes its public key here to OPEN the encrypted
    // channel. Must happen before the commission write (char aca5).
    s_char_session = svc->createCharacteristic(
        CHAR_SESSION_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY);
    s_char_session->addDescriptor(new BLE2902());
    s_char_session->setCallbacks(new SessionWriteCB());

    svc->start();
}


bool ble_init() {

    if (s_inited) return true;

    CommissionData c;
    commission_load(c);
    char role_letter = commission_is_gateway(c) ? 'G' : 'S';

    char name[20];
    snprintf(name, sizeof(name), "STAYCOOL-%c-%s", role_letter, identity_id_hex());

    BLEDevice::init(name);
    s_server = BLEDevice::createServer();
    build_gatt();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMinPreferred(0x12);

    s_inited = true;
    LOG.printf("[BLE] Ready. Advertise name: \"%s\"\n", name);
    return true;
}


void ble_set_snapshot(const BleSnapshot& s) {
    s_snap = s;
    if (s_char_live) {
        String j = live_json();
        s_char_live->setValue(j.c_str());
    }
}


void ble_advertise_forever() {
    if (!s_inited && !ble_init()) return;
    s_accept_commission = false;
    BLEDevice::startAdvertising();
    LOG.println("[BLE] Advertising forever (identification only).");
}


void ble_advertise_window(uint32_t duration_ms) {
    if (!s_inited && !ble_init()) return;
    s_accept_commission = false;
    BLEDevice::startAdvertising();
    LOG.printf("[BLE] Advertising for %lu ms.\n", (unsigned long)duration_ms);
    uint32_t start = millis();
    while (millis() - start < duration_ms) {
        delay(100);
    }
    BLEDevice::stopAdvertising();
}


void ble_pairing_run(uint32_t timeout_ms) {
    if (!s_inited && !ble_init()) return;
    if (timeout_ms == 0) timeout_ms = BLE_PAIRING_TIMEOUT_MS;

    s_accept_commission   = true;
    s_commission_received = false;
    s_payload_pending     = false;
    s_cert_pending        = false;
    s_cert_accum          = "";
    s_session_pending     = false;
    prov_session_reset();   // every pairing starts a fresh secure session

    // Build this device's CSR now (heavy crypto, fine on the main task)
    // and expose it so the phone can read it during this pairing window.
    char csr_buf[1024];
    if (device_cert_build_csr(csr_buf, sizeof(csr_buf))) {
        if (s_char_csr) s_char_csr->setValue(csr_buf);
        LOG.println("[BLE] CSR ready for the app to read.");
    } else {
        LOG.println("[BLE] WARN: could not build CSR (cert provisioning unavailable).");
    }

    // Expose our public key (base64) so the phone can set up the encrypted
    // channel via ECDH before it sends any secret (house_key, wifi_pass).
    uint8_t pub_raw[80];
    size_t  pub_len = 0;
    if (device_cert_export_pubkey(pub_raw, sizeof(pub_raw), &pub_len)) {
        unsigned char pub_b64[160];
        size_t b64_len = 0;
        if (mbedtls_base64_encode(pub_b64, sizeof(pub_b64), &b64_len,
                                  pub_raw, pub_len) == 0) {
            pub_b64[b64_len] = '\0';
            if (s_char_pubkey) s_char_pubkey->setValue((char*)pub_b64);
            LOG.println("[BLE] Public key ready for the app (ECDH).");
        }
    } else {
        LOG.println("[BLE] WARN: could not export public key (secure channel unavailable).");
    }

    BLEDevice::startAdvertising();
    LOG.printf("[BLE] PAIRING MODE for up to %lu s\n",
                  (unsigned long)(timeout_ms / 1000UL));

    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        if (s_session_pending) {
            s_session_pending = false;
            process_session_start(s_peerpub_b64);
        }
        if (s_payload_pending) {
            s_payload_pending = false;
            process_commission_write(s_pending_payload);
        }
        if (s_cert_pending) {
            s_cert_pending = false;
            process_cert_write(s_cert_payload);
        }
        if (s_commission_received) {
            LOG.println("[BLE] Commission received -- rebooting in 500 ms.");
            led_status_blink(5);   // 5 blinks = commissioning succeeded
            delay(500);
            ESP.restart();
        }
        delay(200);
    }
    s_accept_commission = false;
    prov_session_reset();   // never leave a session key sitting in RAM
    BLEDevice::stopAdvertising();
    LOG.println("[BLE] Pairing window timed out.");
}


void ble_shutdown() {
    if (!s_inited) return;
    BLEDevice::stopAdvertising();
    BLEDevice::deinit(true);
    s_inited = false;
    s_server = nullptr;
    s_char_live = nullptr;
    s_char_commission = nullptr;
    s_char_csr = nullptr;
    s_char_cert = nullptr;
    s_char_pubkey = nullptr;
    s_char_session = nullptr;
    prov_session_reset();
    LOG.println("[BLE] Shutdown complete.");
}
