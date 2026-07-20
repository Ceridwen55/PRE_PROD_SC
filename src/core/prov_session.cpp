// core/prov_session.cpp
//
// The encrypted channel used during BLE commissioning. See
// prov_session.h for the full "why" and the step-by-step handshake.
//
// Short version:
//   prov_session_start() : ECDH(our key, phone key) -> HKDF -> AES key
//   prov_session_open()  : AES-128-GCM decrypt the commission blob
//   prov_session_reset() : forget the key

#include "prov_session.h"
#include "log.h"
#include "device_cert.h"

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <string.h>


// Same sizes as core/crypto.h so both ends speak the same dialect.
#define PROV_KEY_SIZE   16    // AES-128
#define PROV_IV_SIZE    12
#define PROV_TAG_SIZE   16

// HKDF "info" label. The phone MUST use the exact same bytes, or the two
// sides derive different keys and every decrypt fails (closed).
static const char* HKDF_INFO = "ACHAS-PROV-v1";

static uint8_t s_session_key[PROV_KEY_SIZE];
static bool    s_active = false;


// ------------------------------------------------------------
// HKDF-SHA256 (RFC 5869), written out with plain HMAC so we do not
// depend on mbedtls_hkdf (which is not compiled into the Arduino build).
// We only ever need <= 32 output bytes, so a single expand block is enough.
//
//   PRK = HMAC-SHA256(salt, IKM)            (salt absent => 32 zero bytes)
//   OKM = HMAC-SHA256(PRK, info || 0x01)    (first `out_len` bytes)
//
// Any standard HKDF library on the phone, with salt = empty and the same
// `info`, produces the identical key.
// ------------------------------------------------------------
static int hkdf_sha256(const uint8_t* ikm,  size_t ikm_len,
                       const uint8_t* info, size_t info_len,
                       uint8_t* out, size_t out_len) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == nullptr || out_len > 32 || info_len > 64) return -1;

    uint8_t zero_salt[32] = {0};
    uint8_t prk[32];
    int rc = mbedtls_md_hmac(md, zero_salt, sizeof(zero_salt),
                             ikm, ikm_len, prk);
    if (rc != 0) return rc;

    uint8_t block_in[64 + 1];
    memcpy(block_in, info, info_len);
    block_in[info_len] = 0x01;                 // T(1) counter byte

    uint8_t okm[32];
    rc = mbedtls_md_hmac(md, prk, sizeof(prk), block_in, info_len + 1, okm);

    memset(prk, 0, sizeof(prk));
    if (rc != 0) return rc;

    memcpy(out, okm, out_len);
    memset(okm, 0, sizeof(okm));
    return 0;
}


// ============================================================
// prov_session_start -- ECDH + HKDF -> one-time AES key
// ============================================================
bool prov_session_start(const uint8_t* peer_pub, size_t peer_len) {

    s_active = false;

    // 1. ECDH: the shared secret only we and the phone can compute.
    uint8_t shared[32];
    if (!device_cert_ecdh(peer_pub, peer_len, shared)) {
        LOG.println("[Prov] ERROR: ECDH failed (missing key or bad public key).");
        return false;
    }

    // 2. HKDF-SHA256(shared) -> 16-byte AES key. info = the version label.
    int rc = hkdf_sha256(shared, sizeof(shared),
                         (const uint8_t*)HKDF_INFO, strlen(HKDF_INFO),
                         s_session_key, PROV_KEY_SIZE);

    // 3. Wipe the raw shared secret the moment we are done with it.
    memset(shared, 0, sizeof(shared));

    if (rc != 0) {
        LOG.printf("[Prov] ERROR: HKDF failed rc=-0x%04x\n", -rc);
        return false;
    }

    s_active = true;
    LOG.println("[Prov] Secure session established (ECDH + HKDF).");
    LOG.println("[Prov] Commissioning secrets are now end-to-end encrypted.");
    return true;
}


// ============================================================
// prov_session_active
// ============================================================
bool prov_session_active() { return s_active; }


// ============================================================
// prov_session_open -- AES-128-GCM decrypt (fails closed)
// ============================================================
size_t prov_session_open(const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t out_max) {

    if (!s_active) { LOG.println("[Prov] ERROR: open before session start."); return 0; }
    if (in == nullptr || out == nullptr)        return 0;
    if (in_len < PROV_IV_SIZE + PROV_TAG_SIZE)  return 0;

    size_t cipher_len = in_len - PROV_IV_SIZE - PROV_TAG_SIZE;
    if (out_max < cipher_len) return 0;

    const uint8_t* iv     = in;
    const uint8_t* cipher = in + PROV_IV_SIZE;
    const uint8_t* tag    = in + PROV_IV_SIZE + cipher_len;

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES,
                                s_session_key, PROV_KEY_SIZE * 8);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&ctx, cipher_len,
                                      iv, PROV_IV_SIZE,
                                      nullptr, 0,            // no AAD
                                      tag, PROV_TAG_SIZE,
                                      cipher, out);
    }
    mbedtls_gcm_free(&ctx);

    if (rc != 0) {
        LOG.println("[Prov] decrypt failed (wrong session key or tampered data).");
        return 0;
    }
    return cipher_len;
}


// ============================================================
// prov_session_reset
// ============================================================
void prov_session_reset() {
    memset(s_session_key, 0, sizeof(s_session_key));
    s_active = false;
}
