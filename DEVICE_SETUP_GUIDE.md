# ACHAS Pre-Production — Device Setup Guide (Flash → Provision → Data in InfluxDB)

This guide takes a **blank ESP32-C3 box** all the way to **live data in InfluxDB**.
It is written to be followed by three different people:

1. **Manufacturer** — flashes the firmware onto the boards.
2. **Installer** — powers on each box and fills in a small web form.
3. **Backend operator** — confirms the data arrived in InfluxDB.

There is **ONE firmware** for every box. A box becomes a *gateway* or a *sensor*
at provisioning time, not at flashing time. So the manufacturer flashes the
exact same file to all 200 boxes.

Firmware version in this build: **4.1.0**
Merged image SHA-256: **`6033a1ba7ee1051491681dcf7d13981863d2f1d0bce0a9bde6fe2e38d40cfd2e`**

> **Flash encryption / Secure Boot are intentionally OFF in this build.**
> This is deliberate for pre-production so any box can be re-flashed or fully
> erased over USB if something goes wrong. Do **not** enable the eFuse hardening
> until the real production run. (See "Re-flashing & erasing" at the end.)

---

## The system in one picture

```
  [ SENSOR box ]  --ESP-NOW (2.4 GHz)-->  [ GATEWAY box ]
   temp/humidity                              |
   battery, seq#                              |  Wi-Fi (home dongle)
                                              v
                                    [ MQTT broker EMQX ]   mutual-TLS, port 8883
                                              |
                                              v
                                      [ Node-RED ]  decrypts (per-house AES key)
                                              |            + anti-replay check
                                              v
                                      [ InfluxDB 3 ]  <-- data lands here
```

- Sensors never touch the internet. They talk only to their gateway over ESP-NOW.
- The gateway is the only box on Wi-Fi. It forwards readings to the cloud.
- Every reading is **AES-128-GCM encrypted with a per-house key** end to end;
  only Node-RED (which fetches the house key from the Fleet API) can read it.

---

# PART A — Manufacturer: flash the firmware

## What you receive

Inside the `FACTORY_FLASH/` folder:

| File | Purpose |
|------|---------|
| `ACHAS_PREPROD_v4.1.0_merged.bin` | **The one file to flash.** Contains bootloader + partitions + app, already at the right offsets. Flash it to address `0x0`. |
| `ACHAS_PREPROD_v4.1.0_merged.bin.sha256` | Checksum to verify the file above wasn't corrupted. |
| `components/` | The same firmware split into 4 separate files, in case you prefer the classic multi-offset method. |

## Option 1 — Flash the single merged file (recommended)

Board: **ESP32-C3**. Put the board into download mode if needed (hold **BOOT**,
tap **RESET**, release **BOOT**). Then:

```bash
esptool.py --chip esp32c3 --port COM5 --baud 921600 write_flash 0x0 ACHAS_PREPROD_v4.1.0_merged.bin
```

Replace `COM5` with the board's port (Windows: Device Manager → Ports;
Linux/Mac: `/dev/ttyUSB0` or `/dev/ttyACM0`).

That is the entire flashing step. One file, one address (`0x0`).

## Option 2 — Flash the 4 component files (classic method)

If you'd rather not use the merged file, flash the parts from `components/`:

```bash
esptool.py --chip esp32c3 --port COM5 --baud 921600 write_flash \
  0x0     components/bootloader.bin \
  0x8000  components/partitions.bin \
  0xe000  components/boot_app0.bin \
  0x10000 components/firmware.bin
```

## Verify the flash (optional but recommended for the first few units)

Open a serial monitor at **115200 baud**. On boot you should see:

```
   ACHAS UNIFIED BOX -- booting...
   Firmware   : 4.1.0
   Broker     : achas-iot.sustainablelivinglab.org:8883 (mTLS)
```

If you see that banner, the flash worked. A freshly flashed box is
"uncommissioned" and will run the self-test, then open its Wi-Fi setup hotspot
(see Part B).

---

# PART B — First power-on: the self-test (LED check)

A brand-new box runs a **Power-On Self-Test** before anything else. This is how
the manufacturer/installer confirms the hardware is healthy, using only the two
LEDs (no computer needed).

The **STATUS LED** blinks a count after each test **passes**:

| Test | Meaning | STATUS LED on pass |
|------|---------|--------------------|
| 1 | SHT41 temp/humidity sensor (I2C) | blinks **1×** |
| 2 | Wi-Fi radio (sees at least 1 network) | blinks **2×** |
| 3 | BLE radio (sees at least 1 device) | blinks **3×** |
| 4 | Battery voltage (sensor boxes only) | blinks **4×** (skipped on gateways / no battery) |

**All tests passed:** both LEDs turn **on together for 1 second**, then the box
continues to the setup hotspot.

**A test FAILED:** the box **stops and locks** here:
- **POWER LED** blinks fast, continuously.
- **STATUS LED** blinks the **failed test number**, pauses, repeats.
  (1 blink = SHT41, 2 = Wi-Fi, 3 = BLE.)

A failed box will **not** open the setup hotspot — that's intentional QA. Set it
aside and note which test failed.

> LEDs are on **GPIO6 (Power)** and **GPIO7 (Status)**, active-HIGH. If a whole
> batch shows nothing, check those two lines / the `LED_ACTIVE_LEVEL` wiring
> before assuming the boards are dead.

---

# PART C — Installer: provision the GATEWAY (do this first per house)

Each house has **one gateway (box 0)** and up to **three sensors (box 1–3)**.
**Always set up the gateway first** — it creates the House ID that the sensors
need.

### What you need before you start
- The **Wi-Fi name + password** of the dongle/router at that house.
- The **Fleet API token** (ask the backend operator; it is the same token for
  the whole batch — see the note at the end of Part C).

### Steps
1. **Power on the box.** After the self-test it becomes a Wi-Fi hotspot named
   **`ACHAS-PROV-XXXX`**, where `XXXX` is the box's ID (also printed on the box
   label — this is how you know which box you joined).
2. On your phone, **connect to that hotspot**. A setup form pops up automatically
   (captive portal). If it doesn't, open a browser to **http://192.168.4.1/**.
3. Tap **Identify (blink LED)** to confirm you're on the right physical box — its
   STATUS LED blinks 4 times.
4. Set **Role = Gateway (box 0)**.
5. Fill in:
   - **House ID** — the number you want for this house, e.g. `1`. **You choose
     it** (1–65535). Use a simple plan, e.g. house 1, 2, 3… down the street.
   - **House label** — the address or name, e.g. `Jl. Melati No.3`.
   - **Wi-Fi name** and **Wi-Fi password** — the dongle at that house (WPA2).
   - **Fleet API token**.
6. Tap **Provision this box**. The box now:
   - drops the hotspot, connects to the Wi-Fi,
   - generates its own private key on-chip and gets a signed certificate,
   - **registers the house under the ID you chose** and fetches its key,
   - stores everything and comes back as a hotspot confirming the **House ID**.
7. **Write that House ID on the box and on this house's sensor boxes.** You need
   it for every sensor in this house.
8. Tap **Finish & restart gateway.** The gateway reboots and starts running.

> ✅ **You now choose the House ID yourself**, and re-provisioning a gateway with
> the **same** House ID is safe — the server reuses that house instead of making
> a duplicate. (Give two different houses two different IDs; giving the same ID
> to two different houses would mix their data.)
>
> ⚠️ *Requires the updated backend.* If the backend has NOT been updated to
> accept a chosen House ID yet, the gateway will fall back to a server-assigned
> number. Confirm with the backend operator before mass provisioning.

### If provisioning fails
The form comes back with a red error (wrong Wi-Fi password, wrong token, no
internet on the dongle, etc.). Fix the field and submit again — nothing is
saved until it succeeds.

### Fixing a box provisioned with wrong info (re-provision, no computer)
If a box already accepted wrong details (wrong House ID, wrong box number, wrong
Wi-Fi), reset it with the **on-board button** — no USB needed.

**Powered / always-on box (gateway):**
- Hold the button and keep holding.
  - At ~3 s the STATUS LED blinks **1×** (pairing zone — keep holding).
  - At ~10 s the STATUS LED blinks **3×**. **Release now.**
- The box wipes its provisioning, restarts, runs the self-test, and re-opens the
  `ACHAS-PROV-XXXX` setup hotspot. Its certificate/identity is kept.

**Sleeping box (sensor) — important:**
A sensor spends most of its time in deep sleep, and on the ESP32-C3 this button
**cannot wake it instantly** (hardware limit — the button is not on a wake-capable
pin). The sensor only samples the button when it boots/wakes. So use the fast
method:

- **Fast (recommended): hold + power-cycle.** Press and HOLD the config button,
  and while holding, **reset or power-cycle the box** (tap RESET/EN, or flip the
  power switch / re-seat the battery). It boots immediately with the button held,
  blinks **3×** at ~10 s → release → wipes and re-opens the setup hotspot.
- **Slow (no reset access): hold and wait.** Press and HOLD continuously; at the
  sensor's next scheduled wake (**up to one full send cycle — ~15 min**) it sees
  the button and blinks 3×. Keep holding until then. The POWER LED pulses at each
  wake — watch for the 3× status blink after a pulse, then release.

For a gateway you can also re-provision with the **same House ID** safely (the
server reuses that house). For a sensor, just re-enter the correct House ID/box.

> **Where does the Fleet API token come from?** It is a single secret string
> generated once on the backend server (`openssl rand -hex 32`, stored in the
> backend `.env` as `API_TOKEN`). The backend operator gives it to installers.
> It is typed per session and used only in RAM — it is **never** stored on the
> device or baked into the firmware.

---

# PART D — Installer: provision the SENSORS

Do this **after** the house's gateway is done, because you need the House ID.

1. Power on the sensor box → connect to its **`ACHAS-PROV-XXXX`** hotspot.
2. Set **Role = Sensor**.
3. Fill in:
   - **House ID** — the number from the gateway screen (step C-7).
   - **Box number** — 1, 2, or 3 (each sensor in a house must be unique).
   - **Wi-Fi name / password** — the same dongle (used once, only to enrol).
   - **Fleet API token** — same token as the gateway.
4. Tap **Provision this box**. On success the STATUS LED blinks 6× quickly and
   the box **restarts by itself** into sensor mode. There is no House ID screen
   for sensors — the fast blink + auto-restart is the "done" signal.

Repeat for each sensor (box 1, 2, 3) in that house.

---

# PART E — Normal operation (what to expect)

**Sensor box (battery powered):**
- Wakes about **every ~15 minutes** (production reporting cycle), reads
  temp/humidity + battery, sends the reading to the gateway over ESP-NOW, then
  deep-sleeps. A small random jitter (a few seconds) is added each cycle so
  multiple sensors in one house don't transmit at the exact same instant.
- **POWER LED** blinks once each wake ("alive").
- **STATUS LED** blinks **2×** when the gateway confirms it received the reading.
- So after provisioning, expect the **first sensor reading within ~15 minutes** —
  it is normal not to see data instantly.

**Gateway box (mains/USB powered, always on):**
- Stays connected to Wi-Fi and the MQTT broker.
- **STATUS LED** blinks **3×** each time it publishes a reading to the cloud.
- Also reads its **own** temp/humidity every 2 minutes.

If a sensor is out of range or the gateway is briefly down, the sensor stores the
reading and retries on the next wake, so a short outage doesn't lose data.

---

# PART F — Confirm the data reached InfluxDB

Once a gateway and at least one sensor are provisioned and running, data should
flow within a couple of minutes. Verify along the path, from the broker inward.
(Exact hosts/tokens are in the backend project's `BACKEND_SETUP.md`.)

### 1. Broker sees the gateway (EMQX)
- Open the **EMQX dashboard** (`:18083`) → **Clients**.
- You should see a client id like **`achas-gw-XXXX-0007`** (`XXXX` = box id,
  `0007` = House ID) with status **connected**.
- On the gateway's `achas/house/<HouseID>/gateway/status` topic you'll see a
  retained `{"status":"online"}`.

### 2. Node-RED decrypts and forwards
- In **Node-RED** (`:1880`, via SSH tunnel/VPN), open the debug sidebar.
- Each reading appears as it is decrypted. If you see
  `decrypt failed (wrong key or tampered)`, that box's house key doesn't match
  the database — re-provision that box.
- `replay: ...` warnings right after a box reboots are normal.

### 3. Data lands in InfluxDB 3
- Data is written to InfluxDB 3 Core (`:8181`), database
  **`ACHAS_SECURITY_NEW_TEST1`**, using the `INFLUX_TOKEN` bearer token.
- Query the most recent points, e.g. with the Influx CLI / HTTP API, filtered to
  the house you just set up. Fresh temperature/humidity rows with the current
  timestamp = **success, the full pipeline works.**

### Quick end-to-end sanity check without hardware
The backend repo ships `tools/simulate_gateway.py`. Running it with the API
token pushes a fake reading through the exact same path — useful to confirm the
cloud side is healthy before blaming a box.

---

# PART G — OTA firmware update (pushing a new version later)

OTA is **already built in and verified in this firmware**. You do not need it to
ship, but here is how it works so updates are painless later.

**How the device stays safe:** before it installs anything, the box requires
**all** of these to pass, or it keeps the old firmware:
1. URL must be `https://`.
2. Downloaded size ≤ 2 MB.
3. **SHA-256** of the download must match the value you sent.
4. A **detached RSA-2048 signature** (`<url>.sig`) must verify against the
   signing public key baked into the firmware.

If any check fails, nothing is flashed — the box just reports the error and keeps
running the current version. (The matching **private** signing key stays offline
on your build machine and is never on the device.)

### To publish an update
1. Build the new firmware and get its **`firmware.bin`**.
2. Compute its SHA-256:
   ```bash
   sha256sum firmware.bin        # or: shasum -a 256 firmware.bin
   ```
3. Sign it with your OTA private key and base64 the signature into a `.sig` file:
   ```bash
   openssl dgst -sha256 -sign ota_private.pem -out fw.sig firmware.bin
   openssl base64 -A -in fw.sig -out firmware.bin.sig
   ```
4. Host **both** files over HTTPS, side by side:
   `https://.../firmware.bin` and `https://.../firmware.bin.sig`.
5. Send the MQTT command to the gateway's command topic
   `achas/house/<HouseID>/gateway/command`:
   - **Update the gateway itself** (`target_box: 0`):
     ```json
     { "command": "OTA", "target_box": 0,
       "url": "https://.../firmware.bin",
       "sha256": "<the-64-hex-digest>" }
     ```
   - **Update a sensor** (`target_box: 1..3`): add its device id. The gateway
     relays the command over ESP-NOW; the sensor downloads and verifies it on
     its next wake:
     ```json
     { "command": "OTA", "target_box": 1, "target_dev": "AB12",
       "url": "https://.../firmware.bin",
       "sha256": "<the-64-hex-digest>" }
     ```
6. Watch the gateway status topic for `ota_started`, then a reboot into the new
   version (the boot banner prints the new version string).

> Keep future builds under the ~1.9 MB app-slot size. This 4.1.0 build already
> uses ~90% of the slot, so large additions may need a partition change.

---

# Appendix 1 — LED quick reference

| When | POWER LED | STATUS LED |
|------|-----------|------------|
| Boot / sensor wake | 1 short blink | — |
| Self-test, each test passed | on (steady during test) | 1/2/3/4 blinks per test |
| Self-test, all passed | on 1 s with status | on 1 s |
| Self-test FAILED | fast continuous blink | N blinks = failed test #, repeating |
| Button held ~3 s (pairing zone) | — | 1 blink → release for BLE pairing |
| Button held ~10 s (reset zone) | — | 3 blinks → release to wipe & re-provision |
| Identify button (setup form) | — | 4 blinks |
| Sensor provisioning success | — | 6 fast blinks, then reboot |
| Sensor delivered a reading | — | 2 blinks |
| Gateway published to cloud | — | 3 blinks |
| Pairing/commissioning success | — | 5 blinks |

---

# Appendix 2 — Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| No `ACHAS-PROV-XXXX` hotspot after power-on | Self-test failed (watch the STATUS LED blink code) or box is already provisioned. Hold **BOOT/pairing button 3 s at power-on** to force BLE pairing, or erase (below) to start over. |
| Setup form won't submit — "Wi-Fi connect failed" | Wrong Wi-Fi name/password, or the network isn't WPA2. |
| Setup form — "Certificate signing failed / Create house failed (token?)" | Wrong Fleet API token, or the dongle has no internet. |
| Gateway never shows as connected in EMQX | Wrong broker host/CA, no internet, or the gateway lost its certificate (it will auto-return to provisioning to re-issue). |
| Node-RED `decrypt failed` | The box's house key ≠ the database's. Re-provision that box. |
| Data in Node-RED but not in InfluxDB | Check `INFLUX_TOKEN`, the `:8181` URL, and the database name. |
| Sensor sends but gateway never confirms (no 2-blink) | Gateway off / out of ESP-NOW range / different Wi-Fi channel congestion. Sensor will retry next wake. |

---

# Appendix 3 — Re-flashing & erasing

There are three levels of "reset", from lightest to heaviest:

**1. Re-provision only (no computer) — for wrong House ID / box / Wi-Fi.**
Hold the on-board button (GPIO10) at boot; release when the STATUS LED blinks
**3×** (~10 s). The box wipes its provisioning, keeps its certificate, and
re-opens the setup hotspot. This is the normal field fix (see Part C).

**2. Full wipe over USB (forget everything, incl. certificate/identity):**
```bash
esptool.py --chip esp32c3 --port COM5 erase_flash
```
then flash again (Part A). Use this only if the device identity itself is broken.

**3. Re-flash firmware (keeps provisioning in NVS):** just repeat Part A without
erasing.

Because flash encryption is **off**, all three are always possible.

> When you move to real production, that's when you enable flash encryption +
> Secure Boot (one-way eFuse operations — a mistake bricks the unit). Until then,
> leave them off so recovery stays this easy.

> When you move to real production, that's when you enable flash encryption +
> Secure Boot (one-way eFuse operations — a mistake bricks the unit). Until then,
> leave them off so recovery stays this easy.
