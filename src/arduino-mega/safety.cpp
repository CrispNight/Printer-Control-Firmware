#include "safety.h"

#include <Arduino.h>

#include "config.h"
#include "pins.h"

namespace safety {
namespace {

/* Which PIN_SAFETY indices belong to which fault domain. */
const uint16_t MASK_DOOR   = 0x0003;  /* 0-1 */
const uint16_t MASK_OXYGEN = 0x000C;  /* 2-3 */
const uint16_t MASK_TEMP   = 0x03F0;  /* 4-9 */
/* Index 10 is the green check light — the chain's own "all clear" lamp. It is
 * an indicator, not a fault domain, so it is reported but never turned into a
 * FAULTBIT. */

uint16_t raw_mask;      /* debounced, one bit per PIN_SAFETY index */
uint16_t pending_mask;  /* last raw sample, not yet confirmed */
bool     changed;
bool     estop_latched;
uint32_t last_sample_ms;

uint16_t sampleMask()
{
    uint16_t m = 0;
    for (uint8_t i = 0; i < NUM_SAFETY_PINS; i++)
        if (digitalRead(PIN_SAFETY[i]) == HIGH) m |= (uint16_t)(1u << i);
    return m;
}

}  // namespace

void begin()
{
    for (uint8_t i = 0; i < NUM_SAFETY_PINS; i++)
        pinMode(PIN_SAFETY[i], INPUT);

    raw_mask = pending_mask = sampleMask();
    changed = true;
    estop_latched = false;
    last_sample_ms = millis();
}

void service()
{
    const uint32_t now = millis();
    if (now - last_sample_ms < SENSOR_SAMPLE_INTERVAL_MS) return;
    last_sample_ms = now;

    const uint16_t m = sampleMask();
    /* Two consecutive equal samples before a change counts. The chain acts on
     * its own regardless of what firmware believes, so this only affects what
     * gets reported — it cannot delay a trip. */
    if (m == pending_mask && m != raw_mask) {
        raw_mask = m;
        changed = true;
    }
    pending_mask = m;
}

void fill(safety_status_t &out)
{
    out.interlock_mask = FAULTBIT_DOOR | FAULTBIT_OXYGEN | FAULTBIT_TEMP;
    out.tripped_mask   = trippedMask();
    out.door_ok        = (raw_mask & MASK_DOOR)   ? 0 : 1;
    out.oxygen_ok      = (raw_mask & MASK_OXYGEN) ? 0 : 1;
    out.temp_ok        = (raw_mask & MASK_TEMP)   ? 0 : 1;
    out.estop_active   = estop_latched ? 1 : 0;
}

uint16_t trippedMask()
{
    uint16_t f = 0;
    if (raw_mask & MASK_DOOR)   f |= FAULTBIT_DOOR;
    if (raw_mask & MASK_OXYGEN) f |= FAULTBIT_OXYGEN;
    if (raw_mask & MASK_TEMP)   f |= FAULTBIT_TEMP;
    return f;
}

bool allClear()
{
    return (raw_mask & (MASK_DOOR | MASK_OXYGEN | MASK_TEMP)) == 0;
}

bool consumeChanged()
{
    const bool c = changed;
    changed = false;
    return c;
}

void setEstop(bool latched) { estop_latched = latched; }

}  // namespace safety
