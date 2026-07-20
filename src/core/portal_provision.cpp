/*
  core/portal_provision.cpp  -- see portal_provision.h for the "why".

  Flow (single radio, so we do NOT keep AP + STA up together):
    1. AP up + web form.
    2. Installer submits -> we store the form, tear the AP down, connect to
       the dongle Wi-Fi (STA), and enrol against the Fleet API over HTTPS.
    3. SENSOR success  -> blink LED + reboot.
       GATEWAY success -> AP back up, show the HOUSE ID to copy onto sensors.
       Failure         -> AP back up, show the error, let them retry.
*/

#include "portal_provision.h"
#include "log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "identity.h"
#include "commission.h"
#include "device_cert.h"
#include "wifi_policy.h"


// ==================================================================
//  State
// ==================================================================
enum PState { P_FORM, P_WORKING, P_GATEWAY_DONE, P_ERROR };

static WebServer  server(80);
static DNSServer  dns;
static PState     g_state = P_FORM;
static bool       g_go    = false;    // /submit -> run loop should enrol

// submitted form fields
static String f_role, f_label, f_room, f_ssid, f_pass, f_token;
static int    f_house = 0, f_box = 0;

// results
static String g_err;
static int    g_done_house = 0;


// ==================================================================
//  Small helpers
// ==================================================================
static String apName() { return String("STAYCOOL-PROV-") + identity_id_hex(); }

static inline void statusLevel(bool on) {
    digitalWrite(PIN_LED_STATUS, on ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL);
}

// Visible "which box is this" blink (used by the Identify button).
static void identifyBlink() {
    pinMode(PIN_LED_STATUS, OUTPUT);
    for (int i = 0; i < 4; i++) { statusLevel(true); delay(200); statusLevel(false); delay(200); }
}

// Parse 32 lowercase/uppercase hex chars into 16 bytes. Returns false on
// any non-hex char or wrong length.
static bool hex32_to_key(const String& hex, uint8_t out[16]) {
    if (hex.length() != 32) return false;
    for (int i = 0; i < 16; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        if (!isxdigit((int)hi) || !isxdigit((int)lo)) return false;
        char bs[3] = { hi, lo, 0 };
        out[i] = (uint8_t)strtoul(bs, nullptr, 16);
    }
    return true;
}


// ==================================================================
//  HTML (kept small on purpose -- "form jangan ribet")
// ==================================================================
// Theme: black & white only (no logo). Plain, high-contrast, prints fine on
// any phone. The captive portal has no internet, so all styling is inline.
static String pageHead(const char* title) {
    String s = F("<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
                 "content='width=device-width,initial-scale=1'>"
                 "<title>STAYCOOL Setup</title><style>"
                 "body{font-family:'Segoe UI',system-ui,sans-serif;margin:0;background:#fff;color:#000}"
                 ".w{max-width:420px;margin:0 auto;padding:20px}"
                 "h2{margin:14px 0 4px;color:#000}"
                 "label{display:block;margin:12px 0 4px;font-size:14px;color:#333}"
                 ".help{font-size:12px;color:#666;margin:2px 0 6px;line-height:1.35}"
                 "input,select{width:100%;padding:10px;border-radius:6px;border:1px solid #999;"
                 "background:#fff;color:#000;box-sizing:border-box}"
                 "input:focus,select:focus{outline:2px solid #000;border-color:#000}"
                 "button{width:100%;padding:12px;margin-top:16px;border:0;border-radius:6px;"
                 "background:#000;color:#fff;font-size:16px;font-weight:600}"
                 ".sec{background:#f5f5f5;border:1px solid #ddd;border-radius:8px;padding:14px;margin-top:12px}"
                 ".b{background:#fff;color:#000;border:2px solid #000}"
                 ".err{background:#f2f2f2;border:1px solid #000;color:#000;padding:10px;border-radius:6px}"
                 ".ok{background:#f2f2f2;border:1px solid #000;color:#000;padding:14px;border-radius:6px}"
                 ".big{font-size:40px;font-weight:700;text-align:center;margin:10px 0;color:#000}"
                 "small{color:#666}"
                 ".brand{padding:12px 0 10px;border-bottom:2px solid #000}"
                 ".t1{display:block;font-size:24px;font-weight:800;letter-spacing:4px;color:#000}"
                 "</style></head><body><div class=w>"
                 "<div class=brand><span class=t1>STAYCOOL</span></div>");
    s += "<h2>"; s += title; s += "</h2>";
    s += "<small>Box ID: "; s += identity_id_hex(); s += "</small>";
    return s;
}
static String pageFoot() { return F("</div></body></html>"); }

static String formPage() {
    String s = pageHead("Device Setup");
    if (g_state == P_ERROR && g_err.length()) {
        s += "<div class=err><b>Failed:</b> " + g_err + "</div>";
    }
    s += F(
      "<form method=POST action=/submit>"
      "<label>Role</label>"
      "<select name=role id=role onchange=\"t()\">"
      "<option value=gateway>Gateway (box 0)</option>"
      "<option value=sensor>Sensor</option></select>"

      "<div id=gw class=sec>"
      "<label>House ID (choose the number for this house)</label>"
      "<input name=house_id_gw type=number inputmode=numeric placeholder='e.g. 1'>"
      "<label>House Address</label>"
      "<div class=help>Enter your block, street name, unit number, and postal code."
      "<br>Block Number / Street Name / Unit No. / Postal Code</div>"
      "<input name=house_label placeholder='Blk 123 Toa Payoh Lorong 1 #04-50 S310123'>"
      "</div>"

      "<div id=sn class=sec style=display:none>"
      "<label>House ID (from the gateway screen)</label>"
      "<input name=house_id type=number inputmode=numeric placeholder='e.g. 7'>"
      "<label>Box number</label>"
      "<select name=box_id><option>1</option><option>2</option><option>3</option></select>"
      "</div>"

      "<div class=sec>"
      "<label>Room / Area Type</label>"
      "<div class=help>Select or type the specific room/area being referenced "
      "(e.g., Bedroom 1, Study Room, Living Room, Kitchen).</div>"
      "<input name=room placeholder='e.g., Bedroom 1, e.g., Kitchen'>"
      "<label>Wi-Fi name (dongle)</label><input name=ssid value='StayCool'>"
      "<label>Wi-Fi password</label><input name=pass type=password value='$t4yc*OL_~'>"
      "<label>Fleet API token</label><input name=token type=password>"
      "</div>"

      "<button type=submit>Provision this box</button>"
      "</form>"
      "<button class=b onclick=\"fetch('/identify')\">Identify (blink LED)</button>"
      "<script>function t(){var g=document.getElementById('role').value=='gateway';"
      "document.getElementById('gw').style.display=g?'':'none';"
      "document.getElementById('sn').style.display=g?'none':'';}t();</script>");
    s += pageFoot();
    return s;
}

static String workingPage() {
    String s = pageHead("Provisioning...");
    s += F("<div class=sec>Connecting to Wi-Fi and enrolling with the server."
           "<br><br><b>Watch the box:</b> the LED blinks on success.<br><br>"
           "<b>Gateway:</b> reconnect to this hotspot in ~30s to read the House ID.<br>"
           "<b>Sensor:</b> the box restarts by itself when done.</div>");
    s += pageFoot();
    return s;
}

static String gatewayDonePage() {
    String s = pageHead("Gateway ready");
    s += F("<div class=ok>Gateway provisioned. Write this House ID on the "
           "3 sensor boxes of this house:</div>");
    s += "<div class=big>House #" + String(g_done_house) + "</div>";
    s += F("<form method=POST action=/reboot><button>Finish &amp; restart gateway</button></form>");
    s += pageFoot();
    return s;
}


// ==================================================================
//  HTTP handlers
// ==================================================================
static void handleRoot() {
    if (g_state == P_GATEWAY_DONE) server.send(200, "text/html", gatewayDonePage());
    else if (g_state == P_WORKING) server.send(200, "text/html", workingPage());
    else                           server.send(200, "text/html", formPage());
}

static void handleSubmit() {
    f_role  = server.arg("role");
    f_label = server.arg("house_label");
    f_room  = server.arg("room");
    // The GATEWAY now chooses its own House ID (field "house_id_gw"); a SENSOR
    // still takes the House ID shown on the gateway screen (field "house_id").
    f_house = (f_role == "gateway")
                ? server.arg("house_id_gw").toInt()
                : server.arg("house_id").toInt();
    f_box   = server.arg("box_id").toInt();
    f_ssid  = server.arg("ssid");
    f_pass  = server.arg("pass");
    f_token = server.arg("token");

    // Minimal validation before we tear the AP down.
    String why;
    if (f_ssid.length() == 0)               why = "Wi-Fi name is empty";
    else if (f_room.length() == 0)          why = "Room / Area Type is empty";
    else if (f_token.length() == 0)         why = "API token is empty";
    else if (f_role == "sensor" && f_house <= 0) why = "House ID is required for a sensor";
    else if (f_role == "sensor" && (f_box < 1 || f_box > 3)) why = "Box number must be 1-3";
    else if (f_role == "gateway" && f_label.length() == 0)   why = "House Address is required";
    else if (f_role == "gateway" && (f_house < 1 || f_house > 65535)) why = "House ID must be 1-65535";

    if (why.length()) { g_state = P_ERROR; g_err = why; server.send(200, "text/html", formPage()); return; }

    g_state = P_WORKING;
    g_go    = true;                       // run loop will do the enrolment
    server.send(200, "text/html", workingPage());
}

static void handleStatus() {
    const char* st = (g_state == P_GATEWAY_DONE) ? "gateway_done"
                   : (g_state == P_WORKING)      ? "working"
                   : (g_state == P_ERROR)        ? "error" : "form";
    server.send(200, "application/json", String("{\"state\":\"") + st + "\"}");
}

static void handleIdentify() { server.send(200, "text/plain", "ok"); identifyBlink(); }

static void handleReboot() { server.send(200, "text/plain", "restarting"); delay(500); ESP.restart(); }

static void handleNotFound() {   // captive-portal redirect
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
}


// ==================================================================
//  AP up / down
// ==================================================================
static void registerRoutes() {
    server.on("/",         handleRoot);
    server.on("/submit",   HTTP_POST, handleSubmit);
    server.on("/status",   handleStatus);
    server.on("/identify", handleIdentify);
    server.on("/reboot",   HTTP_POST, handleReboot);
    server.onNotFound(handleNotFound);
}

static void startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName().c_str());
    delay(200);
    dns.start(53, "*", WiFi.softAPIP());
    server.begin();
    LOG.printf("[Portal] AP up: \"%s\"  ->  http://%s/\n",
                  apName().c_str(), WiFi.softAPIP().toString().c_str());
}

static void stopAP() {
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
}


// ==================================================================
//  Fleet API client (HTTPS, Bearer token). Integrity of the returned
//  secrets is protected by TLS; setInsecure() skips server-cert pinning
//  (same trade-off as OTA -- the transport is still encrypted).
// ==================================================================
static bool https_req(const char* method, const String& url,
                      const String& body, String& out) {
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(15);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(15000);
    if (!http.begin(tls, url)) return false;
    http.addHeader("Authorization", "Bearer " + f_token);
    if (body.length()) http.addHeader("Content-Type", "application/json");

    int code = (strcmp(method, "POST") == 0) ? http.POST(body) : http.GET();
    out = http.getString();
    http.end();
    if (code < 200 || code >= 300) {
        LOG.printf("[Portal] %s %s -> HTTP %d: %s\n", method, url.c_str(), code, out.c_str());
        return false;
    }
    return true;
}

// Create the house with the installer-chosen House ID. If desired_id > 0 we
// send it and the backend honours it (and REUSES the house if that id already
// exists, so re-provisioning a gateway with the same number does not create a
// duplicate). desired_id == 0 falls back to server auto-numbering.
static bool api_create_house(const String& label, int desired_id, int& house_id) {
    JsonDocument in;
    in["house_label"] = label;
    if (desired_id > 0) in["house_id"] = desired_id;
    String body; serializeJson(in, body);
    String resp;
    if (!https_req("POST", String(FLEET_API_BASE) + "/houses", body, resp)) return false;
    JsonDocument out;
    if (deserializeJson(out, resp)) return false;
    house_id = out["house_id"] | 0;
    return house_id > 0;
}

static bool api_sign_csr(const String& csr_pem, String& cert_pem, String& serial) {
    JsonDocument in; in["csr_pem"] = csr_pem;
    String body; serializeJson(in, body);
    String resp;
    if (!https_req("POST", String(FLEET_API_BASE) + "/devices/sign-csr", body, resp)) return false;
    JsonDocument out;
    if (deserializeJson(out, resp)) return false;
    cert_pem = out["cert_pem"] | "";
    serial   = out["serial"]   | "";
    return cert_pem.indexOf("BEGIN CERTIFICATE") >= 0;
}

static bool api_get_house_key(int house_id, String& key_hex) {
    String resp;
    if (!https_req("GET", String(FLEET_API_BASE) + "/houses/" + house_id + "/key", "", resp))
        return false;
    JsonDocument out;
    if (deserializeJson(out, resp)) return false;
    key_hex = out["house_key_hex"] | "";
    return key_hex.length() == 32;
}

static void api_enroll(int house_id, const char* role, int box, const String& serial,
                       const String& room) {
    JsonDocument in;
    in["device_id"]    = identity_id_hex();
    in["mac"]          = identity_mac_str();
    in["house_id"]     = house_id;
    in["role"]         = role;
    in["box_id"]       = box;
    in["logical_name"] = room;   // room name from the portal form
    in["cert_serial"]  = serial;
    String body; serializeJson(in, body);
    String resp;
    // Enrolment is bookkeeping for the OTA orchestrator -- log but don't fail
    // provisioning if the fleet table write hiccups.
    if (!https_req("POST", String(FLEET_API_BASE) + "/devices/enroll", body, resp))
        LOG.println("[Portal] WARN: enroll failed (device still provisioned).");
}


// ==================================================================
//  do_enroll -- the actual work (runs with the AP torn down, STA up)
//  Returns true on success. On a gateway, sets g_done_house.
// ==================================================================
static bool do_enroll() {
    if (!wifi_connect_secure(f_ssid.c_str(), f_pass.c_str(), OTA_WIFI_TIMEOUT_MS)) {
        g_err = "Wi-Fi connect failed (check name/password, WPA2 required)";
        return false;
    }

    CommissionData cd;
    commission_load(cd);
    strncpy(cd.wifi_ssid, f_ssid.c_str(), COMMISSION_SSID_MAX);  cd.wifi_ssid[COMMISSION_SSID_MAX] = 0;
    strncpy(cd.wifi_pass, f_pass.c_str(), COMMISSION_PASS_MAX);  cd.wifi_pass[COMMISSION_PASS_MAX] = 0;
    // Room name from the form: persist on-device (NVS) AND send to the Fleet
    // API at enroll (below). If left blank, commission_autofill() derives a
    // default like "H7-GW" / "H7-S2".
    strncpy(cd.logical_name, f_room.c_str(), COMMISSION_LOGICAL_NAME_MAX);
    cd.logical_name[COMMISSION_LOGICAL_NAME_MAX] = 0;

    String serial;

    if (f_role == "gateway") {
        // 1. Certificate: make one only if this chip doesn't have it yet.
        device_cert_init();
        if (!device_cert_has_cert()) {
            char csr[1024];
            if (!device_cert_build_csr(csr, sizeof(csr))) { g_err = "CSR build failed"; wifi_off(); return false; }
            String cert;
            if (!api_sign_csr(csr, cert, serial))         { g_err = "Certificate signing failed (token?)"; wifi_off(); return false; }
            if (!device_cert_store_cert(cert.c_str()))    { g_err = "Storing certificate failed"; wifi_off(); return false; }
        }
        // 2. Create (or reuse) the house with the installer-chosen House ID.
        int hid = f_house;
        if (!api_create_house(f_label, f_house, hid)) { g_err = "Create house failed (token?)"; wifi_off(); return false; }
        // 3. Fetch its house key.
        String key_hex;
        if (!api_get_house_key(hid, key_hex)) { g_err = "Fetch house key failed"; wifi_off(); return false; }
        if (!hex32_to_key(key_hex, cd.house_key)) { g_err = "Bad house key format"; wifi_off(); return false; }
        cd.house_key_set = true;

        cd.house_id = (uint16_t)hid;
        cd.box_id   = 0;
        cd.role     = COMMISSION_ROLE_GATEWAY;
        strncpy(cd.house_label, f_label.c_str(), COMMISSION_HOUSE_LABEL_MAX); cd.house_label[COMMISSION_HOUSE_LABEL_MAX] = 0;
        strncpy(cd.gateway_device_id, identity_id_hex(), sizeof(cd.gateway_device_id) - 1);
        cd.gateway_device_id[sizeof(cd.gateway_device_id) - 1] = 0;

        api_enroll(hid, "gateway", 0, serial, f_room);
        g_done_house = hid;

    } else {
        // SENSOR: house already exists (created by its gateway).
        int hid = f_house;
        String key_hex;
        if (!api_get_house_key(hid, key_hex)) { g_err = "House ID not found / fetch key failed"; wifi_off(); return false; }
        if (!hex32_to_key(key_hex, cd.house_key)) { g_err = "Bad house key format"; wifi_off(); return false; }
        cd.house_key_set = true;

        cd.house_id = (uint16_t)hid;
        cd.box_id   = (uint8_t)f_box;
        cd.role     = COMMISSION_ROLE_SENSOR;
        // ESP-NOW is broadcast, so the gateway id is only a placeholder.
        strncpy(cd.gateway_device_id, "0001", sizeof(cd.gateway_device_id) - 1);
        cd.gateway_device_id[sizeof(cd.gateway_device_id) - 1] = 0;

        api_enroll(hid, "sensor", f_box, "", f_room);
    }

    wifi_off();

    commission_autofill(cd);
    char err[96];
    if (!commission_validate(cd, err, sizeof(err))) { g_err = String("Profile invalid: ") + err; return false; }
    if (!commission_save(cd)) { g_err = "Saving profile to NVS failed"; return false; }

    LOG.println("[Portal] Provisioning saved. Success.");
    return true;
}


// ==================================================================
//  portal_provision_run
// ==================================================================
void portal_provision_run() {
    LOG.println("[Portal] Starting web provisioning portal.");
    registerRoutes();
    startAP();

    uint32_t start = millis();
    while (millis() - start < PORTAL_TIMEOUT_MS) {
        dns.processNextRequest();
        server.handleClient();

        if (g_go) {
            g_go = false;
            // Let the "Provisioning..." page finish sending to the phone
            // BEFORE we drop the AP to switch the radio to STA (otherwise the
            // phone sees a hung request).
            for (int i = 0; i < 50; i++) { server.handleClient(); delay(10); }
            stopAP();                          // free the radio for STA

            bool ok = do_enroll();

            if (ok && f_role == "sensor") {
                for (int i = 0; i < 6; i++) { statusLevel(true); delay(120); statusLevel(false); delay(120); }
                delay(400);
                ESP.restart();                 // sensor: done, boot into role
            } else if (ok) {
                g_state = P_GATEWAY_DONE;       // gateway: show House ID, wait for /reboot
                startAP();
            } else {
                g_state = P_ERROR;              // failed: show error, allow retry
                startAP();
            }
            start = millis();                   // give the operator a fresh window
        }
        delay(2);
    }

    LOG.println("[Portal] Timed out. Restarting to reopen the portal.");
    stopAP();
    delay(200);
    ESP.restart();
}
