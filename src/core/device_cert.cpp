// core/device_cert.cpp
//
// Per-device X.509 identity that lives ENTIRELY on the chip.
// See device_cert.h for the full "why". Short version:
//   first boot  -> generate an EC P-256 keypair, store the private key in NVS
//   pairing     -> build a CSR from that key (phone reads it over BLE)
//   pairing     -> store the signed certificate the backend sends back
//   gateway run -> hand the cert + key to the TLS client for mTLS
//
// The private key is written to NVS once and never leaves the device.

#include "device_cert.h"
#include "log.h"
#include "identity.h"

#include <Preferences.h>
#include <string.h>

#include <mbedtls/version.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/x509_crt.h>


// NVS namespace + keys where the key/cert PEM strings live.
static const char* NS       = "devcert";
static const char* KEY_PRIV = "pk";    // private key  (PEM)
static const char* KEY_CERT = "crt";   // signed cert  (PEM)

// Buffers kept alive so the TLS client can point at them after load.
static String s_key_pem;
static String s_cert_pem;


// ============================================================
// Tiny helpers
// ============================================================

// Seed a random-number generator from the ESP32 hardware entropy source.
// Both the keygen and the CSR signing need randomness.
static int seed_rng(mbedtls_entropy_context* ent,
                    mbedtls_ctr_drbg_context* rng) {
    mbedtls_entropy_init(ent);
    mbedtls_ctr_drbg_init(rng);
    const char* pers = "achas-device-cert";
    return mbedtls_ctr_drbg_seed(rng, mbedtls_entropy_func, ent,
                                 (const unsigned char*)pers, strlen(pers));
}

// Read a PEM string from NVS; returns "" if the key is missing.
static String nvs_get(const char* key) {
    Preferences p;
    if (!p.begin(NS, true)) return String("");
    String v = p.getString(key, "");
    p.end();
    return v;
}

// Write a PEM string to NVS.
static bool nvs_put(const char* key, const String& val) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = (p.putString(key, val) == val.length());
    p.end();
    return ok;
}

// Parse a private-key PEM into `key`. mbedTLS changed this function's
// signature between v2 and v3 (v3 added an RNG argument), so we branch.
static int parse_private_key(mbedtls_pk_context* key,
                             const String& pem,
                             mbedtls_ctr_drbg_context* rng) {
#if MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_pk_parse_key(
        key, (const unsigned char*)pem.c_str(), pem.length() + 1,
        nullptr, 0, mbedtls_ctr_drbg_random, rng);
#else
    (void)rng;
    return mbedtls_pk_parse_key(
        key, (const unsigned char*)pem.c_str(), pem.length() + 1,
        nullptr, 0);
#endif
}


// ============================================================
// device_cert_init -- make the keypair exactly once
// ============================================================
bool device_cert_init() {

    if (nvs_get(KEY_PRIV).length() > 0) {
        LOG.println("[DevCert] Private key present (chip already has its identity).");
        return true;
    }

    LOG.println("[DevCert] No key yet -- generating EC P-256 keypair (one time)...");

    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context       key;
    mbedtls_pk_init(&key);

    unsigned char pem[2048];
    bool ok = false;

    int rc = seed_rng(&ent, &rng);
    if (rc == 0) {
        rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    }
    if (rc == 0) {
        rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                                 mbedtls_ctr_drbg_random, &rng);
    }
    if (rc == 0) {
        memset(pem, 0, sizeof(pem));
        rc = mbedtls_pk_write_key_pem(&key, pem, sizeof(pem));
    }
    if (rc == 0) {
        ok = nvs_put(KEY_PRIV, String((char*)pem));
    }

    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);

    if (ok) LOG.println("[DevCert] Keypair generated + stored. Private key never leaves NVS.");
    else    LOG.printf("[DevCert] ERROR: keygen failed rc=-0x%04x\n", -rc);
    return ok;
}


// ============================================================
// device_cert_has_cert
// ============================================================
bool device_cert_has_cert() {
    return nvs_get(KEY_CERT).length() > 0;
}


// ============================================================
// device_cert_build_csr -- "here is my public key, sign it please"
// ============================================================
bool device_cert_build_csr(char* out, size_t out_max) {

    String key_pem = nvs_get(KEY_PRIV);
    if (key_pem.length() == 0) {
        LOG.println("[DevCert] ERROR: no private key -- call device_cert_init() first.");
        return false;
    }

    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context       key;
    mbedtls_x509write_csr    req;
    mbedtls_pk_init(&key);
    mbedtls_x509write_csr_init(&req);

    bool ok = false;
    int  rc = seed_rng(&ent, &rng);

    if (rc == 0) rc = parse_private_key(&key, key_pem, &rng);

    // Subject Common Name = this chip's unique id, e.g. "CN=achas-A3F1".
    char subject[40];
    snprintf(subject, sizeof(subject), "CN=achas-%s", identity_id_hex());

    if (rc == 0) rc = mbedtls_x509write_csr_set_subject_name(&req, subject);
    if (rc == 0) {
        mbedtls_x509write_csr_set_key(&req, &key);
        mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);

        memset(out, 0, out_max);
        rc = mbedtls_x509write_csr_pem(&req, (unsigned char*)out, out_max,
                                       mbedtls_ctr_drbg_random, &rng);
        ok = (rc == 0);
    }

    mbedtls_x509write_csr_free(&req);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);

    if (ok) LOG.println("[DevCert] CSR built (subject = this chip's id).");
    else    LOG.printf("[DevCert] ERROR: CSR build failed rc=-0x%04x\n", -rc);
    return ok;
}


// ============================================================
// device_cert_store_cert -- save the signed certificate from BLE
// ============================================================
bool device_cert_store_cert(const char* cert_pem) {

    if (cert_pem == nullptr ||
        strstr(cert_pem, "-----BEGIN CERTIFICATE-----") == nullptr) {
        LOG.println("[DevCert] ERROR: not a PEM certificate.");
        return false;
    }

    // Sanity check: does it parse as a real X.509 certificate?
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int rc = mbedtls_x509_crt_parse(
        &crt, (const unsigned char*)cert_pem, strlen(cert_pem) + 1);
    mbedtls_x509_crt_free(&crt);
    if (rc != 0) {
        LOG.printf("[DevCert] ERROR: certificate did not parse rc=-0x%04x\n", -rc);
        return false;
    }

    if (!nvs_put(KEY_CERT, String(cert_pem))) {
        LOG.println("[DevCert] ERROR: could not store certificate in NVS.");
        return false;
    }

    LOG.println("[DevCert] Signed certificate stored. Device now has its X.509 identity.");
    return true;
}


// ============================================================
// device_cert_load_tls -- give the TLS client cert + key pointers
// ============================================================
bool device_cert_load_tls(const char** out_cert_pem,
                          const char** out_key_pem) {

    s_key_pem  = nvs_get(KEY_PRIV);
    s_cert_pem = nvs_get(KEY_CERT);

    if (s_key_pem.length() == 0 || s_cert_pem.length() == 0) {
        LOG.println("[DevCert] ERROR: device not provisioned (missing key or cert).");
        return false;
    }

    if (out_cert_pem) *out_cert_pem = s_cert_pem.c_str();
    if (out_key_pem)  *out_key_pem  = s_key_pem.c_str();
    return true;
}


// ============================================================
// device_cert_export_pubkey -- hand the phone our PUBLIC key
//
// The phone needs our public key to set up an ECDH-encrypted channel
// before it sends the secret commissioning data. The public key is, by
// definition, safe to share. We output it as a raw uncompressed point
// (0x04 || X || Y = 65 bytes) because every phone crypto library can
// import that format directly.
// ============================================================
bool device_cert_export_pubkey(uint8_t* out, size_t out_max, size_t* out_len) {

    String key_pem = nvs_get(KEY_PRIV);
    if (key_pem.length() == 0) {
        LOG.println("[DevCert] ERROR: no private key -- cannot export public key.");
        return false;
    }

    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context       key;
    mbedtls_pk_init(&key);

    bool ok = false;
    int  rc = seed_rng(&ent, &rng);
    if (rc == 0) rc = parse_private_key(&key, key_pem, &rng);
    if (rc == 0) {
        mbedtls_ecp_keypair* kp = mbedtls_pk_ec(key);
        size_t olen = 0;
        rc = mbedtls_ecp_point_write_binary(&kp->grp, &kp->Q,
                                            MBEDTLS_ECP_PF_UNCOMPRESSED,
                                            &olen, out, out_max);
        if (rc == 0) {
            if (out_len) *out_len = olen;
            ok = true;
        }
    }

    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);

    if (!ok) LOG.printf("[DevCert] ERROR: pubkey export failed rc=-0x%04x\n", -rc);
    return ok;
}


// ============================================================
// device_cert_ecdh -- agree on a shared secret with the phone
//
// shared_secret = ECDH(our_private_key, phone_public_key)
// Both sides compute the SAME 32-byte value without ever sending a
// private key. prov_session.cpp then runs this through HKDF to get the
// AES key that encrypts the commissioning data.
// ============================================================
bool device_cert_ecdh(const uint8_t* peer_point, size_t peer_len,
                      uint8_t out_shared[32]) {

    String key_pem = nvs_get(KEY_PRIV);
    if (key_pem.length() == 0) {
        LOG.println("[DevCert] ERROR: no private key -- cannot run ECDH.");
        return false;
    }

    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context       key;
    mbedtls_ecp_point        peer_q;
    mbedtls_mpi              shared_z;
    mbedtls_pk_init(&key);
    mbedtls_ecp_point_init(&peer_q);
    mbedtls_mpi_init(&shared_z);

    bool ok = false;
    int  rc = seed_rng(&ent, &rng);
    if (rc == 0) rc = parse_private_key(&key, key_pem, &rng);
    if (rc == 0) {
        mbedtls_ecp_keypair* kp = mbedtls_pk_ec(key);
        // 1. Read the phone's public key (a point on the same curve).
        rc = mbedtls_ecp_point_read_binary(&kp->grp, &peer_q, peer_point, peer_len);
        // 2. shared = our_private_d * phone_Q  (take the X coordinate).
        if (rc == 0) {
            rc = mbedtls_ecdh_compute_shared(&kp->grp, &shared_z,
                                             &peer_q, &kp->d,
                                             mbedtls_ctr_drbg_random, &rng);
        }
        // 3. Serialise the X coordinate to a fixed 32 bytes.
        if (rc == 0) rc = mbedtls_mpi_write_binary(&shared_z, out_shared, 32);
        ok = (rc == 0);
    }

    mbedtls_mpi_free(&shared_z);
    mbedtls_ecp_point_free(&peer_q);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);

    if (!ok) LOG.printf("[DevCert] ERROR: ECDH failed rc=-0x%04x\n", -rc);
    return ok;
}


// ============================================================
// device_cert_wipe
// ============================================================
bool device_cert_wipe() {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.clear();
    p.end();
    if (ok) LOG.println("[DevCert] key + cert wiped.");
    return ok;
}
