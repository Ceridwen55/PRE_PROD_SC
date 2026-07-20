# STAYCOOL / ACHAS — Factory Flash Package (v4.3.0)

Newest pre-production build. One firmware for every box — a unit becomes a
gateway or a sensor at **provisioning time** (in the web portal), not at
flashing time, so flash the same file to all 50 units.

> Same firmware family as the earlier `factory_15minutes` (v4.1.1), now at
> **v4.3.0**: 15-minute reporting, reduced sensor wake time, HMAC-authenticated
> ESP-NOW commands, wake-time + room fields in telemetry, and the black & white
> STAYCOOL provisioning portal. Full list in `CHANGELOG` of the source repo.

## Checksums

| File | SHA-256 |
|------|---------|
| `STAYCOOL_PREPROD_v4.3.0_merged.bin` | `78e967d330fd2f4c0478880a325a44e54eec82b47ced9067cbe8c9fa107ae887` |
| `STAYCOOL_PREPROD_v4.3.0_app.bin`    | `ad04f7327f30f061e7dc3aa078562d1cdfe227302177a895d66be37b21d9693d` |

## What's in here

| File | Purpose |
|------|---------|
| `STAYCOOL_PREPROD_v4.3.0_merged.bin` | **The one file to flash over USB.** Bootloader + partitions + app, flash to `0x0`. |
| `STAYCOOL_PREPROD_v4.3.0_merged.bin.sha256` | Checksum for the merged image. |
| `STAYCOOL_PREPROD_v4.3.0_app.bin` | **App-only image for OTA** (this is what devices download over-the-air). |
| `STAYCOOL_PREPROD_v4.3.0_app.bin.sig` | RSA-2048 signature of the app image (base64). Host this next to the app as `<url>.sig` for OTA. |
| `STAYCOOL_PREPROD_v4.3.0_app.bin.sha256` | The app SHA-256 — this hex goes into the OTA MQTT command. |
| `components/` | The 4 separate bins (classic multi-offset method). |

## Flash over USB (factory / bench)

```
esptool.py --chip esp32c3 --port COM5 --baud 921600 write_flash 0x0 STAYCOOL_PREPROD_v4.3.0_merged.bin
```
Replace `COM5` with the board's port. If it won't enter download mode: hold BOOT,
tap RESET, release BOOT.

Verify first (PowerShell): `Get-FileHash .\STAYCOOL_PREPROD_v4.3.0_merged.bin -Algorithm SHA256`
— must match the merged SHA-256 above.

### Component offsets (if flashing the 4 bins separately)

```
0x0      components/bootloader.bin
0x8000   components/partitions.bin
0xe000   components/boot_app0.bin
0x10000  components/firmware.bin
```

## Updating later WITHOUT re-flashing (OTA)

Devices flashed with this build can be updated over-the-air — no USB needed.
The app image here is already signed and ready to host:

1. Put `STAYCOOL_PREPROD_v4.3.0_app.bin` and its `.sig` on an HTTPS host
   (same base URL, `.sig` appended). Keep the URL short (< 200 chars) so the
   sensor relay frame fits.
2. Publish an OTA command to `achas/house/<H>/gateway/command`:
   ```json
   {"cmd":"OTA","url":"https://host/f/app.bin","sha256":"ad04f7327f30f061e7dc3aa078562d1cdfe227302177a895d66be37b21d9693d"}
   ```
3. The device downloads, checks SHA-256, verifies the RSA signature against the
   public key baked into the firmware, then commits and reboots. Any failure
   keeps the old firmware (fail-closed — no brick).

The signature in this package was verified against the firmware's embedded
public key at build time (`Verified OK`).

## Reporting / wake behaviour

- **Sensors** deep-sleep ~895 s + up to 30 s random jitter, wake ~0.4–0.7 s to
  measure and transmit, then sleep again → **~15-minute** reporting cycle.
- **Gateway** never sleeps; reads its own sensor every 15 minutes and forwards
  each sensor reading as it arrives.

> Flash encryption / Secure Boot are **OFF** in this build (pre-production), so
> any unit can be re-flashed or erased over USB. Do not enable eFuse hardening
> until the real production run — it is a one-way operation.
