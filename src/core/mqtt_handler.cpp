// core/mqtt_handler.cpp  (GATEWAY)
//
// MQTT client (TLS 8883). Sensor data publishes are AES-128-GCM-
// encrypted with the per-house key before they leave the device.

#include "mqtt_handler.h"
#include "log.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

#include "config.h"
#include "certs.h"
#include "identity.h"
#include "commission.h"
#include "crypto.h"
#include "device_cert.h"


// ============================================================
// MQTT client setup
// ============================================================
static WiFiClientSecure   wifi_client;
static PubSubClient       mqtt(wifi_client);

static MqttCommandCallback g_command_callback = nullptr;

static char topic_status[80];
static char topic_command[80];
static char mqtt_client_id[40];
static uint16_t g_house_id = 0;


// ============================================================
// base64_encode_simple
// One-shot wrapper around mbedtls_base64_encode. Caller-supplied
// `out` buffer must be at least (4 * (in_len + 2) / 3) + 1 bytes.
// ============================================================
static int base64_encode_simple(const uint8_t* in, size_t in_len,
                                char* out, size_t out_sz) {
    size_t written = 0;
    int rc = mbedtls_base64_encode((unsigned char*)out, out_sz,
                                   &written, in, in_len);
    if (rc != 0) return rc;
    out[written] = '\0';
    return 0;
}


// ============================================================
// on_mqtt_message
// Parse incoming JSON, extract command + target_box + optional
// args, fire the callback registered by tasks.cpp.
// ============================================================
static void on_mqtt_message(char* topic, byte* payload, unsigned int length) {

    if (length == 0) return;

    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    LOG.printf("[MQTT] RX on '%s': %s\n", topic, message.c_str());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, message);
    if (err) {
        LOG.printf("[MQTT] ERROR: JSON parse failed: %s\n", err.c_str());
        return;
    }

    String command = doc["command"] | "";
    int target_box = doc["target_box"] | -1;

    if (command.isEmpty() || target_box < 0) {
        LOG.println("[MQTT] ERROR: command or target_box missing/invalid.");
        return;
    }

    LOG.printf("[MQTT] Command='%s' target_box=%d\n",
                  command.c_str(), target_box);

    if (g_command_callback) {
        g_command_callback(command, target_box, message);
    }
}


// ============================================================
// mqtt_connect
// Build topic names from the commissioned house_id, then connect.
// ============================================================
bool mqtt_connect() {

    wifi_client.stop();  // Release any stale TLS context before reconnect

    CommissionData cd;
    commission_load(cd);
    g_house_id = cd.house_id;

    snprintf(mqtt_client_id, sizeof(mqtt_client_id), "%s-%s-%04u",
             MQTT_CLIENT_PREFIX, identity_id_hex(), g_house_id);

    snprintf(topic_status, sizeof(topic_status),
             "%s/house/%u/gateway/status", MQTT_TOPIC_BASE, g_house_id);
    snprintf(topic_command, sizeof(topic_command),
             "%s/house/%u/gateway/command", MQTT_TOPIC_BASE, g_house_id);

    // Mutual TLS to MQTT broker:
    //   setCACert       -> we verify the broker's server certificate
    //                      (BROKER_ROOT_CA in certs.h — swap for your CA).
    //   setCertificate  -> broker verifies THIS device's identity.
    //   setPrivateKey   -> proves we own that certificate.
    // No username/password: the X.509 certificate IS the login.
    //
    // The cert + private key are NOT hardcoded. They were generated on
    // this chip and signed by the backend CA during BLE commissioning
    // (see device_cert.h). The private key never left the device.
    const char* dev_cert = nullptr;
    const char* dev_key  = nullptr;
    if (!device_cert_load_tls(&dev_cert, &dev_key)) {
        LOG.println("[MQTT] ERROR: no device certificate yet -- provision via BLE pairing first.");
        return false;
    }
    wifi_client.setCACert(BROKER_ROOT_CA);
    wifi_client.setCertificate(dev_cert);
    wifi_client.setPrivateKey(dev_key);

    mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);
    mqtt.setCallback(on_mqtt_message);
    mqtt.setBufferSize(2048);

    LOG.printf("[MQTT] Connecting (mTLS) to %s:%d as '%s'...\n",
                  MQTT_BROKER_HOST, MQTT_BROKER_PORT, mqtt_client_id);

    const char* last_will_json = "{\"status\":\"offline\"}";
    bool connected = mqtt.connect(mqtt_client_id, nullptr, nullptr,
                                  topic_status, 0, true, last_will_json);

    if (!connected) {
        LOG.printf("[MQTT] ERROR: connect failed, state=%d\n", mqtt.state());
        return false;
    }

    mqtt.subscribe(topic_command);
    LOG.printf("[MQTT] subscribed to: %s\n", topic_command);
    mqtt.publish(topic_status, "{\"status\":\"online\"}", true);
    LOG.printf("[MQTT] status=online published on: %s\n", topic_status);
    return true;
}


bool mqtt_is_connected()     { return mqtt.connected(); }
bool mqtt_ensure_connected() {
    if (mqtt.connected()) return true;
    LOG.println("[MQTT] reconnecting...");
    return mqtt_connect();
}
void mqtt_loop()             { if (mqtt.connected()) mqtt.loop(); }


// ============================================================
// mqtt_publish_data
// 1. payload_to_json() -> compact inner JSON string
// 2. AES-128-GCM-encrypt the inner JSON
// 3. base64-encode the envelope
// 4. wrap into {"v":4,"house":H,"id":"DEVID","ct":"..."} and publish
// ============================================================
bool mqtt_publish_data(const AchasPayload& payload, float rssi, float snr) {

    if (!mqtt.connected()) {
        LOG.println("[MQTT] ERROR: not connected, cannot publish.");
        return false;
    }

    // Step 1: build inner JSON.
    String inner = payload_to_json(payload, rssi, snr);

    // Step 2: encrypt.
    uint8_t  cipher_buf[1024];
    size_t   cipher_len = crypto_encrypt(
        (const uint8_t*)inner.c_str(), inner.length(),
        cipher_buf, sizeof(cipher_buf));
    if (cipher_len == 0) {
        LOG.println("[MQTT] ERROR: crypto_encrypt failed.");
        return false;
    }

    // Step 3: base64.
    char b64[1500];
    if (base64_encode_simple(cipher_buf, cipher_len, b64, sizeof(b64)) != 0) {
        LOG.println("[MQTT] ERROR: base64_encode failed.");
        return false;
    }

    // Step 4: envelope JSON.
    JsonDocument env;
    env["v"]     = PAYLOAD_VERSION;
    env["house"] = payload.house_id;
    char dhex[8];
    snprintf(dhex, sizeof(dhex), "%04X", payload.device_id);
    env["id"]    = dhex;
    env["ct"]    = b64;

    String envJson;
    serializeJson(env, envJson);

    // Topic: achas/house/<HOUSE>/box/<BOX>/data
    char topic[80];
    snprintf(topic, sizeof(topic),
             "%s/house/%u/box/%u/data",
             MQTT_TOPIC_BASE, payload.house_id, payload.box_id);

    bool ok = mqtt.publish(topic, envJson.c_str());
    if (ok) LOG.printf("[MQTT] published %u bytes to %s (encrypted)\n",
                          envJson.length(), topic);
    else    LOG.printf("[MQTT] ERROR: publish to %s failed\n", topic);
    return ok;
}


// ============================================================
// mqtt_publish_status
// Plain JSON, retained. Used for {"status":"online"}, OTA progress, etc.
// ============================================================
bool mqtt_publish_status(const char* message) {
    if (!mqtt.connected()) return false;
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"status\":\"%s\"}", message);
    bool ok = mqtt.publish(topic_status, buf, true);
    if (ok) LOG.printf("[MQTT] Status: %s\n", buf);
    return ok;
}


void mqtt_set_command_callback(MqttCommandCallback cb) {
    g_command_callback = cb;
}


void mqtt_disconnect() {
    if (mqtt.connected()) {
        mqtt.publish(topic_status, "{\"status\":\"offline\"}", true);
        mqtt.disconnect();
    }
    LOG.println("[MQTT] Disconnected.");
}
