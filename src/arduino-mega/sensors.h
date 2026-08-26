/*
 * sensors.h — oxygen and thermistor channels.
 *
 * The Mega does two things with each reading: it publishes it in
 * MSG_SENSOR_REPORT, and it asserts one digital line per sensor into the
 * hardware interlock chain. It is a contributor to that chain, not its master
 * — safety.h reads the chain's verdict back on separate input pins.
 *
 * Sampling is round-robin, one channel per tick, so a sweep of eight ADC
 * channels never stalls the stepper service for more than a single conversion
 * pair. The old firmware read all eight every loop iteration, which is roughly
 * 1.5 ms of dead time between step pulses.
 *
 * Overrides are compile-time (see config.h) so they cannot be set by accident,
 * but they are never silent: every substituted channel is published in
 * MSG_SENSOR_OVERRIDE with the true reading beside it.
 */

#ifndef MEGA_SENSORS_H
#define MEGA_SENSORS_H

#include <stdint.h>

#include "protocol.h"

namespace sensors {

void begin();

/* Sample at most one channel. Call from loop(); paced internally by
 * SENSOR_SAMPLE_INTERVAL_MS. */
void service();

void fill(sensor_report_t &out);

/* True when any channel is being substituted, i.e. when fillOverride() has
 * something worth sending. */
bool overrideActive();
void fillOverride(sensor_override_t &out);

/* Set when the override picture changes (which, being compile-time, means
 * once at boot) so main.cpp can publish it on change rather than on every
 * periodic report. */
bool consumeOverrideChanged();

/* Highest of the oxygen channels, which is the one that matters for purging. */
uint16_t oxygenWorstPpm();

/* Bit per PIN_SAFETY index for a channel that is within
 * TEMP_WARN_OFFSET_C_X10 of its limit but has not tripped yet. */
uint16_t warnMask();

/* A channel that is supposed to have a sensor on it has been reading
 * implausibly for long enough to count as broken. Consume-once; returns the
 * sensor_report_t.valid_mask bit index (0-1 oxygen, 8-13 temperature) or
 * SENSOR_NONE. The unused thermistor channels can never appear here: they
 * float or sit on a pull-down and read implausibly by design. */
const uint8_t SENSOR_NONE = 0xFF;
uint8_t consumeInvalidSensor();

}  // namespace sensors

#endif /* MEGA_SENSORS_H */
