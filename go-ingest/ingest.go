//go:build ignore

// ============================================================================
//  STAYCOOL / ACHAS -- MQTT -> decrypt -> InfluxDB ingester  (Go reference)
// ============================================================================
//
//  STATUS: PREPARED, NOT ACTIVE. Node-RED is still the live ingester.
//  The `//go:build ignore` tag at the top means this file is deliberately
//  EXCLUDED from any build (`go build ./...` skips it) so it cannot run by
//  accident. When you are ready to migrate off Node-RED:
//     1. remove the `//go:build ignore` line,
//     2. `go mod init achas-ingest && go mod tidy`,
//     3. fill in the CONFIG block + the two `// TODO` endpoints,
//     4. run it IN PARALLEL with Node-RED writing to a SEPARATE bucket,
//     5. compare 24 h, then cut over (see README.md).
//
//  WHY Go IS SAFE HERE
//  -------------------
//  It only has to reproduce what Node-RED already does. Nothing in the
//  firmware changes. The one thing that must match EXACTLY is the AES-128-GCM
//  envelope the device produces (see decrypt() below) -- Go's crypto/cipher
//  GCM is a byte-for-byte match, so it will not error if the layout is right.
//
//  THE CONTRACT (from the firmware, which does NOT change)
//  -------------------------------------------------------
//   * MQTT topic (mTLS 8883):  achas/house/<H>/box/<B>/data
//   * MQTT message (envelope): {"v":5,"house":H,"id":"DEVID","ct":"<base64>"}
//   * ct = base64( IV[12] || ciphertext || tag[16] ), AES-128-GCM, NO AAD,
//          16-byte per-house key (fetched from the Fleet API).
//   * inner JSON (after decrypt): ts,u,h,d,b,v,seq,wt,ta,rh,bat,f,bl,se,fo,
//          gw,dn,rt,loc,rssi,snr.  (wt = wake_ms, loc = room; both new in fw 4.3.0)
//   * anti-replay: drop any seq <= the last seq already seen for that device.
//
// ============================================================================

package main

import (
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"strings"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	influxdb2 "github.com/influxdata/influxdb-client-go/v2"
)

// ----------------------------------------------------------------------------
// CONFIG  (fill these in when activating)
// ----------------------------------------------------------------------------
var cfg = struct {
	MQTTBroker string // e.g. "ssl://achas-iot.sustainablelivinglab.org:8883"
	MQTTCACert string // path to the broker Root CA (PEM)
	MQTTCert   string // this ingester's client cert (PEM)  -- mTLS
	MQTTKey    string // this ingester's client key  (PEM)
	MQTTTopic  string // "achas/house/+/box/+/data"

	FleetAPIBase string // e.g. "https://achas-iot.sustainablelivinglab.org:8443"
	FleetAPITok  string // API token for the Fleet API (keep out of source in prod)

	InfluxURL    string // e.g. "http://127.0.0.1:8086"
	InfluxToken  string
	InfluxOrg    string
	InfluxBucket string // START with a SEPARATE bucket, e.g. "readings_go"
	InfluxMeas   string // measurement name, e.g. "readings"
}{
	MQTTBroker:   "ssl://achas-iot.sustainablelivinglab.org:8883",
	MQTTTopic:    "achas/house/+/box/+/data",
	FleetAPIBase: "https://achas-iot.sustainablelivinglab.org:8443",
	InfluxBucket: "readings_go", // parallel-run bucket; swap to prod after A/B
	InfluxMeas:   "readings",
}

// ----------------------------------------------------------------------------
// Envelope + inner-JSON shapes (see contract above)
// ----------------------------------------------------------------------------
type envelope struct {
	V     int    `json:"v"`
	House int    `json:"house"`
	ID    string `json:"id"`
	CT    string `json:"ct"`
}

type reading struct {
	TS   string   `json:"ts"`
	U    *int64   `json:"u"`
	H    int      `json:"h"`
	D    string   `json:"d"`
	B    int      `json:"b"`
	V    int      `json:"v"`
	Seq  uint32   `json:"seq"`
	WT   uint16   `json:"wt"`  // wake_ms (new in fw 4.3.0)
	TA   *float64 `json:"ta"`  // null on sensor error
	RH   *float64 `json:"rh"`  // null on sensor error
	Bat  *float64 `json:"bat"` // null on read fail
	F    int      `json:"f"`
	GW   int      `json:"gw"`
	DN   string   `json:"dn"`
	RT   string   `json:"rt"`
	Loc  *string  `json:"loc"`  // room; set for gateway, null for sensor
	RSSI *float64 `json:"rssi"` // null for gateway's own data
}

// ----------------------------------------------------------------------------
// Small caches: per-house AES key, per-device last seq, device->room map.
// All guarded by a single mutex for simplicity.
// ----------------------------------------------------------------------------
var (
	mu       sync.Mutex
	houseKey = map[int][]byte{}    // house_id -> 16-byte key
	lastSeq  = map[string]uint32{} // device_id -> last seq accepted
	roomMap  = map[string]string{} // device_id (UPPER hex) -> room name
)

// ----------------------------------------------------------------------------
// decrypt -- THE critical compatibility function. Must match the firmware's
// AES-128-GCM envelope exactly: base64( IV[12] || ciphertext || tag[16] ), no AAD.
// Go's GCM defaults (12-byte nonce, 16-byte tag appended) line up perfectly.
// ----------------------------------------------------------------------------
func decrypt(key []byte, ctB64 string) ([]byte, error) {
	env, err := base64.StdEncoding.DecodeString(ctB64)
	if err != nil {
		return nil, fmt.Errorf("base64: %w", err)
	}
	const ivLen, tagLen = 12, 16
	if len(env) < ivLen+tagLen {
		return nil, errors.New("envelope too short")
	}
	iv := env[:ivLen]
	ctAndTag := env[ivLen:] // ciphertext || tag(16) -- exactly what gcm.Open wants
	block, err := aes.NewCipher(key) // 16-byte key -> AES-128
	if err != nil {
		return nil, err
	}
	gcm, err := cipher.NewGCM(block) // nonce 12, tag 16 (defaults)
	if err != nil {
		return nil, err
	}
	return gcm.Open(nil, iv, ctAndTag, nil) // AAD = nil
}

// getHouseKey returns the cached 16-byte key or fetches it from the Fleet API.
func getHouseKey(house int) ([]byte, error) {
	mu.Lock()
	k, ok := houseKey[house]
	mu.Unlock()
	if ok {
		return k, nil
	}
	// TODO: GET {FleetAPIBase}/houses/<house>/key  ->  {"house_key_hex":"..32 hex.."}
	//       with Authorization: Bearer {FleetAPITok}. Decode hex -> 16 bytes.
	//       Left as a stub so this dormant file has no live network calls.
	return nil, fmt.Errorf("house key fetch not wired yet (house %d)", house)
}

// resolveRoom returns the room for a reading: the telemetry "loc" if present
// (gateway), else a device->room lookup from the enrol map (sensor).
func resolveRoom(r *reading) string {
	if r.Loc != nil && *r.Loc != "" {
		return *r.Loc
	}
	mu.Lock()
	room := roomMap[strings.ToUpper(r.D)]
	mu.Unlock()
	return room // "" if unknown -> just leave the tag empty
}

// ----------------------------------------------------------------------------
// onMessage -- the MQTT callback: envelope -> key -> decrypt -> anti-replay ->
// parse -> InfluxDB. This mirrors the Node-RED flow one-to-one.
// ----------------------------------------------------------------------------
func onMessage(write influxWriter) mqtt.MessageHandler {
	return func(_ mqtt.Client, m mqtt.Message) {
		var env envelope
		if err := json.Unmarshal(m.Payload(), &env); err != nil {
			log.Printf("bad envelope on %s: %v", m.Topic(), err)
			return
		}

		key, err := getHouseKey(env.House)
		if err != nil {
			log.Printf("house %d key: %v", env.House, err)
			return
		}

		plain, err := decrypt(key, env.CT)
		if err != nil {
			log.Printf("decrypt house %d id %s: %v", env.House, env.ID, err)
			return
		}

		var r reading
		if err := json.Unmarshal(plain, &r); err != nil {
			log.Printf("bad inner json: %v", err)
			return
		}

		// Anti-replay: drop non-increasing seq (same rule as Node-RED / backend).
		mu.Lock()
		prev, seen := lastSeq[r.D]
		if seen && r.Seq <= prev {
			mu.Unlock()
			log.Printf("replay/stale drop dev %s seq %d <= %d", r.D, r.Seq, prev)
			return
		}
		lastSeq[r.D] = r.Seq
		mu.Unlock()

		// Timestamp: prefer the device's own unix time; fall back to now.
		ts := time.Now()
		if r.U != nil && *r.U > 1_700_000_000 {
			ts = time.Unix(*r.U, 0)
		}

		tags := map[string]string{
			"house":  fmt.Sprintf("%d", r.H),
			"device": r.D,
			"box":    fmt.Sprintf("%d", r.B),
			"role":   r.RT,
			"room":   resolveRoom(&r), // gateway loc, else enrol-map lookup
		}
		fields := map[string]interface{}{
			"seq":     int64(r.Seq),
			"wake_ms": int(r.WT), // <-- the new wake-time metric
			"flags":   r.F,
		}
		if r.TA != nil {
			fields["temperature"] = *r.TA
		}
		if r.RH != nil {
			fields["humidity"] = *r.RH
		}
		if r.Bat != nil {
			fields["battery_v"] = *r.Bat
		}
		if r.RSSI != nil {
			fields["rssi"] = *r.RSSI
		}

		if err := write(tags, fields, ts); err != nil {
			log.Printf("influx write dev %s: %v", r.D, err)
		}
	}
}

// influxWriter is a tiny seam so the handler doesn't depend on the client type
// (also makes it trivial to unit-test decrypt/anti-replay without a real DB).
type influxWriter func(tags map[string]string, fields map[string]interface{}, ts time.Time) error

func newTLSConfig() (*tls.Config, error) {
	ca, err := os.ReadFile(cfg.MQTTCACert)
	if err != nil {
		return nil, err
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(ca) {
		return nil, errors.New("bad CA pem")
	}
	cert, err := tls.LoadX509KeyPair(cfg.MQTTCert, cfg.MQTTKey)
	if err != nil {
		return nil, err
	}
	return &tls.Config{RootCAs: pool, Certificates: []tls.Certificate{cert}}, nil
}

func main() {
	// InfluxDB writer (blocking API keeps back-pressure simple).
	ic := influxdb2.NewClient(cfg.InfluxURL, cfg.InfluxToken)
	defer ic.Close()
	api := ic.WriteAPIBlocking(cfg.InfluxOrg, cfg.InfluxBucket)
	write := func(tags map[string]string, fields map[string]interface{}, ts time.Time) error {
		p := influxdb2.NewPoint(cfg.InfluxMeas, tags, fields, ts)
		return api.WritePoint(context.Background(), p)
	}

	tlsCfg, err := newTLSConfig()
	if err != nil {
		log.Fatalf("tls: %v", err)
	}
	opts := mqtt.NewClientOptions().
		AddBroker(cfg.MQTTBroker).
		SetClientID("achas-go-ingest"). // DIFFERENT id from Node-RED during parallel run
		SetTLSConfig(tlsCfg).
		SetAutoReconnect(true).
		SetOnConnectHandler(func(c mqtt.Client) {
			if t := c.Subscribe(cfg.MQTTTopic, 0, onMessage(write)); t.Wait() && t.Error() != nil {
				log.Fatalf("subscribe: %v", t.Error())
			}
			log.Printf("subscribed to %s", cfg.MQTTTopic)
		})

	client := mqtt.NewClient(opts)
	if t := client.Connect(); t.Wait() && t.Error() != nil {
		log.Fatalf("connect: %v", t.Error())
	}
	log.Println("ingester running (parallel mode) -- Ctrl-C to stop")
	select {} // block forever

	// TODO before production:
	//   * wire getHouseKey() to the Fleet API (GET /houses/<id>/key)
	//   * add a goroutine polling the Fleet API device list into roomMap
	//   * persist lastSeq (Redis/Postgres) so a restart can't accept a replay
}
