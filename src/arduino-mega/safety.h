/*
 * safety.h — observing the hardware interlock chain.
 *
 * PIN_SAFETY is eleven INPUTS. The chain is an independent piece of hardware
 * that decides for itself whether the machine may run; the Mega watches its
 * verdict and reports it. Firmware cannot defeat the interlock, and that
 * property is deliberate — keep these pins inputs.
 *
 * The eleven lines are, in order: 0-1 door, 2-3 oxygen, 4-9 temperature,
 * 10 green check light. HIGH means tripped.
 */

#ifndef MEGA_SAFETY_H
#define MEGA_SAFETY_H

#include <stdint.h>

#include "protocol.h"

namespace safety {

void begin();
void service();

void fill(safety_status_t &out);

/* Bits set for FAULTBIT_DOOR / _OXYGEN / _TEMP currently tripped. */
uint16_t trippedMask();

bool allClear();

/* True once per change of the chain's state, so the status can be published on
 * change instead of only on the periodic tick. */
bool consumeChanged();

/* The chain has no estop line of its own on this board, so the latched
 * software state is what gets reported. */
void setEstop(bool latched);

}  // namespace safety

#endif /* MEGA_SAFETY_H */
