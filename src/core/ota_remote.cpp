// core/ota_remote.cpp -- HTTPS+SHA-256 firmware pull over the dongle WiFi.

#include "ota_remote.h"
#include "log.h"

#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <string.h>

#include "config.h"
#include "commission.h"
#include "crypto.h"
#include "wifi_policy.h"


// ---------------------------------------------------------------
// Result name table
// ---------------------------------------------------------------
const char* ota_result_str(OtaResult r) {
    switch (r) {
        case OTA_OK:                       return "ok";
        case OTA_ERR_URL_NOT_HTTPS:        return "url_not_https";
        case OTA_ERR_SHA256_FORMAT:        return "sha256_format";
        case OTA_ERR_NOT_COMMISSIONED:     return "not_commissioned";
        case OTA_ERR_WIFI_FAILED:          return "wifi_failed";
        case OTA_ERR_HTTP_OPEN_FAILED:     return "http_open_failed";
        case OTA_ERR_HTTP_BAD_STATUS:      return "http_bad_status";
        case OTA_ERR_HTTP_NO_LENGTH:       return "http_no_length";
        case OTA_ERR_FIRMWARE_TOO_LARGE:   return "firmware_too_large";
        case OTA_ERR_UPDATE_BEGIN_FAILED:  return "update_begin_failed";
        case OTA_ERR_STREAM_STALLED:       return "stream_stalled";
        case OTA_ERR_SHA256_MISMATCH:      return "sha256_mismatch";
        case OTA_ERR_SIG_FETCH_FAILED:     return "sig_fetch_failed";
        case OTA_ERR_BAD_SIGNATURE:        return "bad_signature";
        case OTA_ERR_UPDATE_END_FAILED:    return "update_end_failed";
    }
    return "unknown";
}


// ---------------------------------------------------------------
// hex_lower
// Convert a digest buffer to a 64-char lowercase hex string.
// ---------------------------------------------------------------
static void hex_lower(const uint8_t* digest, size_t n, char* out) {
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]   = hex[digest[i] >> 4];
        out[2*i+1] = hex[digest[i] & 0x0F];
    }
    out[2*n] = '\0';
}


// ---------------------------------------------------------------
// is_valid_sha256_hex
// Exactly 64 chars, only [0-9a-f].
// ---------------------------------------------------------------
static bool is_valid_sha256_hex(const String& s) {
    if (s.length() != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}


// ---------------------------------------------------------------
// http_get_text
// Small HTTPS GET for a short text body (the ".sig" file). Returns the
// trimmed body in `out`. Integrity of the firmware itself is enforced by
// SHA-256 + the signature check, so setInsecure() is fine here too.
// ---------------------------------------------------------------
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


// ---------------------------------------------------------------
// ota_remote_run -- the actual flow.
// ---------------------------------------------------------------
OtaResult ota_remote_run(const String& url, const String& sha256_hex) {

    LOG.println();
    LOG.println("========================================");
    LOG.println("  REMOTE OTA -- starting");
    LOG.printf ("  URL    : %s\n", url.c_str());
    LOG.printf ("  SHA256 : %s\n", sha256_hex.c_str());
    LOG.println("========================================");

    // ---- Policy gates ---------------------------------------------------
    if (!url.startsWith("https://")) {
        LOG.println("[OTA] REJECT: URL is not HTTPS.");
        return OTA_ERR_URL_NOT_HTTPS;
    }
    if (!is_valid_sha256_hex(sha256_hex)) {
        LOG.println("[OTA] REJECT: SHA-256 must be 64 lowercase hex chars.");
        return OTA_ERR_SHA256_FORMAT;
    }

    CommissionData cd;
    commission_load(cd);
    if (cd.wifi_ssid[0] == '\0') {
        LOG.println("[OTA] ABORT: no dongle WiFi credentials in NVS.");
        return OTA_ERR_NOT_COMMISSIONED;
    }

    // ---- WiFi (WPA2 enforced) -------------------------------------------
    if (!wifi_connect_secure(cd.wifi_ssid, cd.wifi_pass, OTA_WIFI_TIMEOUT_MS)) {
        return OTA_ERR_WIFI_FAILED;
    }

    // ---- HTTPS GET ------------------------------------------------------
    WiFiClientSecure tls;
    tls.setInsecure();   // skip cert pin; integrity is enforced by SHA-256
    tls.setTimeout(OTA_HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);
    http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
    if (!http.begin(tls, url)) {
        LOG.println("[OTA] ERROR: http.begin() failed.");
        wifi_off();
        return OTA_ERR_HTTP_OPEN_FAILED;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        LOG.printf("[OTA] HTTP status %d -- aborting.\n", code);
        http.end();
        wifi_off();
        return OTA_ERR_HTTP_BAD_STATUS;
    }

    int total = http.getSize();
    if (total <= 0) {
        LOG.println("[OTA] ERROR: server did not send Content-Length.");
        http.end();
        wifi_off();
        return OTA_ERR_HTTP_NO_LENGTH;
    }
    if ((unsigned)total > OTA_MAX_FIRMWARE_BYTES) {
        LOG.printf("[OTA] REJECT: firmware too large (%d > %d).\n",
                      total, (int)OTA_MAX_FIRMWARE_BYTES);
        http.end();
        wifi_off();
        return OTA_ERR_FIRMWARE_TOO_LARGE;
    }

    LOG.printf("[OTA] Server returned %d bytes. Begin streaming...\n", total);
    if (!Update.begin((size_t)total)) {
        LOG.printf("[OTA] Update.begin() failed: %s\n", Update.errorString());
        http.end();
        wifi_off();
        return OTA_ERR_UPDATE_BEGIN_FAILED;
    }

    // ---- Stream + hash in one pass --------------------------------------
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[OTA_STREAM_CHUNK_SIZE];
    int    received = 0;
    uint32_t last_pct = 0;
    uint32_t last_progress_ms = millis();

    while (http.connected() && received < total) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - last_progress_ms > OTA_HTTP_TIMEOUT_MS) {
                LOG.println("[OTA] ERROR: stream stalled.");
                Update.abort();
                mbedtls_sha256_free(&sha_ctx);
                http.end();
                wifi_off();
                return OTA_ERR_STREAM_STALLED;
            }
            delay(20);
            continue;
        }

        size_t want = (avail > sizeof(buf)) ? sizeof(buf) : avail;
        int got = stream->readBytes(buf, want);
        if (got <= 0) { delay(20); continue; }

        mbedtls_sha256_update(&sha_ctx, buf, got);
        size_t written = Update.write(buf, got);
        if (written != (size_t)got) {
            LOG.printf("[OTA] ERROR: Update.write() short (%u != %d): %s\n",
                          (unsigned)written, got, Update.errorString());
            Update.abort();
            mbedtls_sha256_free(&sha_ctx);
            http.end();
            wifi_off();
            return OTA_ERR_UPDATE_BEGIN_FAILED;
        }

        received += got;
        last_progress_ms = millis();

        uint32_t pct = (uint32_t)((int64_t)received * 100 / total);
        if (pct >= last_pct + 10) {
            LOG.printf("[OTA] progress: %lu%% (%d / %d)\n",
                          (unsigned long)pct, received, total);
            last_pct = pct;
        }
    }

    http.end();

    // ---- Verify SHA-256 -------------------------------------------------
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    char digest_hex[65];
    hex_lower(digest, sizeof(digest), digest_hex);

    LOG.printf("[OTA] Computed SHA256: %s\n", digest_hex);
    LOG.printf("[OTA] Expected SHA256: %s\n", sha256_hex.c_str());

    if (sha256_hex != digest_hex) {
        LOG.println("[OTA] REJECT: SHA-256 mismatch -- aborting.");
        Update.abort();
        wifi_off();
        return OTA_ERR_SHA256_MISMATCH;
    }

    // ---- Fetch the detached signature ("<url>.sig") ---------------------
    // Too big to relay over ESP-NOW, so we pull it over the same HTTPS
    // channel. WiFi is still up here (wifi_off() has not run yet).
    String sig;
    if (!http_get_text(url + ".sig", sig, 1024)) {
        LOG.println("[OTA] ERROR: could not download <url>.sig -- aborting.");
        Update.abort();
        wifi_off();
        return OTA_ERR_SIG_FETCH_FAILED;
    }

    // ---- Verify the firmware was signed by US ---------------------------
    if (!crypto_verify_signature(digest, sizeof(digest), sig.c_str())) {
        LOG.println("[OTA] REJECT: firmware signature invalid -- aborting.");
        Update.abort();
        wifi_off();
        return OTA_ERR_BAD_SIGNATURE;
    }

    if (!Update.end(true)) {
        LOG.printf("[OTA] Update.end() failed: %s\n", Update.errorString());
        wifi_off();
        return OTA_ERR_UPDATE_END_FAILED;
    }

    LOG.println("[OTA] SUCCESS -- restarting into new firmware in 1 s.");
    wifi_off();
    delay(1000);
    ESP.restart();
    return OTA_OK;     // unreachable, but keeps the compiler happy
}
