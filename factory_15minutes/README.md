# ACHAS Firmware — Factory Flash Package (v4.1.1)

One firmware for every box. A box becomes a gateway or a sensor at provisioning
time (in the web form), not at flashing time — so flash the same file to all.

Merged image SHA-256:
`e7965cfc774bd6921298e696c83491bc1d52e20654336e7287c8def28095ed5c`

## What's in here

| File | Purpose |
|------|---------|
| `ACHAS_PREPROD_v4.1.1_merged.bin` | **The one file to flash.** Bootloader + partitions + app, flash to `0x0`. |
| `ACHAS_PREPROD_v4.1.1_merged.bin.sha256` | Checksum to verify the file above. |
| `components/` | The 4 separate bins (classic multi-offset method). |
| `DEVICE_SETUP_GUIDE.md` | Full step-by-step: flash → provision → data in InfluxDB. |
| `CHANGELOG.md` | What changed in this build. |

## Flash (ESP32-C3)

```
esptool.py --chip esp32c3 --port COM5 --baud 921600 write_flash 0x0 ACHAS_PREPROD_v4.1.1_merged.bin
```
Replace `COM5` with the board's port. If it won't enter download mode: hold BOOT,
tap RESET, release BOOT.

Verify the checksum before flashing (Windows PowerShell):
```
Get-FileHash .\ACHAS_PREPROD_v4.1.1_merged.bin -Algorithm SHA256
```
It must match the SHA-256 above.

## Next

After flashing, follow **DEVICE_SETUP_GUIDE.md** — first boot runs a self-test
(LED codes), then opens the `ACHAS-PROV-XXXX` setup hotspot.

> Flash encryption / Secure Boot are OFF in this build (pre-production), so any
> unit can be re-flashed or erased over USB. Do not enable eFuse hardening until
> the real production run.
