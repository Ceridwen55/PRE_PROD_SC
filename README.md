# ACHAS Pre-Production Firmware (Portal Provisioning)

Same firmware as **UNIFIED**, plus two pre-production extras so 50 houses
(50 gateways + 150 sensors) can be built and installed **without the phone
app**, from a **single `.bin` flashed to every board**:

1. **Power-on self-test (POST)** — LED codes tell the manufacturer if a
   module is bad, right after flashing.
2. **Web-portal provisioning** — a fresh box becomes a Wi-Fi hotspot with a
   small form; the box enrols itself against the Fleet API over HTTPS
   (real per-device certificate, real house key). No app needed.

> BLE app provisioning is **not removed** — hold the button at boot to use
> it. The portal is an additional, app-free path.

---

## 1. Build & flash (one binary for everything)

```
pio run -e preprod
pio run -e preprod -t upload
```
Flash the **same** binary to gateways and sensors. Role is chosen later, at
provisioning time — nothing to change per board.

---

## 2. Manufacturing self-test (LED codes)

On the first power-up after flashing (device is uncommissioned), the POST
runs automatically. Watch the **blue Status LED**:

| Status LED | Meaning |
|---|---|
| 1 blink, pause | SHT41 (temp/humidity) OK |
| 2 blinks, pause | Wi-Fi radio OK |
| 3 blinks, pause | BLE radio OK |
| 4 blinks, pause | Battery healthy (≥3.5 V) — skipped on gateway/no battery |
| Both LEDs ON 1 s | **All good** → portal opens next |

**Failure:** the Power LED blinks fast and the Status LED repeats **N**
blinks, where **N = the failed test** (1 = SHT41, 2 = Wi-Fi, 3 = BLE), then
the board halts. That board has a bad module — set it aside.

---

## 3. Provisioning (installer, phone browser)

Each box makes a hotspot named **`ACHAS-PROV-<ID>`**, where `<ID>` is
printed on the box label — so you always know which box you joined. The
form also has an **Identify** button that blinks the box's LED to confirm
the physical unit.

**Do the GATEWAY first, then its 3 sensors.**

### Gateway
1. Connect your phone to `ACHAS-PROV-<gateway-ID>`; the form opens.
2. Role = **Gateway**. Enter the **house label** (address), the **dongle
   Wi-Fi + password**, and the **Fleet API token**.
3. Submit. The box creates the house, signs its certificate, and stores the
   house key, then shows **HOUSE #N** — **write N on the 3 sensor boxes**.
4. Tap "Finish & restart".

### Each sensor (×3)
1. Connect to `ACHAS-PROV-<sensor-ID>`.
2. Role = **Sensor**. Enter the **House ID (N)** from the gateway, the
   **box number (1/2/3)**, the same **Wi-Fi + password**, and the **token**.
3. Submit. The LED blinks and the box restarts into operation.

> Sensors get the Wi-Fi credentials too — they need them for OTA later.

---

## 4. Prerequisites on the VPS (infra)

The device calls the Fleet API over **HTTPS**. Set `FLEET_API_BASE` in
`src/config.h` to the HTTPS front of the Fleet API. Currently:

```
#define FLEET_API_BASE "https://achas-iot.sustainablelivinglab.org"
```

This requires a reverse proxy (Caddy) on the VPS that:
- serves the Fleet API over HTTPS (the API itself listens on :8000), and
- (for OTA) hosts `firmware.bin` + `firmware.bin.sig`.

Until that HTTPS front exists, provisioning will fail at "certificate
signing / create house" — the firmware is ready; the endpoint must be up.

---

## 5. Security notes

- The **API token is typed by the installer** each session and used only in
  RAM — it is **never stored on the device or baked into the binary**.
- The gateway's **private key is generated on-chip** and never leaves it;
  only a CSR is sent. Certificate + house key arrive over HTTPS.
- This is the **same runtime security** as UNIFIED (AES-128-GCM, mTLS,
  anti-replay). Only the *provisioning method* differs.

---

## 6. What differs from UNIFIED (and only this)

| Added file | Purpose |
|---|---|
| `src/core/selftest.*` | Power-on hardware self-test (LED codes) |
| `src/core/portal_provision.*` | App-free Wi-Fi web-portal enrolment |
| `src/main.cpp` (wiring) | Uncommissioned boot → self-test → portal |
| `src/config.h` | `FLEET_API_BASE`, `PORTAL_TIMEOUT_MS` |
| `src/secrets.h` | default Wi-Fi blanked (portal supplies it) |

Everything else is identical to UNIFIED, including OTA. Firmware signed for
UNIFIED and for pre-production use the **same OTA key**, so OTA works across
both.
