#pragma once

#include <stdint.h>

// This board's protocol presence: identity, liveness, state, and an emergency
// stop that actually stops something.
//
// SHARING THE PORT WITH THE CONSOLE
//
// One USB serial connection carries both the human console and the binary
// protocol, and it does so without a mode switch, an escape sequence or a
// second port. That works because a packet always starts with 0xA5, and 0xA5
// cannot come from a terminal: it is outside the 7-bit range a keyboard
// produces, and the console only ever inserts characters in 0x20..0x7E.
//
// So node::poll() owns the Serial reads and hands each byte to whoever it
// belongs to -- the link if a packet is in progress or this byte opens one,
// the console otherwise. No framing knowledge is duplicated here; the link
// itself says whether it is mid-packet.
//
// The reverse direction needs no help. Packets are written in one call, so
// console text can never land inside one, and a host decoder skips anything
// that is not a packet -- which is exactly what it does with the boot banner.
//
// WHAT THIS BOARD WILL AND WILL NOT DO YET
//
// The galvo engine runs, the console drives it, and the laser I/O lines are
// individually controllable from the console. None of that is reachable over
// the protocol yet, and every message that would reach it is REFUSED rather
// than accepted and ignored. On this machine an ACK_OK reads as "it worked",
// and a laser command that silently does nothing is the worst possible lie.
//
// MSG_ESTOP is the exception, and it is not a stub: it drives every laser
// command line low, asserts the laser's own E-stop input, opens the interlock
// relay and freezes the beam where it stands.

namespace node {

// Bring up the link and announce this board. Call after Serial is up.
void begin();

// Read the port and route bytes. Non-blocking. Call from loop().
void poll();

// Periodic work: heartbeat, and status when something has changed.
// Call from loop().
void tick();

// Was an emergency stop latched? Only a reset clears it.
bool estopped();

// Console command: link counters and the last peer seen. This is the place to
// look when packets "sometimes do not arrive" -- a climbing CRC count means a
// cable or noise problem, not a firmware one.
void cmd_status();

}  // namespace node
