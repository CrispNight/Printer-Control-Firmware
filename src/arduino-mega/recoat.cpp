#include "recoat.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "motion.h"

namespace recoat {
namespace {

/* Steps of the cycle. Which of them run, and in what order, depends on the
 * park mode — see nextStep().
 *
 * PARK_OVERFLOW (the default) leaves the blade at the far end, so the
 * build-plate drop that makes room for the next layer is also the clearance
 * for the return traverse. Powder is spread on the forward pass only and the
 * blade never crosses freshly spread powder:
 *
 *   bed drops -> blade returns over the lowered bed -> supply rises ->
 *   settle -> blade sweeps supply to overflow, spreading
 *
 * PARK_SUPPLY is what the old firmware did. Spreading still happens on the
 * forward pass, but the blade then has to come back over the fresh layer, so
 * it costs an extra drop and raise of the most accuracy-critical axis in the
 * machine:
 *
 *   supply rises -> bed drops -> settle -> sweep -> extra drop ->
 *   blade returns -> raise back
 */
enum step_t {
    S_IDLE = 0,
    S_FEED_RISE,
    S_BED_DROP,
    S_SETTLE,
    S_SWEEP,
    S_SWEEP_PAUSE,
    S_CLEAR_DROP,
    S_RETURN,
    S_CLEAR_RAISE,
    S_REHOME,
    S_PARK,
    S_DONE,
};

struct {
    uint8_t  step;
    uint8_t  park_mode;
    uint8_t  passes;
    uint8_t  pass;
    int32_t  feed_um;
    int32_t  bed_um;
    int32_t  clearance_um;
    uint16_t settle_ms;
    uint32_t wipe_speed_um_s;
    uint32_t wait_start_ms;
    uint16_t wait_ms;
    bool     rehome_due;
    bool     rehomed;
    result_t result;
} c;

/* Cycles since the wiper was last re-homed. It is the only open-loop axis, so
 * it is the only one that drifts. */
uint8_t cycles_since_rehome = 0;

void fail()
{
    motion::stopAll();
    c.step = S_IDLE;
    c.result = RESULT_FAILED;
}

bool startMove(uint8_t axis, int32_t delta_um, uint32_t speed_um_s, uint8_t flags)
{
    if (!motion::moveTo(axis, delta_um, speed_um_s, 0,
                        (uint8_t)(flags | AXIS_MOVE_RELATIVE))) {
        fail();
        return false;
    }
    return true;
}

bool startMoveAbs(uint8_t axis, int32_t target_um, uint32_t speed_um_s, uint8_t flags)
{
    if (!motion::moveTo(axis, target_um, speed_um_s, 0, flags)) {
        fail();
        return false;
    }
    return true;
}

void beginWait(uint16_t ms, uint8_t next)
{
    c.wait_ms = ms;
    c.wait_start_ms = millis();
    c.step = next;
}

/* Advance to the next step and start whatever work it needs. Returns with
 * c.step set to what is now in progress. */
void enter(uint8_t step);

/* Which step follows `done`, for the park mode in force.
 *
 * The wiper re-home is not appended to the end of the cycle. In overflow park
 * the blade finishes at the far end with the layer already spread beneath it,
 * so homing there would drag it straight back across fresh powder. It replaces
 * the return traverse instead — the blade is crossing the lowered, unspread bed
 * at that point anyway, so the re-home is very nearly free. In supply park the
 * blade already ends at the supply end, next to its switch, so the home goes at
 * the end as the old firmware had it. */
uint8_t nextStep(uint8_t done)
{
    const bool overflow = (c.park_mode == PARK_OVERFLOW);
    const bool clearing = (c.park_mode == PARK_SUPPLY) && (c.clearance_um != 0);

    switch (done) {
    case S_BED_DROP:
        if (!overflow) return S_SETTLE;
        return c.rehome_due ? S_REHOME : S_RETURN;

    case S_REHOME:
        return S_PARK;

    case S_PARK:
        /* Overflow park reached here instead of a return traverse, so pick up
         * where that would have led. */
        if (overflow) return (c.pass == 0) ? S_FEED_RISE : S_SWEEP;
        return S_DONE;

    case S_RETURN:
        if (overflow) return (c.pass == 0) ? S_FEED_RISE : S_SWEEP;
        if (clearing) return S_CLEAR_RAISE;
        c.pass++;
        if (c.pass < c.passes) return S_SWEEP;
        return c.rehome_due ? S_REHOME : S_DONE;

    case S_FEED_RISE:
        return overflow ? S_SETTLE : S_BED_DROP;

    case S_SETTLE:
        return S_SWEEP;

    case S_SWEEP:
        if (overflow) {
            c.pass++;
            return (c.pass < c.passes) ? S_RETURN : S_DONE;
        }
        return S_SWEEP_PAUSE;

    case S_SWEEP_PAUSE:
        return clearing ? S_CLEAR_DROP : S_RETURN;

    case S_CLEAR_DROP:
        return S_RETURN;

    case S_CLEAR_RAISE:
        c.pass++;
        if (c.pass < c.passes) return S_SWEEP;
        return c.rehome_due ? S_REHOME : S_DONE;

    default:
        return S_DONE;
    }
}

void enter(uint8_t step)
{
    switch (step) {
    case S_FEED_RISE:
        c.step = S_FEED_RISE;
        startMove(AXIS_FEED, c.feed_um, 0, 0);
        return;

    case S_BED_DROP:
        c.step = S_BED_DROP;
        /* The bed always takes up its backlash the same way, so the layer
         * thickness is repeatable. */
        startMove(AXIS_BED, c.bed_um, 0, AXIS_MOVE_APPROACH_NEG);
        return;

    case S_SETTLE:
        beginWait(c.settle_ms, S_SETTLE);
        return;

    case S_SWEEP:
        c.step = S_SWEEP;
        startMoveAbs(AXIS_WIPE, RECOAT_DISTANCE_UM, c.wipe_speed_um_s, 0);
        return;

    case S_SWEEP_PAUSE:
        beginWait(RECOAT_SWEEP_PAUSE_MS, S_SWEEP_PAUSE);
        return;

    case S_CLEAR_DROP:
        c.step = S_CLEAR_DROP;
        startMove(AXIS_BED, -c.clearance_um, 0, AXIS_MOVE_APPROACH_NEG);
        return;

    case S_RETURN:
        c.step = S_RETURN;
        startMoveAbs(AXIS_WIPE, RECOAT_SUPPLY_PARK_UM, c.wipe_speed_um_s, 0);
        return;

    case S_CLEAR_RAISE:
        c.step = S_CLEAR_RAISE;
        startMove(AXIS_BED, c.clearance_um, 0, 0);
        return;

    case S_REHOME:
        c.step = S_REHOME;
        c.rehomed = true;
        if (!motion::home(AXIS_WIPE)) fail();
        return;

    case S_PARK:
        /* Homing leaves the blade at the post-home park offset; put it back
         * where a sweep starts so the next cycle is identical to this one. */
        c.step = S_PARK;
        startMoveAbs(AXIS_WIPE, RECOAT_SUPPLY_PARK_UM, c.wipe_speed_um_s, 0);
        return;

    case S_DONE:
    default:
        c.step = S_IDLE;
        cycles_since_rehome = c.rehomed ? 0 : (uint8_t)(cycles_since_rehome + 1);
        c.result = RESULT_OK;
        return;
    }
}

}  // namespace

void begin()
{
    memset((void *)&c, 0, sizeof(c));
    c.step = S_IDLE;
    c.result = RESULT_NONE;
}

uint8_t start(const recoat_cycle_t &req)
{
    if (c.step != S_IDLE) return ACK_BUSY;
    if (motion::anyBusy()) return ACK_BUSY;

    if (req.park_mode > PARK_SUPPLY) return ACK_BAD_PARAM;  /* PARK_STAGED not fitted */
    if (req.clearance_um < 0) return ACK_BAD_PARAM;

    /* The sweep targets are absolute wiper positions, so they only mean
     * anything on a homed wiper. The pistons are closed loop and move by
     * increments, so they do not need homing for this. */
    if (!(motion::statusFlags(AXIS_WIPE) & AXIS_FLAG_HOMED)) return ACK_BAD_STATE;

    c.park_mode       = req.park_mode;
    c.passes          = req.passes ? req.passes : 1;
    c.pass            = 0;
    c.feed_um         = req.feed_um;
    c.bed_um          = req.bed_um;
    c.clearance_um    = req.clearance_um;
    c.settle_ms       = req.settle_ms ? req.settle_ms : RECOAT_SETTLE_DEFAULT_MS;
    c.wipe_speed_um_s = req.wipe_speed_mm_s ? (uint32_t)req.wipe_speed_mm_s * 1000UL : 0UL;
    c.rehome_due      = (cycles_since_rehome >= WIPE_REHOME_INTERVAL);
    c.rehomed         = false;
    c.result          = RESULT_NONE;

    /* Overflow park starts by dropping the bed, which is what clears the
     * return traverse. Supply park raises the supply piston first. */
    enter(c.park_mode == PARK_OVERFLOW ? S_BED_DROP : S_FEED_RISE);
    if (c.step == S_IDLE) {
        /* The first move was rejected — report that to the caller now rather
         * than as a failed cycle later. */
        c.result = RESULT_NONE;
        return ACK_REFUSED;
    }
    return ACK_OK;
}

void service()
{
    if (c.step == S_IDLE) return;

    switch (c.step) {
    case S_SETTLE:
    case S_SWEEP_PAUSE:
        if (millis() - c.wait_start_ms < c.wait_ms) return;
        break;
    default:
        if (motion::anyBusy()) return;
        break;
    }

    enter(nextStep(c.step));
}

void abort()
{
    if (c.step == S_IDLE) return;
    motion::stopAll();
    c.step = S_IDLE;
    c.result = RESULT_FAILED;
}

bool active() { return c.step != S_IDLE; }

result_t consumeResult()
{
    const result_t r = c.result;
    c.result = RESULT_NONE;
    return r;
}

}  // namespace recoat
