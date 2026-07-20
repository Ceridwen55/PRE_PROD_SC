/*
  core/crypto.h  (GATEWAY)
  AES-128-GCM + HKDF-SHA256. BYTE-IDENTICAL to the sensor box's
  crypto.h. If you touch one, mirror the other in the same commit.

  See SensorBox V.3/src/core/crypto.h for the full design notes.
*/

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>


#define AES_KEY_SIZE            16
#define AES_IV_SIZE             12
#define AES_TAG_SIZE            16
#define AES_ENVELOPE_OVERHEAD   (AES_IV_SIZE + AES_TAG_SIZE)


bool   crypto_init(uint16_t house_id);
size_t crypto_encrypt(const uint8_t* plain, size_t plain_len,
                      uint8_t* out, size_t out_max);
size_t crypto_decrypt(const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t out_max);
bool   crypto_self_test();


// Verify an RSA signature (given as base64) over a SHA-256 `digest`,
// using the OTA signing PUBLIC key in certs.h. Returns true only if the
// signature is valid -- i.e. the firmware really was signed by us.
// Used by the OTA code before flashing a downloaded image.
bool   crypto_verify_signature(const uint8_t* digest, size_t digest_len,
                               const char* sig_base64);


// ------------------------------------------------------------------
// ESP-NOW downlink authentication (gateway -> sensor commands).
//
// The uplink (sensor -> gateway reading) is already AES-GCM encrypted.
// The downlink reply is plaintext, so a REBOOT/PAIR/OTA command could be
// spoofed by anyone in radio range. These two helpers seal a command with
// an 8-byte HMAC-SHA256 tag keyed by the shared HOUSE KEY, bound to the
// uplink's `seq` so a captured command cannot be replayed on a later wake.
// An attacker without the house key cannot forge the tag.
//
// Tag = first 8 bytes of HMAC-SHA256(house_key, seq_le32 || msg), hex.
// `out_hex` needs room for 16 chars + NUL (>= 17). Both return false if
// crypto is not initialised.
// ------------------------------------------------------------------
#define DOWNLINK_TAG_HEX_LEN   16   // 8 bytes -> 16 hex chars

bool   crypto_downlink_tag(uint32_t seq, const char* msg, char* out_hex, size_t out_sz);
bool   crypto_verify_downlink(uint32_t seq, const char* msg, const char* tag_hex);
