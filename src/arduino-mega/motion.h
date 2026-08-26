/*
 * motion.h — the three steppers, non-blocking.
 *
 * The old firmware ran every move as `while (distanceToGo()) { run(); }`.
 * During a move the board read no sensors, checked no interlocks and could not
 * receive an emergency stop. "MSG_ESTOP is handled first in every handler" is
 * meaningless while the loop is blocked, so the port restructures motion
 * rather than translating it.
 *
 * Every axis is a state machine advanced one step per service() call. Starting
 * a move returns immediately; loop() keeps running, so sensors, interlocks and
 * incoming packets are all still live mid-move.
 *
 * Positions are in micrometres, absolute, and the Mega owns them. The PC used
 * to hold the build and supply positions and send absolute targets, which is
 * why the machine structurally could not run without it.
 */

#ifndef MEGA_MOTION_H
#define MEGA_MOTION_H

#include <stdint.h>

#include "protocol.h"

namespace motion {

/* Number of axes actually fitted. AXIS_BLADE_LIFT is reserved in the protocol
 * but has no hardware, so it is refused rather than silently accepted. */
const uint8_t AXIS_COUNT = 3;

void begin();

/* Advance every axis by one step. Must be called as often as possible — this
 * is what generates step pulses. */
void service();

bool fitted(uint8_t axis);

/* Start a home cycle. Returns false if the axis is unknown or already busy. */
bool home(uint8_t axis);

/* Start a move. speed_um_s / accel_um_s2 of 0 mean "use this axis's default".
 * flags are AXIS_MOVE_* from protocol.h. Returns false if the axis is unknown
 * or busy. */
bool moveTo(uint8_t axis, int32_t target_um, uint32_t speed_um_s,
            uint32_t accel_um_s2, uint8_t flags);

/* Decelerate to a stop and stay where that lands. */
void stop(uint8_t axis);
void stopAll();

/* Freeze every axis instantly, no deceleration ramp. For MSG_ESTOP only. */
void estop();

bool    busy(uint8_t axis);
bool    anyBusy();
bool    homing(uint8_t axis);
int32_t position_um(uint8_t axis);
int32_t target_um(uint8_t axis);
uint8_t statusFlags(uint8_t axis);   /* AXIS_FLAG_* */

/* Travel limit for an axis. The wiper's is measured during homing rather than
 * assumed, so it is not a constant. */
int32_t maxTravel_um(uint8_t axis);

/* An axis that hit a limit switch where none was expected latches a fault.
 * Returns the axis id and clears it, or AXIS_NONE when there is nothing
 * pending, so the caller can turn it into one MSG_FAULT. */
const uint8_t AXIS_NONE = 0xFF;
uint8_t consumeLimitFault();

}  // namespace motion

#endif /* MEGA_MOTION_H */
