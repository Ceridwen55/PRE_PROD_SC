/*
  ============================================================
   drivers/EspNow.h  -- the wireless link (replaces the old LoRa)
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  This is how a sensor box talks to the gateway. We use ESP-NOW: a
  tiny, fast, connectionless protocol built into every ESP32's WiFi.
  No router, no WiFi password, no IP address -- one ESP32 sends a
  small packet straight to another using only the WiFi radio.

  WHERE IT IS USED
  ----------------
   * SENSOR box (main.cpp)      -> espnow_send_payload(): send a reading
   * GATEWAY    (core/tasks.cpp) -> espnow_receive(): get the next packet
                                    espnow_reply():   answer a sensor
                                    espnow_queue_command(): schedule a
                                                            command for a box

  THE CHANNEL PROBLEM  (read once -- it explains the weird "all channels" loop)
  ----------------------------------------------------------------------------
  ESP-NOW can ONLY talk between two chips that sit on the SAME WiFi
  channel (1..13). The gateway's channel is chosen by the WiFi router /
  4G dongle it connects to -- we don't control it. A sleeping sensor
  has no idea which channel that is.

  Foolproof fix: the sensor sends its packet on EVERY channel, one
  after another, and listens for a reply on each channel right after
  sending. On whichever channel the gateway really is, the packet AND
  the reply line up perfectly.

  HOW WE KNOW THE PACKET ARRIVED  (acknowledgement / "ACK")
  ---------------------------------------------------------
  An ESP-NOW broadcast has no built-in delivery confirmation. So the
  gateway, every time it receives a valid reading, sends a short reply
  back to that sensor:
     * a real COMMAND if one is waiting for that box (OTA/PING/etc), OR
     * a plain "ACK" if there is nothing to say.
  Either reply proves to the sensor "you were heard". If the sensor
  hears NOTHING on any channel, it saves the reading and retries on the
  next wake (see core/storage.h).

  STEP-BY-STEP -- SENSOR SEND (espnow_send_payload)
  -------------------------------------------------
    1. for channel = 1 .. 13:
    2.     switch the radio to that channel
    3.     broadcast the encrypted packet
    4.     listen ~150 ms for a reply on the same channel
    5.     if a reply arrives -> we were heard (delivered = true);
           parse it in case it is a command, then stop early
    6. return a SendResult (delivered? + any command to act on)

  STEP-BY-STEP -- GATEWAY RECEIVE (in core/tasks.cpp)
  ---------------------------------------------------
    1. a packet arrives -> the recv callback copies it into a queue
    2. espnow_receive() hands the next packet to the gateway task
    3. the task decrypts + checks it, then ALWAYS replies (command/ACK)
*/

#pragma once

#include <Arduino.h>
#include <stdint.h>


// ------------------------------------------------------------------
// One received packet (what the gateway gets from a sensor).
// ------------------------------------------------------------------
struct EspNowPacket {
    uint8_t  mac[6];      // sender's WiFi MAC (so the gateway can reply)
    uint8_t  data[64];    // the raw (still encrypted) bytes
    size_t   length;      // how many bytes are in data[]
    int8_t   rssi;        // signal strength in dBm (0 if unknown)
    bool     valid;       // true = this packet is real (false = timeout)
};


// ------------------------------------------------------------------
// Commands the gateway can send back to a sensor (downlink).
// ------------------------------------------------------------------
enum class Command {
    NONE,     // nothing / just an ACK
    OTA,      // update firmware from a URL
    PING,     // "are you alive?"
    REBOOT,   // restart now
    PAIR,     // open BLE pairing
    UNKNOWN   // we got a word we don't recognise
};

// A parsed downlink reply.
struct Downlink {
    Command command;
    String  ota_url;     // only filled when command == OTA
    String  ota_sha256;  // only filled when command == OTA
    bool    valid;       // true = there is a real command to act on
    // NOTE: no signature field. The firmware signature is far too big for
    // one ESP-NOW frame -- the sensor fetches "<url>.sig" over HTTPS.
};

// Result of a sensor send attempt.
struct SendResult {
    bool     delivered;  // true = the gateway replied (it heard us)
    Downlink downlink;   // a command to act on (downlink.valid says if real)
};


// ------------------------------------------------------------------
// Setup. Call ONCE.
//   as_gateway = true  on the gateway  (sets up the receive queue)
//   as_gateway = false on a sensor box (sets up the broadcast peer)
// ------------------------------------------------------------------
bool espnow_init(bool as_gateway);

// GATEWAY: re-init ESP-NOW's send path after WiFi joins the AP (call once,
// right after wifi_connect, before the RX task starts replying to sensors).
// Fixes silently-failing ACKs/commands. See EspNow.cpp for the "why".
bool espnow_gateway_reinit();

// Turn the radio off and free ESP-NOW (sensor calls this before BLE
// and before deep sleep so the two radios don't fight).
void espnow_deinit();


// ===== SENSOR side =====
// Broadcast `data` on all channels and listen for the gateway's reply.
// `seq` is this uplink's anti-replay sequence number; it authenticates any
// command in the reply (the gateway signs commands with HMAC(house_key,
// seq || cmd), so a stale/forged command is rejected -- see parse_downlink).
SendResult espnow_send_payload(const uint8_t* data, size_t length, uint32_t seq);


// ===== GATEWAY side =====
// Wait up to timeout_ms for the next received packet.
EspNowPacket espnow_receive(uint32_t timeout_ms);

// Send `message` straight back to one sensor (by its MAC). Used for
// the command/ACK reply right after a packet is received.
bool espnow_reply(const uint8_t* mac, const String& message);

// Schedule a command for a sensor box (by 4-hex device id). It is sent
// the next time that box checks in. Used by core/ota_manager.cpp.
void espnow_queue_command(const String& device_id_hex, const String& message);

// If a command is waiting for this device id, copy it into `out` and return
// true. `current_boot` is the sensor's boot counter from this uplink (top 16
// bits of seq): once it rises above the value seen when delivery started, the
// sensor has rebooted (i.e. it took the OTA) and the command is cleared WITHOUT
// re-sending -- so a landed OTA is not re-applied on later check-ins. The
// per-command try budget only kicks in while the boot counter is unchanged
// (delivery kept getting lost), so nothing is dropped prematurely.
bool espnow_take_pending(uint16_t device_id, uint16_t current_boot, String& out);
