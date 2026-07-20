/*
  core/mqtt_handler.h  (GATEWAY)

  Connects to the MQTT broker over MUTUAL TLS (per-device X.509
  certificate, port 8883) and publishes the fleet's sensor data wrapped
  in an AES-128-GCM envelope so the broker (and any subscriber WITHOUT
  the per-house key) cannot read the actual measurements. The device's
  certificate + private key are generated on-chip and provisioned over
  BLE (see device_cert.h) -- nothing is hardcoded.

  ON-THE-WIRE FORMAT
  ------------------
  Topic: achas/house/<HOUSE>/box/<BOX>/data

  Payload (JSON envelope):
    { "v"     : 4,
      "house" : 42,
      "id"    : "A3F1",
      "ct"    : "<base64( IV(12) | ciphertext | tag(16) )>" }

  After decryption with the per-house AES key, `ct` yields the plain
  JSON described in payload_to_json (see payload.h). Anyone with:
    * the AES master key, AND
    * the house_id (visible in `house` and the topic)
  can decrypt it. The broker has neither -- it only forwards bytes.

  COMMANDS (broker -> gateway)
  ---------------------------
  Topic: achas/house/<HOUSE>/gateway/command
  Payload (plain JSON; no envelope -- TLS is the only confidentiality):
    { "command":"OTA",    "target_box":0, "url":"https://...","sha256":"<64hex>","version":"1.2.0" }
    { "command":"OTA",    "target_box":2, "url":"https://...","sha256":"<64hex>","version":"1.2.0" }
    { "command":"PING",   "target_box":1 }
    { "command":"REBOOT", "target_box":0 }
    { "command":"PAIR",   "target_box":2 }
    target_box == 0  -> action targets the gateway itself.
    target_box 1..3  -> relayed to the specified sensor box over ESP-NOW
                        (queued, then delivered on the sensor's next check-in).
*/

#pragma once

#include <Arduino.h>
#include "payload.h"


// MQTT command callback. target_box=0 -> gateway; 1..3 -> sensor box.
// `args_json` is the raw JSON string of the original command so the
// handler can read OTA url/sha256/version fields without re-parsing.
typedef void (*MqttCommandCallback)(const String& command,
                                    int target_box,
                                    const String& args_json);


bool mqtt_connect();
bool mqtt_is_connected();
bool mqtt_ensure_connected();
void mqtt_loop();


// Publish one payload as an AES-128-GCM-encrypted JSON envelope.
bool mqtt_publish_data(const AchasPayload& payload, float rssi, float snr);


// Publish a status message (plaintext JSON, e.g. "online"/"offline").
bool mqtt_publish_status(const char* message);


void mqtt_set_command_callback(MqttCommandCallback callback);
void mqtt_disconnect();
