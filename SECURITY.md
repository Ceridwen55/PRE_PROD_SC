# ACHAS / STAYCOOL — Security Model

> Reference document for the "System Overview" presentation.
> Scope: every protection layer in the system — what it is, which threat it
> stops, where it is enforced — plus an honest assessment of the current
> maturity and a hardening roadmap. Firmware referenced: **v4.4.2**.

---

## 1. Security philosophy

The system uses **defense in depth**: each hop in the data path has its own
cryptographic protection, keyed independently, so compromising one layer does
not automatically compromise the next. The most valuable secret — the OTA
firmware-signing private key — is kept **completely offline** and is the one
thing no online compromise can reach.

Two deliberate posture notes for the current phase:
- This is a **pre-production / pilot** build. Hardware anti-tamper (flash
  encryption + secure boot) is intentionally **off** — it is a one-way,
  brick-on-mistake eFuse operation reserved for the production run.
- Devices **do not pin the server TLS certificate**; trust is anchored instead
  on payload signatures (OTA) and the mutual-TLS CA chain (MQTT).

---

## 2. Defense-in-depth at a glance

| # | Hop / asset | Mechanism | Key material | Protects against |
|---|-------------|-----------|--------------|------------------|
| 1 | Sensor → Gateway (ESP-NOW data) | **AES-128-GCM** | Per-house key (16 B) | Eavesdropping + tampering of readings |
| 2 | Gateway → Sensor (commands) | **HMAC-SHA256** over `seq‖cmd` | Per-house key | Forged/replayed REBOOT/PAIR/OTA commands |
| 3 | Gateway → Cloud (MQTT) | **Mutual TLS** | Per-device X.509 cert | Impersonation + wiretap of the uplink |
| 4 | Firmware updates (OTA) | **SHA-256 + RSA-2048 signature**, fail-closed | Fleet OTA keypair (private = offline) | Malicious/corrupted firmware |
| 5 | BLE commissioning | **ECDH P-256 → HKDF → AES-128-GCM** (app-layer) | Ephemeral session key | Sniffing secrets during pairing |
| 6 | House key at rest (DB) | **AES-256-GCM** | Master KEK (server env) | DB dump revealing house keys |
| 7 | Replay across the whole path | **Monotonic sequence** (`boot‖msg`) | — | Duplicate / replayed readings |
| 8 | Device identity | **On-chip EC P-256 keygen** + CA-signed X.509 | Per-device private key (never leaves chip) | Cloned / spoofed devices |
| 9 | Fleet API | **Bearer token** over HTTPS | Shared API token | Unauthorised provisioning calls |

---

## 3. Layer-by-layer detail

### Layer 1 — ESP-NOW data encryption (AES-128-GCM)
- Every sensor reading is encrypted before it leaves the sensor. Envelope =
  `IV(12) ‖ ciphertext ‖ tag(16)`.
- GCM provides **confidentiality + integrity** in one pass: a flipped bit makes
  the tag check fail and the reading is dropped.
- Keyed by the **per-house** key, so a compromise is scoped to one house, not the
  whole fleet.
- Same key/algorithm on both ends (`crypto_encrypt` / `crypto_decrypt`).

### Layer 2 — Command authentication (HMAC-SHA256)
- Actionable commands the gateway sends back to a sensor (OTA / REBOOT / PAIR)
  carry a trailing **HMAC tag** over `sequence ‖ command`, keyed by the house key.
- The sensor recomputes and **constant-time compares** the tag; a bad tag → the
  command is ignored.
- Because the tag is bound to *that reading's sequence number*, a captured
  command **cannot be replayed** on a later check-in.
- Plain ACKs carry no tag (they trigger no action).

### Layer 3 — MQTT mutual TLS
- The gateway connects to EMQX on 8883 presenting its **per-device X.509
  certificate**; the broker verifies it against the ACHAS root CA (`verify_peer`).
- The gateway also verifies the **broker's** certificate against a baked-in CA
  (`setCACert`) — so this hop is mutually authenticated (unlike the provisioning
  calls, see Open Items).

### Layer 4 — OTA signing (SHA-256 + RSA-2048), fail-closed
- Firmware images are signed with a **fleet-wide RSA-2048 key**; the **public
  key is baked into every firmware** (`certs.h`), the **private key is offline**.
- A device downloading an update verifies the SHA-256 **and** the RSA signature
  against the embedded public key before committing.
- **Fail-closed:** updates are written to the *inactive* flash partition; any
  failure keeps the currently-running firmware — a bad or forged image never
  bricks a box and is never booted.
- This is the strongest layer: even a full server compromise **cannot push
  malicious firmware**, because the signing key isn't on the server.

### Layer 5 — BLE commissioning channel (app-layer E2E)
- The system does **not** use standard BLE pairing/bonding (no passkey, no LE
  Secure Connections at the link layer). The raw BLE GATT link is unencrypted.
- Instead an **application-layer** secure channel is built on top:
  **ECDH (P-256)** between the device's key and the phone's ephemeral key →
  **HKDF-SHA256** (`info = "ACHAS-PROV-v1"`) → a one-time **AES-128-GCM** key.
- Commissioning secrets (house key, Wi-Fi password) are encrypted under that key,
  so they **never appear in plaintext over Bluetooth**. The device **refuses**
  to accept a commissioning blob before a secure session exists.
- The session key is **ephemeral** and wiped after pairing; nothing persists.
- **Known limitation:** the ECDH is **not authenticated** (no passkey /
  numeric-compare / out-of-band verification), so it is strong against *passive*
  sniffing but theoretically vulnerable to an *active* man-in-the-middle present
  in BLE range during the short, button-initiated pairing window.

### Layer 6 — House key at rest (AES-256-GCM)
- House AES keys are **never stored in plaintext** in PostgreSQL. They are sealed
  with a server-side **master KEK** (AES-256-GCM) and only unsealed in memory
  when served over an authenticated request.
- A stolen database dump alone does not reveal any house key without the KEK.

### Layer 7 — Anti-replay (monotonic sequence)
- `seq = [16-bit boot counter][16-bit message counter]` (see ARCHITECTURE.md §9).
- Node-RED drops any reading with `seq ≤ last seen` for that device.
- Doubles as tamper-evidence and as OTA-landed proof (boot counter step).

### Layer 8 — Device identity (on-chip keygen + CA)
- Each device **generates its own EC P-256 private key on first boot** and stores
  it in NVS; it **never leaves the chip**.
- The device only ever sends a **CSR**; the backend CA signs it into an X.509
  client certificate. So a single shared firmware binary yields a **unique,
  attestable identity per device** with no shared private material.

### Layer 9 — Fleet API authentication
- All provisioning/CA endpoints require a **Bearer token**; only `/health` is
  open.
- Today this is a **single global token** shared by every installer and by
  Node-RED (see Open Items #1).

---

## 4. Trust boundaries

```
  ┌─────────────────────────── DEVICE (trusted silicon) ───────────────────────────┐
  │  on-chip private key • house key in NVS • baked OTA public key • baked broker CA │
  └───────────────┬─────────────────────────────────────────────┬──────────────────┘
                  │ ESP-NOW (AES-GCM, HMAC)                       │ MQTT (mutual TLS)
                  ▼                                               ▼
        ┌──────────────────┐                          ┌────────────────────────┐
        │  radio proximity │  ◄── attacker needs to    │      INTERNET edge     │
        │   (~10–100 m)    │      be physically near   │  (8883 data, 8443 API) │
        └──────────────────┘                          └───────────┬────────────┘
                                                                  │
                              ┌───────────────────────────────────▼───────────────────┐
                              │                SERVER (semi-trusted)                    │
                              │  API token • house keys (sealed) • master KEK • CA key  │
                              │  ── CANNOT reach: OTA signing private key (OFFLINE) ──   │
                              └─────────────────────────────────────────────────────────┘
```

- **Radio-proximity attacks** (ESP-NOW / BLE) require being physically near a
  device — not remote.
- **Network attacks** (Fleet API, MQTT) are remote but bounded by tokens + mTLS.
- **The OTA signing private key sits outside every online boundary** — it is the
  hard floor under the whole system.

---

## 5. Threat model — what an attacker can and cannot do

### If the Fleet API token leaks
**Can:** read any house's AES key; mint arbitrary device certificates (the CA
signs any CN); rotate/sabotage house keys; write junk rows; and — because there
is no rate limit — degrade the service (CPU/disk exhaustion).
**Cannot:** log into the server/OS; run code on the server; read the master KEK,
CA private key, or DB credentials (never returned by any endpoint); **or push
malicious firmware** (OTA signing key is offline).
> Blast radius today = the whole fleet (single global token). See roadmap #1.

### If a house key leaks (e.g., via the token above)
**Can:** decrypt that house's ESP-NOW traffic **if physically in radio range**,
and forge valid REBOOT/PAIR commands to its sensors.
**Cannot:** affect other houses (keys are per-house); push firmware (separate
offline key); act remotely (needs radio proximity).

### If the server is fully compromised
**Can:** disrupt provisioning, read sealed data if the KEK is also taken,
impersonate the broker to devices that don't pin it.
**Cannot:** push malicious firmware to the fleet — the OTA signing private key is
not on the server. This is the single most important containment property.

### If an attacker has physical access to a device
**Can (today):** dump the entire flash over USB and extract the house key,
device private key, and Wi-Fi credentials — because flash encryption is off.
**After production hardening:** flash encryption + secure boot seal this.

---

## 6. Current maturity assessment

An honest, self-critical read (percentages are indicative, not measured):

| Dimension | Maturity | Notes |
|-----------|:--------:|-------|
| Application / protocol crypto | ~90% | AES-GCM, HMAC, signed OTA, anti-replay — well designed. |
| Device identity & PKI | ~85% | On-chip keys, per-device certs, mutual TLS. |
| OTA integrity | ~95% | Signed, fail-closed, offline key. Strongest layer. |
| Transport (provisioning) | ~60% | `setInsecure()` on Fleet API calls (no server-cert auth). |
| Backend hardening | ~50% | Single token, no rate limit, no body cap, verbose errors. |
| Physical / hardware | ~5% | Flash encryption + secure boot deliberately **off** (pre-prod). |
| **Overall (as a shipped product)** | **~65–70%** | The valuable/hard part is done; the gap is mostly one deferred production step. |

**Headline:** the hardest, highest-value work (the cryptographic protocol design)
is solid. The remaining gap is dominated by **one deferred production step**
(eFuse hardening) plus a set of **backend-only** improvements that need **no
firmware change**.

---

## 7. Known gaps & open items

| # | Gap | Where | Firmware change? |
|---|-----|-------|:----------------:|
| 1 | **Single global Fleet API token** → one leak exposes the whole fleet | Fleet API `auth()` | No (backend) |
| 2 | Token compared with a **non-constant-time** `!=` (timing side-channel) | Fleet API `auth()` | No (backend) |
| 3 | **No rate limiting / body-size cap** → brute-force + DoS | Caddy / Fleet API | No (backend) |
| 4 | **`sign-csr` signs any CN** with no enrolment check | Fleet API CA | No (backend) |
| 5 | **DB error strings leaked** in HTTP responses | Fleet API handlers | No (backend) |
| 6 | **`firmware.bin` served publicly** and contains baked secrets (incl. default Wi-Fi creds) | Caddy `/fw/*` | No (backend) |
| 7 | **`setInsecure()`** on provisioning calls (no server-cert auth) → local-network MITM of the house key | Firmware portal | Yes |
| 8 | **BLE ECDH unauthenticated** → active-MITM in pairing window | Firmware BLE | Yes (+ app) |
| 9 | **Flash encryption / Secure Boot OFF** → physical flash dump | eFuse (production) | Production step |
| 10 | **EMQX topic ACL scope unconfirmed** → a valid cert may pub/sub any house's topics | EMQX config | No (backend) |

---

## 8. Hardening roadmap (priority order)

**Tier 0 — verify first (read-only, no change):**
- Confirm EMQX topic ACL (#10) and that admin UIs (Node-RED 1880 / EMQX 18083 /
  InfluxDB 8181/8888) are **not** exposed to the internet.

**Tier 1 — backend quick wins (no firmware change, low effort):**
- Constant-time token compare (#2).
- Rate limiting + request body-size cap (#3).
- Generic error responses; log details server-side only (#5).
- Restrict / obscure the public firmware path or add IP allow-listing (#6).

**Tier 2 — backend structural (no firmware change):**
- **Per-house installer tokens** (2-tier: a "service token" for Node-RED that
  keeps working unchanged, plus per-house installer tokens that scope blast
  radius to one house). Store token **hashes**, not plaintext (#1).
- Scope `sign-csr` to an enrolled/expected CN pattern (#4).

**Tier 3 — production hardening (the big physical step):**
- Enable **flash encryption + secure boot** on the production run (#9).
- Optionally add server-cert pinning on provisioning (#7) and an authenticated
  BLE pairing confirmation (#8) in a future firmware.

> Everything in Tier 0–2 is achievable **without touching already-shipped
> firmware** — an important operational property once units are in the field.

---

## 9. Key & secret inventory

| Secret | Type | Where it lives | Rotation |
|--------|------|----------------|----------|
| Device private key | EC P-256 | On-chip NVS (never leaves) | New device = new key |
| Per-house key | AES-128 | Device NVS + DB (sealed) | `POST /houses/{id}/rotate` |
| Master KEK | AES-256 | Server env var | Manual (re-seal all keys) |
| Root CA private key | EC/RSA | Server file (`chmod 600`) + offline backup | Rare (re-issues fleet) |
| **OTA signing private key** | **RSA-2048** | **OFFLINE only** (never on server/CI) | Rare (fleet-wide) |
| OTA signing public key | RSA-2048 | Baked into every firmware (`certs.h`) | With firmware |
| Fleet API token | Bearer string | Device (typed) + Node-RED env | On leak / per roadmap #1 |
| InfluxDB token | Bearer string | Node-RED env | Manual |
| Broker root CA | X.509 | Baked into firmware + EMQX | Rare |

---

## 10. One-slide summary (for the deck)

- **Every hop is independently encrypted/authenticated** — AES-GCM (data),
  HMAC (commands), mutual TLS (uplink), RSA signatures (firmware).
- **Per-house keying** limits any single compromise to one house.
- **Per-device on-chip identity** — no shared private material across the fleet.
- **The firmware-signing key is offline** — no online compromise can push
  malicious firmware. This is the system's hard security floor.
- **Current maturity ~65–70%**: protocol crypto is strong; the main remaining
  step is enabling hardware anti-tamper at production, plus a set of
  backend-only hardening items that need no firmware change.
