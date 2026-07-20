/*
  ============================================================
   core/device_cert.h  -- this device's OWN X.509 identity
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Each device makes its OWN private key INSIDE the chip on first boot.
  The private key NEVER leaves the chip. We then build a CSR
  ("Certificate Signing Request" = "here is my PUBLIC key, please give
  me a certificate"), hand the CSR to the phone over BLE, the backend CA
  signs it, and the signed certificate comes back over BLE.

  WHY WE DO IT THIS WAY
  ---------------------
    * ONE identical firmware can be flashed to every device
      (there is NO per-device secret baked into the binary).
    * Every device still gets a UNIQUE identity, because the key is
      generated randomly inside each chip.
    * "one device cracked != all devices cracked": the private key is
      never copied anywhere, never sent over the air, never on a server.

  WHERE IT IS USED
  ----------------
    * device_cert_init()        -> main.cpp at boot   (make the key once)
    * device_cert_build_csr()   -> ble_pairing.cpp    (phone READS the CSR)
    * device_cert_store_cert()  -> ble_pairing.cpp    (phone WRITES the cert)
    * device_cert_load_tls()    -> mqtt_handler.cpp   (mTLS to the broker)

  KEY TYPE: EC P-256 (secp256r1). It is small + fast on the ESP32-C3, so
  first-boot key generation takes only ~1-2 s and the CSR/cert are small
  enough to travel over BLE comfortably.
*/

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>


// Make sure this chip has a private key. Generates one (and saves it to
// NVS) the very first time only; on later boots it just confirms a key
// exists. Safe to call on every boot.
bool device_cert_init();

// True once a signed certificate from the backend CA has been stored.
bool device_cert_has_cert();

// Build this device's CSR as PEM text into `out`. Returns false on error
// or if `out` is too small. This is heavy crypto -- call it from the
// MAIN task, never from inside a BLE callback.
bool device_cert_build_csr(char* out, size_t out_max);

// Validate and store a signed certificate (PEM text) received over BLE.
// Rejects anything that is not a parseable X.509 certificate.
bool device_cert_store_cert(const char* cert_pem);

// Load the private key + signed certificate from NVS into memory so the
// TLS client can point at them. Returns false if no certificate has been
// provisioned yet. The returned pointers stay valid until the next call.
bool device_cert_load_tls(const char** out_cert_pem,
                          const char** out_key_pem);


// ---- Secure BLE provisioning helpers (see prov_session.h) ----
//
// Both reuse THIS chip's identity keypair, but NEVER expose the private
// key. They let the phone set up an encrypted channel (ECDH) before it
// sends the secret commissioning data (house_key, wifi password).

// Write this device's PUBLIC key as a raw uncompressed EC point
// (0x04 || X(32) || Y(32) = 65 bytes) into `out`. The phone reads this
// (over BLE char aca8) to perform the ECDH key agreement. Returns false
// if there is no keypair yet or the buffer is too small.
bool device_cert_export_pubkey(uint8_t* out, size_t out_max, size_t* out_len);

// Compute the ECDH shared secret between THIS device's private key and
// the phone's public key (`peer_point`, also a 65-byte uncompressed EC
// point). Writes the 32-byte shared secret (the X coordinate) to
// `out_shared`. The private key never leaves the chip. Returns true on
// success.
bool device_cert_ecdh(const uint8_t* peer_point, size_t peer_len,
                      uint8_t out_shared[32]);

// Forget the stored key + certificate (used when re-provisioning).
bool device_cert_wipe();
