// core/seq_counter.cpp
//
// See seq_counter.h for the full "why". Short version: keep a boot counter
// in NVS so the anti-replay seq number never goes backwards across reboots.

#include "seq_counter.h"
#include "log.h"

#include <Arduino.h>
#include <Preferences.h>


// NVS namespace + key where the boot counter lives.
static const char* NS  = "seq";
static const char* KEY = "boot";


uint32_t seq_boot_base() {
    Preferences p;
    uint32_t boot = 0;

    if (p.begin(NS, false)) {
        boot = p.getUInt(KEY, 0);   // 0 on a brand-new chip
        boot += 1;
        p.putUInt(KEY, boot);       // one small write per boot
        p.end();
    } else {
        // If NVS is somehow unavailable we still return a usable base; the
        // worst case is a single boot's data being treated as a replay.
        LOG.println("[Seq] WARN: NVS unavailable -- boot base not persisted.");
    }

    // Low 16 bits of the boot counter become the high half of the seq, so
    // each boot starts a new, higher block of message numbers.
    uint32_t base = (boot & 0xFFFF) << 16;
    LOG.printf("[Seq] boot #%lu -> seq base 0x%08lX\n",
                  (unsigned long)boot, (unsigned long)base);
    return base;
}
