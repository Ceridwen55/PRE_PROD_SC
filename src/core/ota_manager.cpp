// core/ota_manager.cpp  (GATEWAY)
//
// Self-OTA goes over HTTPS and verifies SHA-256 before commit. Relay
// OTA tells a sensor box to do its own HTTPS+SHA-256 download by
// QUEUEING an ESP-NOW command for it (delivered on its next check-in).

#include "ota_manager.h"
#include "log.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "crypto.h"
#include "drivers/EspNow.h"


// ============================================================
// Helpers
// ============================================================

static bool is_valid_sha256_hex(const String& s) {
    if (s.length() != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static void hex_lower(const uint8_t* digest, size_t n, char* out) {
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]   = hex[digest[i] >> 4];
        out[2*i+1] = hex[digest[i] & 0x0F];
    }
    out[2*n] = '\0';
}

// Small HTTPS GET for a short text body (the "<url>.sig" file). The
// firmware's own integrity is enforced by SHA-256 + the signature check,
// so setInsecure() is acceptable here as well.
static bool http_get_text(const String& url, String& out, size_t max_len) {
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(OTA_HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);
    http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
    if (!http.begin(tls, url)) return false;

    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return false; }

    int len = http.getSize();
    if (len > (int)max_len) { http.end(); return false; }

    out = http.getString();
    http.end();
    out.trim();
    return out.length() > 0;
}


// ============================================================
// ota_self_update_https
// HTTPS GET -> stream into Update partition -> verify SHA-256 ->
// commit and reboot. ANY failure keeps the OLD firmware.
// ============================================================
bool ota_self_update_https(const String& url, const String& sha256_hex) {

    LOG.println();
    LOG.println("============== GATEWAY SELF-OTA ==============");
    LOG.printf ("  URL    : %s\n", url.c_str());
    LOG.printf ("  SHA256 : %s\n", sha256_hex.c_str());
    LOG.println("==============================================");

    if (!url.startsWith("https://")) {
        LOG.println("[OTA-Self] REJECT: URL is not HTTPS.");
        return false;
    }
    if (!is_valid_sha256_hex(sha256_hex)) {
        LOG.println("[OTA-Self] REJECT: bad SHA-256 string.");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        LOG.println("[OTA-Self] ERROR: WiFi not connected.");
        return false;
    }

    WiFiClientSecure tls;
    tls.setInsecure();              // SHA-256 + HTTPS provide integrity
    tls.setTimeout(OTA_HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);
    http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
    if (!http.begin(tls, url)) {
        LOG.println("[OTA-Self] ERROR: http.begin failed.");
        return false;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        LOG.printf("[OTA-Self] HTTP %d -- aborting.\n", code);
        http.end();
        return false;
    }

    int total = http.getSize();
    if (total <= 0) {
        LOG.println("[OTA-Self] ERROR: no Content-Length.");
        http.end();
        return false;
    }
    if ((unsigned)total > OTA_MAX_FIRMWARE_BYTES) {
        LOG.printf("[OTA-Self] REJECT: too large (%d).\n", total);
        http.end();
        return false;
    }

    if (!Update.begin((size_t)total)) {
        LOG.printf("[OTA-Self] Update.begin failed: %s\n",
                      Update.errorString());
        http.end();
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t  buf[OTA_STREAM_CHUNK_SIZE];
    int      got_total = 0;
    uint32_t last_log = millis();
    uint32_t last_progress_ms = millis();

    while (http.connected() && got_total < total) {
        // Feed the task watchdog: this download runs inside the publisher
        // task (60 s WDT). A large firmware over slow 4G can take longer than
        // that, and without this the WDT would reset the gateway mid-OTA.
        // (Harmless no-op if the current task isn't WDT-subscribed.)
        esp_task_wdt_reset();

        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - last_progress_ms > OTA_HTTP_TIMEOUT_MS) {
                LOG.println("[OTA-Self] ERROR: stream stalled.");
                Update.abort();
                mbedtls_sha256_free(&sha);
                http.end();
                return false;
            }
            delay(20);
            continue;
        }
        size_t want = (avail > sizeof(buf)) ? sizeof(buf) : avail;
        int got = stream->readBytes(buf, want);
        if (got <= 0) { delay(20); continue; }

        mbedtls_sha256_update(&sha, buf, got);
        size_t written = Update.write(buf, got);
        if (written != (size_t)got) {
            LOG.printf("[OTA-Self] Update.write short: %s\n",
                          Update.errorString());
            Update.abort();
            mbedtls_sha256_free(&sha);
            http.end();
            return false;
        }
        got_total += got;
        last_progress_ms = millis();

        if (millis() - last_log > 2000) {
            last_log = millis();
            LOG.printf("[OTA-Self] %d / %d bytes (%.1f%%)\n",
                          got_total, total,
                          100.0f * got_total / total);
        }
    }

    http.end();

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    char digest_hex[65];
    hex_lower(digest, sizeof(digest), digest_hex);
    LOG.printf("[OTA-Self] Computed SHA256: %s\n", digest_hex);
    LOG.printf("[OTA-Self] Expected SHA256: %s\n", sha256_hex.c_str());
    if (sha256_hex != digest_hex) {
        LOG.println("[OTA-Self] REJECT: SHA-256 mismatch.");
        Update.abort();
        return false;
    }

    // Fetch the detached signature ("<url>.sig") over HTTPS -- WiFi is
    // still connected here. Then verify the firmware was signed by US
    // before committing.
    String sig;
    if (!http_get_text(url + ".sig", sig, 1024)) {
        LOG.println("[OTA-Self] ERROR: could not download <url>.sig.");
        Update.abort();
        return false;
    }
    if (!crypto_verify_signature(digest, sizeof(digest), sig.c_str())) {
        LOG.println("[OTA-Self] REJECT: firmware signature invalid.");
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        LOG.printf("[OTA-Self] Update.end failed: %s\n",
                      Update.errorString());
        return false;
    }

    LOG.println("[OTA-Self] SUCCESS -- rebooting into new firmware.");
    delay(500);
    ESP.restart();
    return true;
}


// ============================================================
// Relay commands to a sensor box.
//
// We build the message "ACHAS|<DEVID>|<CMD>|<args>" and hand it to the
// ESP-NOW command queue. tasks.cpp sends it the moment that sensor next
// checks in (the sensor only listens for a reply right after sending).
// ============================================================

bool ota_send_to_sensor(const String& device_id_hex,
                        const String& url,
                        const String& sha256_hex) {

    if (!url.startsWith("https://")) {
        LOG.println("[OTA-Relay] REJECT: URL is not HTTPS.");
        return false;
    }
    if (!is_valid_sha256_hex(sha256_hex)) {
        LOG.println("[OTA-Relay] REJECT: bad SHA-256 string.");
        return false;
    }
    // Message: ACHAS|<DEVID>|OTA|<url>|<sha256>
    // The signature is NOT relayed -- the base64 RSA sig (~344 chars) does
    // not fit in one 250-byte ESP-NOW frame. The sensor downloads it from
    // "<url>.sig" itself (see core/ota_remote.cpp).
    // Limit 200: the gateway appends a 17-char "|<hmac>" auth tag at reply
    // time, and the sensor's receive buffer caps the whole frame at 220 B.
    String msg = "ACHAS|" + device_id_hex + "|OTA|" + url + "|" + sha256_hex;
    if (msg.length() >= 200) {
        LOG.printf("[OTA-Relay] REJECT: command too long (%u) -- shorten the URL.\n",
                      (unsigned)msg.length());
        return false;
    }
    espnow_queue_command(device_id_hex, msg);
    return true;
}


bool ota_relay_simple(const String& device_id_hex, const char* cmd) {
    String msg = String("ACHAS|") + device_id_hex + "|" + cmd;
    espnow_queue_command(device_id_hex, msg);
    return true;
}
