/*
  ============================================================
   core/prov_session.h  -- the ENCRYPTED channel used during pairing
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  When the phone commissions a device it has to send SECRET values:
      * the per-house AES key (house_key)
      * the WiFi password
  If those travelled over BLE in the clear, anyone running nRF Connect
  nearby could sniff them -- and the house_key unlocks ALL of that house's
  data. That would break the whole "one device cracked != all cracked"
  promise.

  So before any secret is sent, the phone and the device agree on a
  one-time AES key that ONLY the two of them know:

      1. The device already has an EC P-256 keypair (see device_cert.h).
      2. The phone reads the device's PUBLIC key over BLE (char aca8).
      3. The phone makes its OWN throw-away EC keypair and sends its
         public key to the device (char aca9).
      4. Both sides run ECDH -> the SAME 32-byte shared secret, which an
         eavesdropper cannot compute (they never see either private key).
      5. HKDF-SHA256 turns that secret into a 16-byte AES key.
      6. The phone encrypts the commissioning JSON with AES-128-GCM and
         sends it (char aca5). The device decrypts it here.

  WHERE IT IS USED
  ----------------
    * prov_session_start()  -> ble_pairing.cpp, when the phone writes its
                               public key (char aca9).
    * prov_session_open()   -> ble_pairing.cpp, to decrypt the commission
                               blob (char aca5).
    * prov_session_reset()  -> ble_pairing.cpp, when pairing ends.

  WIRE FORMAT of the encrypted commission blob (before base64):
        IV (12 bytes) || ciphertext (N) || GCM tag (16 bytes)
  This matches core/crypto.h so the app side is identical in style.
*/

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>


// Derive the one-time AES session key from the phone's public key
// (ECDH + HKDF). Call this once, when the phone writes its public key.
// `peer_pub` is the phone's public key as a raw uncompressed EC point
// (0x04 || X || Y = 65 bytes). Returns false if the device has no
// keypair yet or the public key is malformed.
bool prov_session_start(const uint8_t* peer_pub, size_t peer_len);

// True once a session key has been derived (i.e. it is now safe to
// receive encrypted commissioning data).
bool prov_session_active();

// Decrypt one commissioning blob the phone sent (AES-128-GCM):
//     in = IV(12) || ciphertext || tag(16)
// Writes the plaintext to `out` and returns its length, or 0 on any
// failure (no session, wrong key, tampered data, buffer too small).
// Fails CLOSED -- a bad tag yields 0, never partial plaintext.
size_t prov_session_open(const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t out_max);

// Wipe the session key from RAM and mark the session inactive. Call this
// when the pairing window ends or a new pairing starts.
void prov_session_reset();
