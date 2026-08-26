#include "persist.h"

#include <Arduino.h>
#include <avr/eeprom.h>
#include <string.h>

#include "config.h"
#include "motion.h"
#include "protocol.h"

namespace persist {
namespace {

/* One snapshot of the persisted axes. `seq` increments on every write and the
 * highest one with a good CRC wins at boot, which is what makes the ring
 * self-describing — no separate index to keep consistent. */
struct record_t {
    uint16_t seq;
    int32_t  pos_um[PERSIST_AXIS_COUNT];
    uint8_t  referenced_mask;   /* bit per persisted axis with a real zero */
    uint8_t  pad;
    uint16_t crc;
};

const uint16_t RECORD_SIZE = (uint16_t)sizeof(record_t);
const uint16_t SLOT_COUNT  = PERSIST_SLOT_COUNT;

/* The ring has to fit, and the CRC has to cover everything ahead of it. Both
 * are easy to break by adding a field, so they are checked by the compiler
 * rather than by a comment. */
static_assert(sizeof(record_t) == 2 + 4 * PERSIST_AXIS_COUNT + 2 + 2,
              "record_t gained padding or a field; the CRC span assumes its layout");
static_assert((uint32_t)PERSIST_SLOT_COUNT * sizeof(record_t) <= (uint32_t)E2END + 1,
              "the position ring does not fit in this part's EEPROM");

/* Wear levelling. Each write lands in the next slot, so a slot is only
 * rewritten once every SLOT_COUNT records. At roughly one record per layer
 * that is SLOT_COUNT * 100k layers before the endurance limit matters, which
 * is thousands of full prints. */
uint16_t next_slot;

record_t live;        /* what we would write */
record_t written;     /* what is already in EEPROM */
bool     dirty;
uint32_t still_since_ms;
uint32_t last_write_ms;
bool     ever_written;

/* Byte-at-a-time write in progress. */
bool     writing;
uint16_t write_addr;
uint8_t  write_index;
uint16_t write_count;

uint16_t crcOf(const record_t &r)
{
    return crc16_ccitt((const uint8_t *)&r, (uint16_t)(RECORD_SIZE - sizeof(uint16_t)));
}

bool slotValid(const record_t &r)
{
    return r.crc == crcOf(r);
}

void beginWrite()
{
    live.seq = (uint16_t)(written.seq + 1);
    live.pad = 0;
    live.crc = crcOf(live);

    write_addr  = (uint16_t)(next_slot * RECORD_SIZE);
    write_index = 0;
    writing     = true;
    dirty       = false;
}

uint8_t persistSlotOf(uint8_t axis)
{
    /* Only AXIS_FEED and AXIS_BED are persisted; see the header for why the
     * recoater is not. */
    if (axis == AXIS_FEED) return 0;
    if (axis == AXIS_BED)  return 1;
    return 0xFF;
}

}  // namespace

void begin()
{
    memset(&live, 0, sizeof(live));
    memset(&written, 0, sizeof(written));
    dirty = false;
    writing = false;
    write_count = 0;
    next_slot = 0;
    still_since_ms = millis();
    last_write_ms = millis();
    ever_written = false;

    /* Find the newest good record. A blank or corrupt EEPROM simply yields
     * nothing, and every axis boots with no position. */
    bool found = false;
    record_t best;
    uint16_t best_slot = 0;
    memset(&best, 0, sizeof(best));

    for (uint16_t slot = 0; slot < SLOT_COUNT; slot++) {
        record_t r;
        eeprom_read_block(&r, (const void *)(uintptr_t)(slot * RECORD_SIZE), RECORD_SIZE);
        if (!slotValid(r)) continue;
        /* Sequence numbers wrap, so "newer" is a signed difference rather than
         * a plain comparison. */
        if (!found || (int16_t)(r.seq - best.seq) > 0) {
            best = r;
            best_slot = slot;
            found = true;
        }
    }

    if (found) {
        written = best;
        live    = best;
        next_slot = (uint16_t)((best_slot + 1) % SLOT_COUNT);
    }
}

void service()
{
    if (writing) {
        if (!eeprom_is_ready()) return;
        /* update_ rather than write_: an unchanged byte costs nothing and
         * burns no endurance, and most of a record is unchanged. */
        eeprom_update_byte((uint8_t *)(uintptr_t)(write_addr + write_index),
                           ((const uint8_t *)&live)[write_index]);
        write_index++;
        if (write_index >= RECORD_SIZE) {
            writing = false;
            written = live;
            next_slot = (uint16_t)((next_slot + 1) % SLOT_COUNT);
            write_count++;
            last_write_ms = millis();
            ever_written = true;
        }
        return;
    }

    /* Kept running whether or not there is anything to write — otherwise a
     * change arriving after a long quiet spell would look instantly settled. */
    if (motion::anyBusy()) {
        still_since_ms = millis();
        return;
    }

    if (!dirty) return;
    if (millis() - still_since_ms < PERSIST_SETTLE_MS) return;
    /* Spacing, so a burst of setup jogs becomes one record rather than one
     * each. A print's per-layer moves are much further apart than this, so
     * every layer is still saved. */
    if (ever_written && (millis() - last_write_ms) < PERSIST_MIN_INTERVAL_MS) return;

    beginWrite();
}

bool restored(uint8_t axis)
{
    const uint8_t slot = persistSlotOf(axis);
    if (slot == 0xFF) return false;
    return (written.referenced_mask & (uint8_t)(1u << slot)) != 0;
}

int32_t restoredPosition_um(uint8_t axis)
{
    const uint8_t slot = persistSlotOf(axis);
    return (slot == 0xFF) ? 0 : written.pos_um[slot];
}

void note(uint8_t axis, int32_t position_um, bool referenced)
{
    const uint8_t slot = persistSlotOf(axis);
    if (slot == 0xFF) return;

    const uint8_t bit = (uint8_t)(1u << slot);
    const uint8_t mask = referenced ? (uint8_t)(live.referenced_mask | bit)
                                    : (uint8_t)(live.referenced_mask & ~bit);

    /* A sub-threshold change is not worth an EEPROM cycle. The pistons move by
     * a layer thickness at a time, so this only filters out noise from the
     * step/micrometre round trip. */
    const int32_t delta = position_um - live.pos_um[slot];
    const bool moved = (delta > PERSIST_MIN_DELTA_UM) || (delta < -PERSIST_MIN_DELTA_UM);

    if (!moved && mask == live.referenced_mask) return;

    live.pos_um[slot] = position_um;
    live.referenced_mask = mask;
    dirty = true;
}

void forget(uint8_t axis)
{
    const uint8_t slot = persistSlotOf(axis);
    if (slot == 0xFF) return;
    const uint8_t bit = (uint8_t)(1u << slot);
    if (!(live.referenced_mask & bit)) return;
    live.referenced_mask &= (uint8_t)~bit;
    live.pos_um[slot] = 0;
    dirty = true;
}

uint16_t writes() { return write_count; }

}  // namespace persist
