/*
 * persist.h — axis positions that survive a power cycle.
 *
 * WHY THIS IS A SAFETY FEATURE, not a convenience:
 *
 * The build and supply pistons have a limit switch at the BOTTOM ONLY. Nothing
 * mechanical stops them at the top — driven up far enough they push the weight
 * out of the cylinder and break things. The software travel limit is the only
 * protection at that end, and a software limit needs a zero to measure from.
 *
 * Without persistence the machine has no zero after a reboot, so the limit
 * cannot be applied, so the top of travel is unprotected. That is exactly the
 * hole the old firmware's permanently-on BED_OVERRIDE / FEED_OVERRIDE left.
 *
 * With persistence the workflow the machine owner wants and the safe behaviour
 * are the same thing: home the pistons ONCE, and the position comes back on
 * every boot after that, with the upper bound enforced the whole time.
 *
 * The recoater is deliberately NOT persisted. It is the only open-loop axis,
 * it drifts, it moves several times per layer, and it has switches at both
 * ends so homing it is quick and safe. It boots with no position and must be
 * homed — which the recoat cycle already requires.
 *
 * A restored position is BELIEVED, not verified: nothing stops an axis being
 * moved by hand while the machine is off. That is why it reports as
 * AXIS_FLAG_POS_RESTORED alongside AXIS_FLAG_HOMED rather than pretending to
 * be a fresh home.
 */

#ifndef MEGA_PERSIST_H
#define MEGA_PERSIST_H

#include <stdint.h>

namespace persist {

/* Read the newest valid record. Call before motion::begin(). */
void begin();

/* Writes one EEPROM byte per call, at most, and only when the device is ready.
 * A byte takes ~3.3 ms to burn, so writing a whole record in one go would
 * stall the stepper service for ~60 ms. Call from loop(). */
void service();

/* True if this axis came back with a position from a previous power cycle. */
bool restored(uint8_t axis);
int32_t restoredPosition_um(uint8_t axis);

/* Tell the store where an axis is. Cheap to call often — a write is only
 * queued once every axis has been still for a moment and something actually
 * changed. */
void note(uint8_t axis, int32_t position_um, bool referenced);

/* This axis no longer has a trustworthy position (it never did, or it was
 * homed and the home failed). */
void forget(uint8_t axis);

/* Diagnostics: how many records have been written this power cycle, and which
 * slot the ring is on. */
uint16_t writes();

}  // namespace persist

#endif /* MEGA_PERSIST_H */
