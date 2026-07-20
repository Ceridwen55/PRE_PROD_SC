/*
  ============================================================
   core/ota_remote.h -- a SENSOR updating its own firmware over WiFi
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Lets a sensor box update itself without a technician on-site.

  WHERE IT IS USED
  ----------------
  main.cpp (handle_downlink) calls ota_remote_run() when the gateway
  replies with an OTA command.

  THE FLOW
  --------
    1. Gateway receives an OTA command from MQTT.
    2. Gateway queues "ACHAS|<DEVID>|OTA|<url>|<sha256>" and sends it
       to the sensor as the reply to its next check-in (ESP-NOW).
       NOTE: the signature is NOT in this command -- it is far too big
       for one ESP-NOW frame (base64 RSA-2048 ~344 B, ESP-NOW cap 250 B).
       Instead the sensor fetches it over HTTPS (see step 3).
    3. Sensor joins the dongle WiFi (WPA2 enforced), HTTPS-GETs <url>,
       hashes the bytes as they stream, and verifies the hash matches
       <sha256>. It then HTTPS-GETs "<url>.sig" (a small base64 file) and
       verifies the firmware was signed by us before committing.
    4. Reboot into the new firmware.

  WHY THE SIGNATURE IS A SEPARATE ".sig" FILE
  -------------------------------------------
  The RSA-2048 signature is ~256 bytes -> ~344 base64 chars. That does not
  fit in a single ESP-NOW reply (250-byte hard limit), so relaying it
  inline silently TRUNCATED it and every sensor OTA failed the signature
  check. We host it next to the firmware as "<url>.sig" and pull it over
  the same HTTPS channel that already carries the (much larger) firmware.

  SENSOR WiFi REQUIREMENT
  -----------------------
  A sensor normally has no WiFi (it uses ESP-NOW). To self-update it needs
  the dongle's WiFi credentials in NVS, so sensors MUST be commissioned
  with wifi_ssid/wifi_pass too (same dongle as the gateway). Without them
  this returns OTA_ERR_NOT_COMMISSIONED and keeps the old firmware.

  POLICY (refused before any I/O happens)
  ---------------------------------------
   * URL must start with "https://".
   * SHA-256 string must be 64 lowercase hex chars.
   * Binary size must be <= OTA_MAX_FIRMWARE_BYTES.
   * Hash of the streamed bytes must match exactly. ANY mismatch aborts
     and the device keeps running the OLD firmware.

  PARTIAL DOWNLOAD SAFETY
  -----------------------
  The Update library writes to the inactive OTA partition. We only call
  Update.end(true) after the SHA-256 verifies -- this is the call that
  flips the "boot from this partition next" flag. If anything fails
  before that, the next boot reverts cleanly to the running firmware.
*/

#pragma once

#include <Arduino.h>


// Result codes for ota_remote_run(). Anything other than OTA_OK keeps
// the device on the OLD firmware -- nothing is committed.
enum OtaResult {
    OTA_OK = 0,
    OTA_ERR_URL_NOT_HTTPS,
    OTA_ERR_SHA256_FORMAT,
    OTA_ERR_NOT_COMMISSIONED,
    OTA_ERR_WIFI_FAILED,
    OTA_ERR_HTTP_OPEN_FAILED,
    OTA_ERR_HTTP_BAD_STATUS,
    OTA_ERR_HTTP_NO_LENGTH,
    OTA_ERR_FIRMWARE_TOO_LARGE,
    OTA_ERR_UPDATE_BEGIN_FAILED,
    OTA_ERR_STREAM_STALLED,
    OTA_ERR_SHA256_MISMATCH,
    OTA_ERR_SIG_FETCH_FAILED,   // could not download "<url>.sig"
    OTA_ERR_BAD_SIGNATURE,
    OTA_ERR_UPDATE_END_FAILED,
};


// Human-readable name of an OtaResult. Used in serial logs and (for the
// gateway side, mirrored module) MQTT status messages.
const char* ota_result_str(OtaResult r);


// Run the remote OTA flow synchronously. On OTA_OK this function does
// NOT return -- it triggers a restart so the new firmware boots. On
// any error it returns the error code; the caller should report it
// (over ESP-NOW or MQTT) and keep running the OLD firmware.
//
// Inputs:
//   url     : full HTTPS URL of the firmware.bin
//   sha256  : 64 lowercase hex chars matching the file's SHA-256
//
// The signature is NOT passed in -- it is fetched over HTTPS from
// "<url>.sig" (base64 RSA signature) and checked against the OTA public
// key in certs.h. A missing/invalid signature makes the update fail
// closed -- nothing is flashed.
//
// The dongle WiFi credentials come from NVS (commission.h), so the
// caller doesn't have to pass them. If the device is not yet
// commissioned this returns OTA_ERR_NOT_COMMISSIONED immediately.
OtaResult ota_remote_run(const String& url, const String& sha256);
