// core/crypto.cpp  (GATEWAY) -- mirror of the sensor box crypto module.

#include "crypto.h"
#include "log.h"
#include "commission.h"
#include "certs.h"

#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <string.h>


static uint8_t g_house_key[AES_KEY_SIZE];
static bool    g_ready = false;


bool crypto_init(uint16_t house_id) {

    g_ready = false;
    if (house_id == 0) {
        LOG.println("[Crypto] ERROR: house_id is 0 -- not commissioned.");
        return false;
    }

    CommissionData cd;
    if (!commission_load(cd) || !cd.house_key_set) {
        LOG.println("[Crypto] ERROR: house_key not provisioned -- commission device via app first.");
        return false;
    }

    memcpy(g_house_key, cd.house_key, AES_KEY_SIZE);
    g_ready = true;

    LOG.println();
    LOG.println("================ CRYPTO INIT (AES-128-GCM) ===========");
    LOG.printf( "  House ID   : %u\n", house_id);
    LOG.printf( "  Key source : NVS (server-provisioned via BLE)\n");
    LOG.printf( "  Mode       : AES-128-GCM\n");
    LOG.printf( "  IV / tag   : %d / %d B\n", AES_IV_SIZE, AES_TAG_SIZE);
    LOG.println("============== CRYPTO READY ==========================");
    return true;
}


size_t crypto_encrypt(const uint8_t* plain, size_t plain_len,
                      uint8_t* out, size_t out_max) {

    if (!g_ready) { LOG.println("[Crypto] ERROR: encrypt before init."); return 0; }
    if (plain == nullptr || out == nullptr) return 0;
    if (out_max < plain_len + AES_ENVELOPE_OVERHEAD) return 0;

    uint8_t* iv     = out;
    uint8_t* cipher = out + AES_IV_SIZE;
    uint8_t* tag    = cipher + plain_len;

    esp_fill_random(iv, AES_IV_SIZE);

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, g_house_key,
                                AES_KEY_SIZE * 8);
    if (rc != 0) { mbedtls_gcm_free(&ctx); return 0; }

    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                   plain_len,
                                   iv, AES_IV_SIZE,
                                   nullptr, 0,
                                   plain, cipher,
                                   AES_TAG_SIZE, tag);
    mbedtls_gcm_free(&ctx);
    if (rc != 0) return 0;
    return AES_IV_SIZE + plain_len + AES_TAG_SIZE;
}


size_t crypto_decrypt(const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t out_max) {

    if (!g_ready) { LOG.println("[Crypto] ERROR: decrypt before init."); return 0; }
    if (in == nullptr || out == nullptr) return 0;
    if (in_len < AES_ENVELOPE_OVERHEAD)   return 0;

    size_t cipher_len = in_len - AES_ENVELOPE_OVERHEAD;
    if (out_max < cipher_len) return 0;

    const uint8_t* iv     = in;
    const uint8_t* cipher = in + AES_IV_SIZE;
    const uint8_t* tag    = in + AES_IV_SIZE + cipher_len;

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, g_house_key,
                                AES_KEY_SIZE * 8);
    if (rc != 0) { mbedtls_gcm_free(&ctx); return 0; }

    rc = mbedtls_gcm_auth_decrypt(&ctx,
                                  cipher_len,
                                  iv, AES_IV_SIZE,
                                  nullptr, 0,
                                  tag, AES_TAG_SIZE,
                                  cipher, out);
    mbedtls_gcm_free(&ctx);

    if (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) {
        LOG.println("[Crypto] decrypt: auth tag mismatch (wrong key or tampered).");
        return 0;
    }
    if (rc != 0) return 0;
    return cipher_len;
}


bool crypto_verify_signature(const uint8_t* digest, size_t digest_len,
                             const char* sig_base64) {

    if (sig_base64 == nullptr || sig_base64[0] == '\0') {
        LOG.println("[Crypto] OTA signature missing.");
        return false;
    }

    // Step 1: base64-decode the signature (RSA-2048 sig = 256 bytes).
    uint8_t sig[512];
    size_t  sig_len = 0;
    int rc = mbedtls_base64_decode(sig, sizeof(sig), &sig_len,
                                   (const unsigned char*)sig_base64,
                                   strlen(sig_base64));
    if (rc != 0) {
        LOG.println("[Crypto] OTA signature: base64 decode failed.");
        return false;
    }

    // Step 2: load our public key from certs.h.
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    rc = mbedtls_pk_parse_public_key(
            &pk,
            (const unsigned char*)OTA_SIGNING_PUBLIC_KEY,
            strlen(OTA_SIGNING_PUBLIC_KEY) + 1);   // +1: include the NUL for PEM
    if (rc != 0) {
        LOG.printf("[Crypto] OTA public key parse failed: -0x%04x\n", -rc);
        mbedtls_pk_free(&pk);
        return false;
    }

    // Step 3: verify the signature over the firmware's SHA-256 digest.
    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, digest_len,
                           sig, sig_len);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        LOG.printf("[Crypto] OTA signature INVALID: -0x%04x\n", -rc);
        return false;
    }
    LOG.println("[Crypto] OTA signature OK (firmware is authentic).");
    return true;
}


// ------------------------------------------------------------------
// ESP-NOW downlink command authentication (HMAC-SHA256, house-key).
// ------------------------------------------------------------------

// Raw 32-byte HMAC-SHA256(house_key, seq_le32 || msg).
static bool downlink_hmac_raw(uint32_t seq, const char* msg, uint8_t out[32]) {
    if (!g_ready || msg == nullptr) return false;
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == nullptr) return false;

    uint8_t seq_le[4] = { (uint8_t)(seq),        (uint8_t)(seq >> 8),
                          (uint8_t)(seq >> 16),  (uint8_t)(seq >> 24) };

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    bool ok = mbedtls_md_setup(&ctx, md, 1) == 0            // 1 = HMAC
           && mbedtls_md_hmac_starts(&ctx, g_house_key, AES_KEY_SIZE) == 0
           && mbedtls_md_hmac_update(&ctx, seq_le, sizeof(seq_le)) == 0
           && mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, strlen(msg)) == 0
           && mbedtls_md_hmac_finish(&ctx, out) == 0;
    mbedtls_md_free(&ctx);
    return ok;
}

bool crypto_downlink_tag(uint32_t seq, const char* msg, char* out_hex, size_t out_sz) {
    if (out_hex == nullptr || out_sz < DOWNLINK_TAG_HEX_LEN + 1) return false;
    uint8_t full[32];
    if (!downlink_hmac_raw(seq, msg, full)) return false;
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < DOWNLINK_TAG_HEX_LEN / 2; i++) {
        out_hex[2*i]   = hx[full[i] >> 4];
        out_hex[2*i+1] = hx[full[i] & 0x0F];
    }
    out_hex[DOWNLINK_TAG_HEX_LEN] = '\0';
    return true;
}

bool crypto_verify_downlink(uint32_t seq, const char* msg, const char* tag_hex) {
    if (tag_hex == nullptr) return false;
    char expect[DOWNLINK_TAG_HEX_LEN + 1];
    if (!crypto_downlink_tag(seq, msg, expect, sizeof(expect))) return false;
    if (strlen(tag_hex) != DOWNLINK_TAG_HEX_LEN) return false;
    // Constant-time compare (avoid a byte-by-byte early-out timing signal).
    uint8_t diff = 0;
    for (int i = 0; i < DOWNLINK_TAG_HEX_LEN; i++) diff |= (uint8_t)(expect[i] ^ tag_hex[i]);
    return diff == 0;
}


bool crypto_self_test() {

    if (!g_ready) return false;
    const char* msg = "ACHAS-TEST-VECTOR-OK!";
    size_t      msg_len = strlen(msg);

    uint8_t env[64];
    size_t env_len = crypto_encrypt((const uint8_t*)msg, msg_len,
                                    env, sizeof(env));
    if (env_len == 0) return false;

    uint8_t back[64];
    size_t back_len = crypto_decrypt(env, env_len, back, sizeof(back));
    if (back_len != msg_len || memcmp(back, msg, msg_len) != 0) return false;

    env[AES_IV_SIZE] ^= 0x01;
    size_t tampered = crypto_decrypt(env, env_len, back, sizeof(back));
    env[AES_IV_SIZE] ^= 0x01;
    if (tampered != 0) return false;

    LOG.println("[Crypto] self-test: PASS");
    LOG.println("[Crypto]   - encrypt + decrypt round-trip OK");
    LOG.println("[Crypto]   - 1-byte tamper correctly rejected");
    return true;
}
