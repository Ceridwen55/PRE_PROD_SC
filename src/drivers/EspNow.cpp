// ============================================================
//  drivers/EspNow.cpp  -- the ESP-NOW wireless link
// ============================================================
//  See EspNow.h for the big-picture explanation. This file is the
//  implementation. It is written to be read top-to-bottom.

#include "EspNow.h"
#include "log.h"
#include "config.h"
#include "core/crypto.h"   // downlink command authentication (HMAC, house key)

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>


// ------------------------------------------------------------------
// Shared state (private to this file -- "static" hides it)
// ------------------------------------------------------------------

// The special "send to everyone" address.
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Which role are we running? Set once in espnow_init().
static bool s_is_gateway = false;
static bool s_ready      = false;

// ----- GATEWAY: incoming packets land in this queue -----
static QueueHandle_t s_rx_queue = nullptr;

// ----- GATEWAY: commands waiting to be sent to sensor boxes -----
// Small fixed table. One slot per box is plenty (we have 3 sensors).
struct PendingCommand {
    bool     active;
    uint16_t device_id;     // lower 16 bits of the sensor's MAC
    char     message[220];  // e.g. "ACHAS|A3F1|OTA|https://...|<sha>"
    uint8_t  tries_left;    // re-deliver over this many check-ins, then drop
    uint16_t boot_at_start; // sensor boot counter when we first delivered
                            // (BOOT_UNSET until then); a rise past it means the
                            // sensor rebooted -> it took the OTA -> stop sending.
};
#define BOOT_UNSET 0xFFFF

// A single ESP-NOW reply is fire-and-forget: the sensor only listens for a
// short window and the ~160-byte command frame is far more likely to be lost
// than a 13-byte ACK. Delivering the command ONCE and immediately discarding
// it meant a single dropped frame killed the OTA forever. Instead we re-send
// the command on up to this many consecutive check-ins (each freshly HMAC-
// signed with that check-in's seq, so anti-replay is preserved). Delivery
// normally stops earlier: as soon as the sensor's boot counter rises we know
// it rebooted (took the OTA) and clear the command -- see espnow_take_pending.
#define PENDING_CMD_TRIES 4

static PendingCommand   s_pending[4];
static SemaphoreHandle_t s_pending_mutex = nullptr;

// ----- SENSOR: the gateway's reply lands in this buffer -----
static volatile bool s_reply_pending = false;
static char          s_reply_buf[220] = {0};
static volatile int  s_reply_len = 0;

// ----- SENSOR: last channel the gateway answered on -----
// Kept in RTC memory (same pattern as the anti-replay counter in main.cpp)
// so it survives deep sleep. 0 = unknown; resets only on full power loss.
// Trying this channel first turns the usual 13-channel blind sweep (~2 s)
// into a single ~150 ms hit when the gateway's channel is stable.
RTC_DATA_ATTR static int s_last_good_channel = 0;


// ==================================================================
//  Small text helpers
// ==================================================================

// Turn a 4-character hex string ("A3F1") into a number (0xA3F1).
static uint16_t hex4_to_u16(const String& s) {
    return (uint16_t)strtoul(s.c_str(), nullptr, 16);
}

// Split `s` into '|'-separated fields (up to `max`), trimming each.
static int split_pipe(const String& s, String* f, int max) {
    int n = 0, start = 0;
    for (int i = 0; i <= (int)s.length() && n < max; i++) {
        if (i == (int)s.length() || s[i] == '|') {
            f[n++] = s.substring(start, i);
            start = i + 1;
        }
    }
    for (int i = 0; i < n; i++) f[i].trim();
    return n;
}

// Parse a downlink message the gateway sent back to us (the sensor).
// Format (fields separated by '|', none of the values contain '|'):
//   "ACHAS|<TARGET>|ACK"                          plain ack (no tag)
//   "ACHAS|<TARGET>|<CMD>|<tag>"                  PING/REBOOT/PAIR
//   "ACHAS|<TARGET>|OTA|<url>|<sha256>|<tag>"     OTA
//
// Every ACTIONABLE command carries a trailing HMAC tag over (seq || the
// rest of the message). Without a valid tag the command is ignored -- this
// is what stops a spoofed REBOOT/PAIR/OTA from a bystander in radio range.
// A plain ACK carries no tag (it triggers no action, only "you were heard").
static Downlink parse_downlink(const String& msg, uint32_t seq) {
    Downlink r;
    r.command = Command::NONE;
    r.valid   = false;

    // Look only at the leading fields to classify the command. These sit in
    // fixed positions whether or not a tag is appended at the end.
    String head[3];
    int hn = split_pipe(msg, head, 3);
    if (hn < 3 || head[0] != "ACHAS") return r;

    String cmd = head[2];
    cmd.toUpperCase();

    // ACK is not an action and carries no tag -- accept as "heard you".
    if (cmd == "ACK") { r.command = Command::NONE; return r; }

    Command c;
    if      (cmd == "OTA")    c = Command::OTA;
    else if (cmd == "PING")   c = Command::PING;
    else if (cmd == "REBOOT") c = Command::REBOOT;
    else if (cmd == "PAIR")   c = Command::PAIR;
    else                      return r;   // unknown word -> ignore

    // Actionable: the tag is the final '|' field. Verify HMAC over the rest.
    int cut = msg.lastIndexOf('|');
    if (cut <= 0) {
        LOG.println("[ESP-NOW] downlink has no auth tag -- ignoring command.");
        return r;
    }
    String signed_part = msg.substring(0, cut);
    String tag         = msg.substring(cut + 1);
    tag.trim();

    if (!crypto_verify_downlink(seq, signed_part.c_str(), tag.c_str())) {
        LOG.println("[ESP-NOW] downlink tag INVALID (forged/replayed?) -- ignoring.");
        return r;
    }

    // Authentic. Now pull OTA args out of the (verified) signed portion.
    r.command = c;
    if (c == Command::OTA) {
        String f[5];
        int n = split_pipe(signed_part, f, 5);
        r.ota_url    = (n > 3) ? f[3] : String();
        r.ota_sha256 = (n > 4) ? f[4] : String();
    }
    r.valid = true;
    return r;
}


// ==================================================================
//  Receive callback -- the WiFi driver calls this whenever an
//  ESP-NOW packet arrives. Keep it SHORT: just copy the data away.
// ==================================================================
#if ESP_IDF_VERSION_MAJOR >= 5
static void on_recv(const esp_now_recv_info_t* info,
                    const uint8_t* data, int len) {
    const uint8_t* src_mac = info->src_addr;
    int8_t rssi = (info->rx_ctrl != nullptr) ? info->rx_ctrl->rssi : 0;
#else
static void on_recv(const uint8_t* src_mac, const uint8_t* data, int len) {
    int8_t rssi = 0;
#endif
    if (len <= 0) return;

    if (s_is_gateway) {
        // Gateway path: an uplink reading (small, encrypted). pkt.data is
        // 64 bytes, so anything larger cannot be a valid reading -- drop it.
        if (len > (int)sizeof(((EspNowPacket*)nullptr)->data)) return;

        // Drop the raw packet into the queue for the task to handle.
        EspNowPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        memcpy(pkt.mac, src_mac, 6);
        memcpy(pkt.data, data, len);
        pkt.length = len;
        pkt.rssi   = rssi;
        pkt.valid  = true;
        // 0 timeout: if the queue is full we just drop this packet.
        xQueueSend(s_rx_queue, &pkt, 0);
    } else {
        // Sensor path: the gateway's reply. This is NOT an uplink reading --
        // it can be a command (an OTA command is ~80-140 B), so it is bounded
        // by s_reply_buf (220 B), NOT the 64-byte uplink limit. The old
        // ">64 drop" above silently killed EVERY OTA command sent to a sensor.
        if (len < (int)sizeof(s_reply_buf)) {
            memcpy(s_reply_buf, data, len);
            s_reply_buf[len] = '\0';
            s_reply_len = len;
            s_reply_pending = true;
        }
    }
}


// ==================================================================
//  espnow_init -- set up WiFi + ESP-NOW for our role.
// ==================================================================
bool espnow_init(bool as_gateway) {

    s_is_gateway = as_gateway;

    // ESP-NOW rides on the WiFi radio, so WiFi must be in station mode.
    WiFi.mode(WIFI_STA);

    if (esp_now_init() != ESP_OK) {
        LOG.println("[ESP-NOW] ERROR: esp_now_init failed.");
        return false;
    }
    esp_now_register_recv_cb(on_recv);

    if (as_gateway) {
        // Gateway: make the queue that incoming packets go into.
        s_rx_queue      = xQueueCreate(8, sizeof(EspNowPacket));
        s_pending_mutex = xSemaphoreCreateMutex();
        memset(s_pending, 0, sizeof(s_pending));
        if (s_rx_queue == nullptr || s_pending_mutex == nullptr) {
            LOG.println("[ESP-NOW] ERROR: queue/mutex alloc failed.");
            return false;
        }
        LOG.println("[ESP-NOW] Ready (gateway: listening for sensor packets).");
    } else {
        // Sensor: register the broadcast address as a peer so we can
        // send to it. channel 0 means "use whatever channel we're on".
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, BROADCAST_MAC, 6);
        peer.ifidx   = WIFI_IF_STA;
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            LOG.println("[ESP-NOW] ERROR: could not add broadcast peer.");
            return false;
        }
        LOG.println("[ESP-NOW] Ready (sensor: will broadcast on all channels).");
    }

    s_ready = true;
    return true;
}


void espnow_deinit() {
    if (!s_ready) return;
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
    s_ready = false;
    LOG.println("[ESP-NOW] Stopped (radio off).");
}


// ==================================================================
//  GATEWAY: re-init ESP-NOW after WiFi has joined the AP.
//  Joining an AP resets ESP-NOW's send-side state (recv keeps working, but
//  esp_now_send() returns ESP_ERR_ESPNOW_NOT_INIT), so replies to sensors
//  silently fail -- the sensor never hears an ACK, retries every wake, and
//  never caches a channel. Re-initialising here, on the now-fixed AP channel,
//  makes the reply path work. We do NOT touch WiFi.mode() (that would drop the
//  AP link) and we keep the RX queue / command table from espnow_init().
// ==================================================================
bool espnow_gateway_reinit() {
    esp_now_deinit();                     // harmless if it was already down
    if (esp_now_init() != ESP_OK) {
        LOG.println("[ESP-NOW] ERROR: gateway re-init failed.");
        return false;
    }
    esp_now_register_recv_cb(on_recv);    // re-hook the receive callback
    s_ready = true;
    LOG.println("[ESP-NOW] Gateway re-init after WiFi connect (reply path ready).");
    return true;
}


// ==================================================================
//  SENSOR: send the packet and listen for a reply. The last channel
//  the gateway answered on is tried first (RTC cache); only on a miss
//  do we fall back to the full 1..13 sweep.
// ==================================================================

// Try ONE channel: hop, broadcast, listen for a reply. On success fills
// `res`, remembers the channel for the next wake, and returns true.
static bool try_channel(int ch, const uint8_t* data, size_t length,
                        uint32_t seq, SendResult& res) {

    // Step 1: hop the radio to this channel.
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    delay(2);   // give the radio a moment to settle

    // Step 2: broadcast the packet. (We can't know yet if anyone heard.)
    s_reply_pending = false;
    esp_now_send(BROADCAST_MAC, data, length);

    // Step 3: listen on THIS channel for the gateway's reply.
    uint32_t start = millis();
    while (millis() - start < ESPNOW_LISTEN_PER_CHANNEL_MS) {
        if (s_reply_pending) {
            s_reply_pending = false;
            String reply(s_reply_buf);
            LOG.printf("[ESP-NOW] reply on ch %d: \"%s\"\n", ch, reply.c_str());
            res.delivered = true;                       // ANY reply = we were heard
            res.downlink  = parse_downlink(reply, seq); // verified before it counts
            s_last_good_channel = ch;                   // fast path for next wake
            return true;
        }
        delay(5);
    }
    return false;
}

SendResult espnow_send_payload(const uint8_t* data, size_t length, uint32_t seq) {

    SendResult res;
    res.delivered        = false;
    res.downlink.command = Command::NONE;
    res.downlink.valid   = false;

    if (!s_ready) {
        LOG.println("[ESP-NOW] ERROR: send before init.");
        return res;
    }

    // Fast path: the channel that worked last wake (survives deep sleep).
    int cached = s_last_good_channel;
    if (cached >= ESPNOW_CHANNEL_MIN && cached <= ESPNOW_CHANNEL_MAX) {
        if (try_channel(cached, data, length, seq, res)) return res;
        LOG.printf("[ESP-NOW] cached ch %d missed -- full sweep.\n", cached);
    }

    // Slow path: sweep every channel (skip the cached one, already tried).
    // On total failure the cache is left as-is, so the next wake still
    // starts with the previously known channel.
    for (int ch = ESPNOW_CHANNEL_MIN; ch <= ESPNOW_CHANNEL_MAX; ch++) {
        if (ch == cached) continue;
        if (try_channel(ch, data, length, seq, res)) return res;
    }

    LOG.println("[ESP-NOW] No reply on any channel -- gateway not reachable.");
    return res;   // delivered stays false
}


// ==================================================================
//  GATEWAY: hand the next received packet to the caller (or time out).
// ==================================================================
EspNowPacket espnow_receive(uint32_t timeout_ms) {
    EspNowPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.valid = false;

    if (s_rx_queue == nullptr) return pkt;
    xQueueReceive(s_rx_queue, &pkt, pdMS_TO_TICKS(timeout_ms));
    return pkt;   // pkt.valid is true only if something was received
}


// ==================================================================
//  GATEWAY: send a reply straight to one sensor (by its MAC).
// ==================================================================
bool espnow_reply(const uint8_t* mac, const String& message) {

    // Add the sensor as a temporary peer (channel 0 = our current channel).
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);   // ok if it already exists

    esp_err_t rc = esp_now_send(mac, (const uint8_t*)message.c_str(),
                                message.length());

    esp_now_del_peer(mac);     // tidy up so the peer table never fills
    return (rc == ESP_OK);
}


// ==================================================================
//  GATEWAY: command queue (set by MQTT side, read by the RX task)
// ==================================================================
void espnow_queue_command(const String& device_id_hex, const String& message) {

    if (s_pending_mutex == nullptr) return;
    uint16_t dev = hex4_to_u16(device_id_hex);

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    // Reuse a slot for the same box, otherwise take a free one.
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (s_pending[i].active && s_pending[i].device_id == dev) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < 4; i++) if (!s_pending[i].active) { slot = i; break; }
    }
    if (slot >= 0) {
        s_pending[slot].active        = true;
        s_pending[slot].device_id     = dev;
        s_pending[slot].tries_left    = PENDING_CMD_TRIES;
        s_pending[slot].boot_at_start = BOOT_UNSET;
        strncpy(s_pending[slot].message, message.c_str(),
                sizeof(s_pending[slot].message) - 1);
        s_pending[slot].message[sizeof(s_pending[slot].message) - 1] = '\0';
        LOG.printf("[ESP-NOW] Command queued for box %04X (%u tries): %s\n",
                      dev, s_pending[slot].tries_left, s_pending[slot].message);
    } else {
        LOG.println("[ESP-NOW] WARN: command queue full -- dropped.");
    }
    xSemaphoreGive(s_pending_mutex);
}


bool espnow_take_pending(uint16_t device_id, uint16_t current_boot, String& out) {

    if (s_pending_mutex == nullptr) return false;
    bool found = false;

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    for (int i = 0; i < 4; i++) {
        if (!(s_pending[i].active && s_pending[i].device_id == device_id)) continue;

        // First time we deliver to this sensor: remember its boot counter.
        if (s_pending[i].boot_at_start == BOOT_UNSET) {
            s_pending[i].boot_at_start = current_boot;
        }
        // Boot counter rose since we started -> the sensor rebooted, i.e. it
        // already took the OTA. Clear the command WITHOUT re-sending so a
        // landed OTA is not re-applied on later check-ins (battery/wear).
        else if (current_boot != s_pending[i].boot_at_start) {
            s_pending[i].active = false;
            LOG.printf("[ESP-NOW] Command for box %04X: sensor rebooted (boot %u->%u), OTA landed -- clearing.\n",
                          device_id, s_pending[i].boot_at_start, current_boot);
            break;   // found stays false: send a plain ACK, no command
        }

        // Same boot -> delivery hasn't taken yet. (Re-)send, bounded by budget.
        out = String(s_pending[i].message);
        if (s_pending[i].tries_left > 0) s_pending[i].tries_left--;
        if (s_pending[i].tries_left == 0) {
            s_pending[i].active = false;   // budget spent -- give up
            LOG.printf("[ESP-NOW] Command for box %04X: last try, dropping after this.\n",
                          device_id);
        } else {
            LOG.printf("[ESP-NOW] Command for box %04X: sent, %u tries left.\n",
                          device_id, s_pending[i].tries_left);
        }
        found = true;
        break;
    }
    xSemaphoreGive(s_pending_mutex);
    return found;
}
