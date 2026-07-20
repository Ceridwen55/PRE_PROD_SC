# ACHAS / STAYCOOL — System Architecture

> Reference document for the "System Overview" presentation.
> Scope: end-to-end IoT platform for environmental monitoring (temperature,
> humidity, battery) across many houses, from the sensor silicon to the
> dashboard. Firmware version referenced: **v4.4.2**.

---

## 1. One-paragraph summary

ACHAS is a battery-powered environmental-monitoring system. Each house has a set
of small ESP32-C3 boxes. **One box acts as a Gateway** (always powered, on
Wi-Fi) and **up to three boxes act as Sensors** (battery, deep-sleep). Sensors
measure temperature/humidity and send readings over **ESP-NOW** (a lightweight
peer-to-peer radio protocol) to their Gateway. The Gateway forwards every
reading to the cloud over **MQTT with mutual TLS**. A **Node-RED** pipeline
decrypts each reading, enriches it, and writes it to **InfluxDB**, which feeds
dashboards. A small **Go "Fleet API" + Certificate Authority** handles
provisioning: it issues each device a unique identity certificate and hands out
per-house encryption keys.

**Key design principle:** a single firmware binary runs on every box. Whether a
box becomes a Gateway or a Sensor is decided at **provisioning time** (written to
non-volatile storage), not at flashing time — *"flash one `.bin` to all."*

---

## 2. Hardware

| Item | Detail |
|------|--------|
| MCU | **ESP32-C3** (single-core 32-bit RISC-V, Wi-Fi 2.4 GHz + Bluetooth LE) |
| Sensor | **SHT41** temperature + humidity (I²C bus) |
| Power sense | Battery voltage via ADC |
| UI | Status LED (blink codes) + one BOOT button (GPIO10) |
| Power | Gateway = mains/USB (always on); Sensor = battery (deep-sleep) |
| Radio link between boxes | **ESP-NOW** (connectionless 2.4 GHz, no Wi-Fi AP needed) |
| Uplink to cloud | Gateway only: **Wi-Fi → MQTT/TLS** |

---

## 3. Roles — one binary, two personalities

Selected at runtime from the commissioning profile stored in NVS (flash
key-value store). Decided in `main.cpp :: setup()`:

| Role | box_id | Power | Behaviour |
|------|--------|-------|-----------|
| **Gateway** | 0 | Always on | Receives ESP-NOW from sensors, reads its own SHT41, uploads everything to MQTT. Never sleeps. |
| **Sensor** | 1–3 | Battery | Wakes ~0.3–0.7 s, measures, transmits over ESP-NOW, sleeps ~15 min. Never touches Wi-Fi/MQTT in steady state. |

The gateway also relays over-the-air update commands to its sensors (see §7).

---

## 4. System-level data flow

```
  ┌──────────────┐   ESP-NOW (AES-128-GCM)   ┌──────────────┐
  │  SENSOR BOX  │ ────────────────────────► │  GATEWAY BOX │
  │  (box 1..3)  │   encrypted reading        │   (box 0)    │
  │  SHT41 +     │ ◄──────────────────────── │              │
  │  battery     │   ACK / signed command     │              │
  └──────────────┘   (HMAC-SHA256)            └──────┬───────┘
                                                     │ MQTT over mutual TLS
                                                     │ (port 8883)
                                                     ▼
                                              ┌──────────────┐
                                              │  EMQX broker │
                                              └──────┬───────┘
                                                     │ subscribe
                                                     ▼
                     Fleet API (house key)   ┌──────────────┐
                    ◄─────────────────────── │   Node-RED   │  decrypt +
                     ──────────────────────► │   pipeline   │  anti-replay +
                       house_key_hex          └──────┬───────┘  room lookup
                                                     │ line protocol (HTTP)
                                                     ▼
                                              ┌──────────────┐
                                              │   InfluxDB   │ ──► dashboards
                                              └──────────────┘
```

### Step-by-step (a single sensor reading)

1. **Measure** — Sensor wakes on its deep-sleep timer, reads SHT41 + battery.
2. **Build payload** — `payload_build()` packs a compact 19-byte binary struct
   (`AchasPayload`): version, house_id, device_id, box_id, sequence number,
   previous wake duration, temperature ×100, humidity ×100, battery mV, flags.
3. **Encrypt** — `crypto_encrypt()` wraps the struct in **AES-128-GCM** using the
   per-house key → envelope = `IV(12) ‖ ciphertext ‖ tag(16)`.
4. **Transmit** — `espnow_send_payload()` broadcasts the envelope over ESP-NOW,
   sweeping channels until the Gateway answers, then listens briefly for a reply.
5. **Gateway receives** — RX task validates size, **decrypts** with the same
   house key, checks the sequence number, prints/queues the reading.
6. **Gateway replies** — a plain ACK, *or* a queued command (OTA/PING/REBOOT/PAIR)
   signed with **HMAC-SHA256** over `sequence ‖ command`.
7. **Publish** — the publisher task drains the queue and publishes a compact JSON
   document to MQTT topic `achas/house/<H>/...` over **mutual TLS**.
8. **Broker → Node-RED** — EMQX delivers the message to the Node-RED subscriber.
9. **Decrypt in cloud** — Node-RED fetches that house's AES key from the Fleet
   API, decrypts the envelope, and runs an **anti-replay** check (drops any
   sequence ≤ the last seen for that device).
10. **Enrich** — if the reading has no room label (sensors don't carry one),
    Node-RED looks it up via `GET /devices/{id}` on the Fleet API.
11. **Store** — Node-RED formats an InfluxDB **line-protocol** record and writes
    it to the `sensors` table (measurement) with tags `house/device/box/room`
    and fields `temperature/humidity/battery_v/wake_ms/seq`.
12. **Visualise** — dashboards / InfluxDB Explorer read from InfluxDB.

---

## 5. Firmware module map (`src/`)

```
src/
├── main.cpp            Entry point. setup() decides role; run_gateway_mode()
│                       or run_sensor_mode(); loop() only idles the gateway.
├── config.h            All constants (timings, pins, versions, timeouts).
├── secrets.h           Build-time secrets baked into the binary.
├── certs.h             Broker root CA + OTA signing PUBLIC key.
│
├── core/               ── LOGIC ──
│   ├── identity        Device ID derived from the chip's MAC.
│   ├── commission      Read/write the provisioning profile in NVS
│   │                   (role, house, box, wifi, house_key, room, cert).
│   ├── crypto          AES-128-GCM (data) + HMAC-SHA256 (command auth).
│   ├── device_cert     On-chip EC P-256 keypair + X.509 cert + ECDH.
│   ├── payload         Binary reading struct <-> JSON; validation.
│   ├── tasks           Gateway's 3 FreeRTOS tasks (RX / self-read / publish).
│   ├── mqtt_handler    MQTT connection, mTLS, subscribe commands, publish.
│   ├── ota_manager     Gateway self-OTA + queue/relay OTA to sensors.
│   ├── ota_remote      Sensor side: download + verify + flash new firmware.
│   ├── portal_provision  App-free Wi-Fi captive portal for setup.
│   ├── prov_session    ECDH + HKDF encrypted channel for BLE commissioning.
│   ├── seq_counter     Persistent anti-replay base per boot.
│   ├── storage         Retry buffer for failed sends (NVS).
│   ├── selftest        Power-on hardware self-test (LED codes).
│   └── wifi_manager / wifi_policy   Wi-Fi connect + WPA2 policy.
│
└── drivers/            ── HARDWARE ──
    ├── EspNow          ESP-NOW transport (send, receive, command queue).
    ├── SHT41           Temperature/humidity driver (I²C).
    ├── Battery         Battery voltage (ADC).
    └── Led             Status LED blink codes.
```

**Pattern:** `core/` = logic, `drivers/` = hardware. Every module is a `.h`
contract + `.cpp` implementation.

---

## 6. Gateway concurrency model

The gateway runs three **FreeRTOS tasks** that communicate through one shared
queue (classic producer/consumer):

```
 task_espnow_receiver ─┐
   (RX + decrypt +      ├─► g_data_queue ─► task_publisher ─► MQTT
    reply/relay)        │                     (drain + upload)
 task_sensor_reader ────┘
   (own SHT41 every 15 min)
```

- A **watchdog** reboots the gateway if any task stalls for 60 s.
- `loop()` on the gateway only feeds the watchdog; the real work is in the tasks.
- A **sensor never reaches `loop()`** — `run_sensor_mode()` always ends in deep
  sleep.

---

## 7. Over-the-air (OTA) update architecture

Two flavours, both fail-closed (any verification failure keeps the old firmware
— writes go to an inactive flash partition, so a bad update never bricks a box):

| | Gateway self-OTA | Sensor OTA |
|--|------------------|-----------|
| Trigger | MQTT command `target_box:0` | MQTT command `target_box:1..3` |
| Transport | Gateway downloads over HTTPS directly | Gateway **relays** the command to the sensor over ESP-NOW on its next check-in |
| Auth of command | mTLS to broker | **HMAC-SHA256** signed with the house key |
| Download | Gateway Wi-Fi | Sensor briefly joins Wi-Fi to download |
| Verify | SHA-256 + **RSA-2048 signature** | same |
| Result | reboot into new image | reboot into new image |

**v4.4.x reliability fix ("land-once"):** the gateway re-delivers a sensor's OTA
command on each check-in until it observes the sensor's boot counter increment
(proof it rebooted into the new image), then clears the command — so a single
lost radio frame no longer silently kills the update, and a landed update is
never re-applied.

**Signing model:** one RSA-2048 keypair for the whole fleet. The **private key
stays offline** (never on the server/CI). The **public key is baked into every
firmware** (`certs.h`). Each build produces `firmware.bin`, its SHA-256, and a
detached base64 signature (`firmware.bin.sig`). One signature works for every
device.

---

## 8. Provisioning (commissioning) — two paths

A brand-new box has `house_id == 0` and enters provisioning. Both paths write the
same NVS profile and then reboot into the chosen role.

### 8a. Web captive portal (primary)
- Box hosts a Wi-Fi access point `STAYCOOL-PROV-<id>` and a small web form
  (served entirely by the ESP32 — no internet needed on the box side).
- Installer picks role, house, box number, room, Wi-Fi, and a **Fleet API token**.
- On submit, the box joins the dongle Wi-Fi and calls the Fleet API to: sign its
  certificate (CSR), create/reuse the house, fetch the house key, and enrol.

### 8b. BLE commissioning (secondary / phone app)
- Triggered by holding the BOOT button 3 s at wake.
- Uses an **application-layer encrypted channel** (ECDH P-256 → HKDF-SHA256 →
  AES-128-GCM) so secrets never appear in plaintext over Bluetooth (see
  SECURITY.md §BLE).

### Button hold zones (at boot/wake)
| Hold | Action |
|------|--------|
| < 3 s | Normal boot |
| 3–10 s | BLE pairing mode |
| ≥ 10 s | Wipe commissioning → reboot into web portal (device key/cert kept) |

---

## 9. Anti-replay & sequence design

Every reading carries a 32-bit **sequence number**:

```
 seq = [ 16-bit boot counter ][ 16-bit message counter ]
        └ increments on reboot  └ increments per reading
          (persisted in NVS)      (resets to 1 each boot)
```

- The message counter guarantees monotonic increase within a boot.
- The boot counter guarantees the sequence still moves forward after a reboot /
  power loss (so the backend never mistakes a fresh boot for a replay).
- **Node-RED drops any reading whose sequence ≤ the last one seen** for that
  device → replayed/duplicate packets are discarded.
- Bonus: because a successful OTA reboots a sensor, the boot counter incrementing
  is used as **proof an OTA landed** (used by the land-once fix and for remote
  verification without a serial cable).

---

## 10. Reporting cadence

| | Behaviour |
|--|-----------|
| Sensor | Deep-sleep ~895 s **+ 0–30 s random jitter** → ~15 min cycle. Jitter (hardware RNG) de-correlates multiple sensors in one house so their ESP-NOW transmissions don't collide. |
| Gateway | Never sleeps; reads its own SHT41 every 15 min and forwards each sensor reading the moment it arrives. |

---

## 11. Technology stack

| Layer | Technology |
|-------|-----------|
| MCU / firmware | ESP32-C3, PlatformIO, Arduino-ESP32 core, FreeRTOS |
| Inter-box radio | ESP-NOW |
| Crypto (device) | mbedTLS (AES-GCM, HMAC, ECDH, HKDF, RSA verify, X.509) |
| Cloud transport | MQTT over mutual TLS (EMQX Enterprise) |
| Provisioning API + CA | Go (Fleet API + Certificate Authority) |
| Relational store | PostgreSQL (houses, devices, issued certs) |
| Stream processing | Node-RED |
| Time-series store | InfluxDB 3 (+ Explorer UI) |
| Reverse proxy / OTA host | Caddy (HTTPS) |
| Deployment | Docker Compose on a Linux VPS |

See **BACKENDSETUP.md** for the server side and **SECURITY.md** for the
protection layers.
