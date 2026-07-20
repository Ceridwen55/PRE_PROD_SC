#include "storage.h"
#include "log.h"

// ------------------------------------------------------------
// NVS namespace and key names
//
// Namespace = "folder" name in NVS (max 15 chars)
// Keys      = variable names inside that folder (max 15 chars)
// ------------------------------------------------------------

static const char* NVS_NAMESPACE   = "achas";
static const char* KEY_PENDING     = "pending";     // uint8_t: 1 = has data
static const char* KEY_PAYLOAD     = "payload";     // blob: AchasPayload bytes
static const char* KEY_RETRY_COUNT = "retry_cnt";   // uint8_t: retry counter

// Preferences object — ESP32's NVS wrapper
static Preferences prefs;


//STORAGE INIT

bool storage_init() {

    // Open NVS namespace in read-write mode
    // false = read-write (true would be read-only)
    bool ok = prefs.begin(NVS_NAMESPACE, false);

    if (!ok) {
        LOG.println("[Storage] ERROR: Failed to open NVS namespace.");
        LOG.println("[Storage] Flash may be corrupted or partition missing.");
        return false;
    }

    LOG.println("[Storage] NVS initialized.");

    // Report current state on boot — useful for debugging
    if (prefs.getBool(KEY_PENDING, false)) {
        uint8_t retries = prefs.getUChar(KEY_RETRY_COUNT, 0);
        LOG.printf("[Storage] Found pending payload (retry count: %d)\n", retries);
    } else {
        LOG.println("[Storage] No pending payload.");
    }

    prefs.end();
    return true;
}


//STORAGE SAVE PENDING

void storage_save_pending(const AchasPayload& payload) {

    prefs.begin(NVS_NAMESPACE, false);

    // Store the raw bytes of the struct
    // putBytes() handles any binary data
    size_t written = prefs.putBytes(KEY_PAYLOAD, &payload, sizeof(AchasPayload));

    if (written != sizeof(AchasPayload)) {
        LOG.println("[Storage] ERROR: Failed to write payload to NVS.");
        prefs.end();
        return;
    }

    // Mark as pending and reset retry counter
    prefs.putBool(KEY_PENDING, true);
    prefs.putUChar(KEY_RETRY_COUNT, 0);

    prefs.end();
    LOG.printf("[Storage] Payload saved to NVS (%d bytes).\n", written);
}


//STORAGE HAS PENDING

bool storage_has_pending() {

    prefs.begin(NVS_NAMESPACE, true);   // read-only
    bool pending = prefs.getBool(KEY_PENDING, false);
    prefs.end();
    return pending;
}


//STORAGE LOAD PENDING

bool storage_load_pending(AchasPayload& out) {

    prefs.begin(NVS_NAMESPACE, true);   // read-only

    if (!prefs.getBool(KEY_PENDING, false)) {
        prefs.end();
        return false;   // nothing saved
    }

    size_t bytes_read = prefs.getBytes(KEY_PAYLOAD, &out, sizeof(AchasPayload));
    prefs.end();

    if (bytes_read != sizeof(AchasPayload)) {
        LOG.println("[Storage] ERROR: Payload size mismatch on load.");
        return false;
    }

    LOG.println("[Storage] Pending payload loaded from NVS.");
    return true;
}


//STORAGE INCREMENT RETRY

uint8_t storage_increment_retry() {

    prefs.begin(NVS_NAMESPACE, false);
    uint8_t count = prefs.getUChar(KEY_RETRY_COUNT, 0);
    count++;
    prefs.putUChar(KEY_RETRY_COUNT, count);
    prefs.end();

    LOG.printf("[Storage] Retry count incremented to %d/%d.\n",
        count, STORAGE_MAX_RETRIES);

    return count;
}


// STORAGE GET RETRY

uint8_t storage_get_retry_count() {

    prefs.begin(NVS_NAMESPACE, true);
    uint8_t count = prefs.getUChar(KEY_RETRY_COUNT, 0);
    prefs.end();
    return count;
}

//STORAGE CLEAR PENDING

void storage_clear_pending() {

    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(KEY_PENDING, false);
    prefs.putUChar(KEY_RETRY_COUNT, 0);
    prefs.end();

    LOG.println("[Storage] Pending payload cleared.");
}