# ACHAS / STAYCOOL — Backend Setup

> Reference document for the "System Overview" presentation.
> Scope: the cloud side that receives, decrypts, stores, and serves the data,
> plus the provisioning API and Certificate Authority. All services run as
> Docker containers on a single Linux VPS, on one shared Docker network.

---

## 1. Service overview

The backend is a set of containers on one internal Docker network
(`iot-net`). Devices only ever reach two of them from the outside: the **MQTT
broker** (for data) and the **HTTPS front** (for provisioning + OTA downloads).

| Container | Image | Role | Exposed |
|-----------|-------|------|---------|
| **achas-db** | `postgres:16` | Houses, devices, issued certificates. House keys encrypted at rest. | internal only |
| **achas-api** | Go (custom) | Fleet API + Certificate Authority. | internal (via Caddy) |
| **achas-caddy** | `caddy` | HTTPS front: serves OTA firmware + reverse-proxies the Fleet API. | **8443 → 443** |
| **emqx** | `emqx-enterprise` | MQTT broker with mutual-TLS listener. | **8883** (mTLS), internal 1883 |
| **node-red** | `nodered/node-red:4.1.10` (pinned) | Decrypt + enrich + write to InfluxDB. | internal (admin UI 1880 local only) |
| **influxdb3-core** | InfluxDB 3 | Time-series store (`STAYCOOL_PREPROD_1`). | internal 8181 |
| **influxdb3-explorer** | InfluxDB 3 UI | Query/visualise UI. | local only 8888 |

> **Version pin note:** Node-RED is pinned to **4.1.10** on purpose. Node-RED 5
> changed how HTTP-request nodes handle `msg.url`/`msg.headers` overrides, which
> breaks the decrypt pipeline's dynamic per-house key lookup.

---

## 2. Network & exposure model

```
   Internet
      │
      │  8883  (MQTT + mutual TLS)                8443  (HTTPS)
      ▼                                            ▼
 ┌─────────┐                                 ┌───────────┐
 │  emqx   │                                 │   caddy   │
 └────┬────┘                                 └─────┬─────┘
      │ internal 1883                    /fw/* │    │ else
      │                       (firmware files) │    │ reverse_proxy
      ▼                                        ▼    ▼
 ┌──────────┐   API_TOKEN    ┌────────────────────────┐
 │ node-red │ ─────────────► │        achas-api       │
 └────┬─────┘  (house key,   │   (Fleet API + CA)     │
      │         room lookup) └───────────┬────────────┘
      │ line protocol                    │ SQL
      ▼                                   ▼
 ┌──────────┐                       ┌──────────┐
 │ influxdb │                       │ achas-db │
 └──────────┘                       └──────────┘
```

**Only two inbound ports face the internet: 8883 (data) and 8443 (provision +
OTA).** Everything else is reachable only inside `iot-net` or over an SSH tunnel
for admin (InfluxDB Explorer, EMQX dashboard, Node-RED editor).

---

## 3. Fleet API + Certificate Authority (`achas-api`)

A small Go service. It handles provisioning; it **never touches MQTT or
InfluxDB** — that pipeline is entirely Node-RED's job.

### 3a. Endpoints

| Method + path | Auth | Purpose |
|---------------|------|---------|
| `GET /health` | none | Liveness probe. |
| `POST /houses` | Bearer | Create a house (or reuse an existing id), generate + store its AES house key. |
| `GET /houses/{id}/key` | Bearer | Return the house's AES key (hex). Used by the device at provisioning **and** by Node-RED to decrypt. |
| `POST /houses/{id}/rotate` | Bearer | Generate a new house key (bumps key_version). |
| `POST /devices/sign-csr` | Bearer | **CA:** sign a device CSR → per-device X.509 client certificate. |
| `GET /ca/root` | Bearer | Return the root CA certificate (public). |
| `POST /devices/enroll` | Bearer | Record which device is installed where (house/role/box/room/cert). |
| `GET /devices/{id}` | Bearer | Look up a device's house/role/box/room (used by Node-RED for room enrichment). |

### 3b. Authentication (current model)

A **single global Bearer token** (`API_TOKEN`) protects every endpoint except
`/health`. It is supplied to:
- the **device** (installer types it into the provisioning form), and
- **Node-RED** (as an environment variable) for the house-key + room lookups.

> See SECURITY.md for the planned move to per-house installer tokens (a
> backend-only change; no firmware change required).

### 3c. Configuration (environment variables)

| Variable | Meaning |
|----------|---------|
| `PORT` | Listen port (default 8000). |
| `DATABASE_URL` | PostgreSQL connection string. |
| `API_TOKEN` | The bearer token for all protected endpoints. |
| `ACHAS_MASTER_KEK` | 32-byte base64 key-encryption-key. Encrypts house keys **at rest** in the DB (AES-256-GCM). Never leaves the server. |
| `ACHAS_CA_CERT` / `ACHAS_CA_KEY` | Paths to the root CA certificate + private key (the CA's "crown jewels"). |
| (built-in) `CertDays` | Issued device-cert lifetime = **730 days**. |

### 3d. Certificate Authority behaviour

- The device generates its **own** EC P-256 keypair on-chip and only sends a
  **CSR** (its public key). The server never sees a device private key.
- `sign-csr` validates the CSR signature (proves the device holds the matching
  private key), then issues a client certificate (`ExtKeyUsage = ClientAuth`,
  `IsCA = false`) signed by the root CA.
- Every issued certificate is recorded in `issued_certs` (serial + common name)
  for audit / future revocation.
- The **root CA private key** is the single most sensitive file on the server —
  it should be `chmod 600` and backed up offline.

---

## 4. Database schema (PostgreSQL)

```sql
houses (
    house_id      SERIAL PRIMARY KEY,   -- installer may also choose the number
    house_label   TEXT NOT NULL,        -- address / name
    house_key_enc BYTEA,                -- AES house key, ENCRYPTED with the KEK
    key_version   INT,                  -- bumped on rotate
    active        BOOLEAN
)

fleet_devices (
    device_id    TEXT PRIMARY KEY,      -- 4 hex chars from the MAC
    mac          TEXT,
    house_id     INT REFERENCES houses(house_id),
    role         TEXT,                  -- 'gateway' | 'sensor'
    box_id       INT,                   -- 0..3
    logical_name TEXT,                  -- room label
    cert_serial  TEXT,                  -- links to issued_certs.serial
    enrolled_at  TIMESTAMPTZ
)

issued_certs (
    serial       TEXT PRIMARY KEY,
    common_name  TEXT,                  -- CN=achas-XXXX
    issued_at    TIMESTAMPTZ
)
```

**Key-at-rest:** the AES house key is never stored in plaintext. It is sealed
with `ACHAS_MASTER_KEK` (AES-256-GCM) before being written to `house_key_enc`,
and only unsealed in memory when handed out over an authenticated TLS request.

---

## 5. MQTT broker (EMQX)

- **Listener:** mutual-TLS on **8883**. Devices present their per-device X.509
  certificate; the broker verifies it against the ACHAS root CA (`verify_peer`).
- The broker also trusts the same CA the Fleet API signs with, so any device
  enrolled through the CA can connect.
- **Internal plaintext 1883** exists on the Docker network for server-side
  clients (e.g. Node-RED subscribing, and admin publish of OTA commands).
- **Topics:**
  - `achas/house/<H>/gateway/status` — retained status (online/offline/ota_*).
  - `achas/house/<H>/gateway/command` — command channel (OTA/PING/REBOOT/PAIR).
  - Data topics under `achas/house/<H>/...`.

> **To confirm during hardening:** whether the broker's ACL restricts each
> certificate to only its own house's topics, or allows any valid cert to
> pub/sub any topic (see SECURITY.md, "Open items").

---

## 6. Node-RED decrypt pipeline

The heart of the cloud ingest. Flow (one message = one reading):

```
 MQTT in ─► fn_parse ─► http_key ─► fn_decrypt ─┬─(no room)─► http_lookup ─► fn_apply_lookup ─┐
            (extract    (GET        (AES-128-GCM │                (GET /devices/{id})           │
             envelope,   house key)  decrypt +   │                                              ▼
             ask for                 anti-replay)└─(has room)──────────────────────────► fn_finalize
             house key)                                                                    (line protocol)
                                                                                                │
                                                                                                ▼
                                                                                           http_influx
                                                                                        (write to InfluxDB)
```

- **fn_parse** — parses the MQTT envelope, extracts `house`, `device`,
  ciphertext; builds a `GET /houses/{house}/key` request with the `API_TOKEN`.
- **fn_decrypt** — AES-128-GCM decrypt using the fetched house key; runs the
  **anti-replay** check (`flow` context stores the last sequence per
  `house:device`; drops non-increasing). If the reading has no room, emits a
  `GET /devices/{device}` lookup.
- **fn_apply_lookup** — copies the room label from the device record.
- **fn_finalize** — builds an InfluxDB **line-protocol** string:
  `sensors,house=..,device=..,box=..,room=.. temperature=..,humidity=..,battery_v=..,wake_ms=..i,seq=..i`
  and writes it with the `INFLUX_TOKEN`.

Node-RED holds two secrets as environment variables: **`API_TOKEN`** (Fleet API)
and **`INFLUX_TOKEN`** (InfluxDB write).

---

## 7. InfluxDB

- **Database:** `STAYCOOL_PREPROD_1`, measurement/table `sensors`.
- **Tags:** `house`, `device`, `box`, `room`.
- **Fields:** `temperature`, `humidity`, `battery_v`, `wake_ms`, `seq`.
- **Auth:** bearer token required for read/write (held by Node-RED for writes;
  admin queries run inside the container using its own token so the value never
  leaves the server).
- **Explorer UI** (port 8888) for ad-hoc queries — reached over an SSH tunnel,
  not exposed publicly.

---

## 8. HTTPS front (Caddy) + OTA hosting

One HTTPS site does two jobs:

| Path | Behaviour |
|------|-----------|
| `/fw/*` | Static file server for OTA: `firmware.bin` + `firmware.bin.sig`. |
| everything else | Reverse-proxy to the Fleet API (`achas-api:8000`). |

- Uses a **self-signed certificate** (`tls internal`). This is fine because
  devices **do not pin the server certificate** — they verify OTA payloads by
  RSA signature and connect to MQTT by the CA-trusted chain instead.
- OTA firmware files live in a host directory bind-mounted into the container
  (e.g. `.../caddy/firmware → /srv/fw`).

---

## 9. Bring-up sequence (how to stand the backend up)

1. **Provision the VPS** — Docker + Docker Compose; create the external
   `iot-net` network.
2. **Generate the PKI** — root CA cert + key (kept `chmod 600`); the OTA signing
   keypair is generated **offline** and only the public key ships in firmware.
3. **Start PostgreSQL** and load the schema (`houses`, `fleet_devices`,
   `issued_certs`).
4. **Configure + start the Fleet API** with its env vars (`DATABASE_URL`,
   `API_TOKEN`, `ACHAS_MASTER_KEK`, CA paths). Verify `GET /health`.
5. **Install EMQX certificates** (CA + server cert/key) and enable the mutual-TLS
   8883 listener (`verify_peer`).
6. **Start Caddy** on 8443; drop `firmware.bin` + `.sig` into the OTA directory;
   verify both are served.
7. **Start Node-RED** (pinned 4.1.10) with `API_TOKEN` + `INFLUX_TOKEN`; import
   the decrypt flow; confirm it connects to the broker.
8. **Start InfluxDB** + create the `STAYCOOL_PREPROD_1` database and a token.
9. **End-to-end smoke test** — provision one device, confirm a reading lands in
   InfluxDB with the correct room and wake-time.
10. **Open only 8443 + 8883** to the internet (firewall). Keep admin UIs
    (1880 / 8181 / 8888 / EMQX dashboard) bound to localhost or behind SSH.

---

## 10. Common operational tasks

| Task | How |
|------|-----|
| **Trigger a gateway OTA** | Publish to `achas/house/<H>/gateway/command` with `{"command":"OTA","target_box":0,"url":"https://<host>:8443/fw/firmware.bin","sha256":"<hash>"}` (internal 1883 or an authorised mTLS client). |
| **Trigger a sensor OTA** | Same, with `"target_box":1..3` and `"target_dev":"<4-hex>"`. The gateway relays it on the sensor's next check-in. |
| **Watch device status** | Subscribe to `achas/house/<H>/gateway/status` (online / offline / ota_started / relay_*). |
| **Verify an OTA landed** | Confirm the device's `seq` boot counter (top 16 bits) incremented in InfluxDB. |
| **Sign a new firmware** | `openssl dgst -sha256 -sign <ota_private.pem> firmware.bin \| base64 -w0 > firmware.bin.sig` (private key stays offline). |
| **Publish new firmware** | Copy `firmware.bin` + `.sig` into the Caddy OTA directory. |

---

## 11. Ports reference

| Port | Service | Exposure |
|------|---------|----------|
| 8883 | EMQX MQTT (mutual TLS) | Internet |
| 8443 | Caddy HTTPS (Fleet API + OTA) | Internet |
| 1883 | EMQX MQTT (plaintext) | Internal only |
| 8000 | Fleet API | Internal (via Caddy) |
| 1880 | Node-RED editor | Local / SSH tunnel |
| 8181 | InfluxDB | Internal |
| 8888 | InfluxDB Explorer | Local / SSH tunnel |
| 18083 | EMQX dashboard | Local / SSH tunnel |

See **SECURITY.md** for the protection each layer provides and the hardening
roadmap.
