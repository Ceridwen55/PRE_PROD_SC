# ACHAS Pre-Production Firmware — Changelog

Firmware version: **4.1.0**
Merged image SHA-256: **`6033a1ba7ee1051491681dcf7d13981863d2f1d0bce0a9bde6fe2e38d40cfd2e`**

This lists everything changed from the original PREPROD (Portal Provisioning)
files, for this production build.

---

## Firmware changes

### 1. `src/main.cpp` — button reset + power/timing
- **Button (GPIO10) reworked into 3 hold zones**, decided on release with LED
  feedback so the installer knows which zone they're in:
  - `< 3 s`  → normal boot
  - `3–10 s` → BLE pairing (unchanged behaviour) — STATUS LED blinks **1×**
  - `≥ 10 s` → **wipe commissioning + reboot into the web portal** — blinks **3×**
  - Only the `commission` NVS namespace is cleared; the device's private key +
    certificate (`devcert`) are kept, so re-provisioning does not mint a new cert.
- **Deep-sleep anti-collision jitter**: each sleep now adds a random `0–30 s`
  (hardware RNG) on top of the base, so multiple sensors in one house never stay
  phase-locked and their ESP-NOW transmissions don't collide.
- Added `#include <esp_random.h>`.

### 2. `src/config.h` — 15-minute production cycle
- `DEEP_SLEEP_DURATION_US`: `115000000` (~2 min) → **`895000000` (~15 min)**.
- Added `DEEP_SLEEP_JITTER_US = 30000000` (0–30 s anti-collision jitter; set 0 to
  disable).

### 3. `src/core/portal_provision.cpp` — gateway chooses its House ID
- Gateway setup form now has a **House ID** field (`house_id_gw`); the installer
  picks the number instead of the server auto-assigning it.
- `handleSubmit()` reads and validates it (1–65535) for the gateway role.
- `api_create_house()` sends the chosen `house_id` to the Fleet API.
- Result: no more duplicate/orphan houses on re-provision — provisioning the same
  gateway with the same House ID reuses that house (idempotent).

### 4. Backend `backend/api/handlers.go` (separate repo — deploy on the VPS)
- `POST /houses` now accepts an **optional `house_id`**:
  - omitted → auto-increment (original behaviour, backward-compatible);
  - given & existing → reuse the house, key unchanged (idempotent);
  - given & new → create with exactly that number, and bump the Postgres SERIAL
    sequence so future auto-inserts can't collide.
- **Ordering:** deploy this backend change **before** flashing firmware that sends
  a House ID. (It is backward-compatible, so old firmware keeps working.)

---

## Unchanged (verified intact)

OTA (self + relay + sensor), AES-128-GCM crypto, RSA-2048 OTA signature check,
device certificate / mTLS, WiFi WPA2 policy, ESP-NOW transport, anti-replay seq,
payload build/validate — **not modified**, and the build compiles all of them
cleanly.

---

## Security posture (unchanged, intended for pre-production)

- **Flash encryption + Secure Boot: OFF** — so any unit can be re-flashed or fully
  erased over USB during pre-production. Enable the eFuse hardening only for the
  real production run.
- Per-device private key generated on-chip, never leaves NVS.
- Fleet API token typed per session, never stored on the device.

---

## Build

```
Environment : preprod  (ESP32-C3, Arduino)
Flash usage : ~89.9% of the ~1.97 MB app slot
Encryption  : none  (flash & secure-boot eFuses untouched)
```
