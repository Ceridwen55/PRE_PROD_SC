# go-ingest — dormant reference (NOT active)

> **The maintained, buildable version now lives in its own project folder:**
> **`../../PREPROD_GO_PORTAL/`** — split into small files, config already filled
> in from the firmware, verified to build/vet clean, with `README.md` +
> `SETUP.md` (a from-zero VPS guide). This `go-ingest/` folder is kept only as
> the original single-file reference. For any real work, use `PREPROD_GO_PORTAL`.

Node-RED is still the live MQTT→InfluxDB ingester. This folder is a **prepared
Go replacement**, kept dormant so nothing runs by accident.

- `ingest.go` starts with `//go:build ignore`, so `go build ./...` **skips it**.
- It reproduces exactly what Node-RED does: subscribe (mTLS) → decrypt
  (AES-128-GCM, matches the firmware envelope) → anti-replay (seq) → write
  InfluxDB. Includes the new `wt` (wake_ms) and `loc` (room) fields.

## Activation (later, when migrating off Node-RED)

1. Delete the `//go:build ignore` line at the top of `ingest.go`.
2. `go mod init achas-ingest && go mod tidy`
   (deps: `github.com/eclipse/paho.mqtt.golang`, `github.com/influxdata/influxdb-client-go/v2`)
3. Fill the `cfg` block (broker, mTLS certs, Fleet API, InfluxDB) and the two
   `// TODO`s: `getHouseKey()` (GET `/houses/<id>/key`) and the room-map poll.
4. **Run in parallel** with Node-RED, writing to a **separate bucket**
   (`readings_go`). Use a different MQTT client-id.
5. Compare ~24 h against Node-RED's bucket. If identical, point it at the prod
   bucket and stop Node-RED.
6. Rollback = start Node-RED, stop Go. (Parallel + separate bucket makes this instant.)

## The one thing that must match: the crypto envelope

`ct` = base64( `IV[12] || ciphertext || tag[16]` ), AES-128-GCM, **no AAD**,
16-byte per-house key. Validate `decrypt()` against one real message before
trusting anything else — if that works, everything else is plumbing.
