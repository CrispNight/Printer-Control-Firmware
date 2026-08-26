/*
 * settings.h — the values a settings page owns, kept in non-volatile memory.
 *
 * Every one of these is something the firmware cannot know for itself: it
 * depends on the gas, the regulator, the camera or the powder. Compiled-in
 * defaults are a starting point, not an answer.
 *
 * They are stored on the board rather than held by the host for a specific
 * reason: a host can only display what it last sent. That is wrong after any
 * reset, wrong for a host that was not the one that set them, and wrong the
 * moment a second host exists — and a settings page showing the wrong numbers
 * is worse than one showing none, because it looks right.
 *
 * A zero in the matching field of a command means "use the stored setting", so
 * a one-off run can still override without disturbing what is stored.
 */

#ifndef MEGA_SETTINGS_H
#define MEGA_SETTINGS_H

#include <stdint.h>

#include "protocol.h"

namespace settings {

/* Loads from storage, falling back to the compiled-in defaults if nothing
 * valid is there. Call before the modules that read settings. */
void begin();

const mega_settings_t &get();

/* Validate and store. Returns an ack_status_t; out-of-range values are
 * refused rather than clamped, so a settings page finds out it was wrong
 * instead of silently getting something else. */
uint8_t set(const mega_settings_t &in);

/* Convenience readers, each applying the "0 means use the stored value" rule
 * to a value that arrived in a command. */
uint16_t purgeTarget(uint16_t requested);
uint16_t purgeTimeout(uint16_t requested);
uint16_t purgeMinMix(uint16_t requested);
uint16_t lightSettle(uint8_t mode, uint16_t requested);
uint16_t recoatSettle(uint16_t requested);

/* Millilitres of argon for a given solenoid-open time. An estimate from the
 * regulator setting, not a measurement. */
uint32_t argonMl(uint16_t open_seconds);

}  // namespace settings

#endif /* MEGA_SETTINGS_H */
