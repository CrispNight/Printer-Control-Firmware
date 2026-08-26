#pragma once

#include <stdint.h>

// Laser I/O primitives. Individual GPIO commands + polled status reads.
// No state machine yet -- that's a later pass once each signal has been
// bench-verified against the BFSC controller.
//
// Six command outputs: MODULATION, CONTROL, ENABLE, RED_LIGHT, FAULT_RESET,
// ESTOP. All active-high. All boot-safe to LOW immediately in setup().
// Five status inputs from the laser + five monitor read-backs from the
// laser side of our own commands (verify what the laser actually sees).
// One E-stop chain input. One interlock SSR output.
//
// Console interface:
//   laser                  -- print all state
//   laser status           -- same as 'laser'
//   laser mod on|off       -- MODULATION output
//   laser control on|off   -- CONTROL output
//   laser enable on|off    -- ENABLE output
//   laser red on|off       -- RED_LIGHT output
//   laser reset on|off     -- FAULT_RESET output
//   laser estop on|off     -- ESTOP output
//   interlock on|off       -- INTERLOCK_RELAY_EN (SSR to DB-44 pins 1-2)

namespace laser_io {

enum Command : uint8_t {
  CMD_MODULATION = 0,
  CMD_CONTROL,
  CMD_ENABLE,
  CMD_RED_LIGHT,
  CMD_FAULT_RESET,
  CMD_ESTOP,
  CMD_COUNT
};

// Configure all command GPIOs to OUTPUT LOW, interlock SSR pin to OUTPUT LOW,
// all status and monitor inputs to INPUT (opto pullups provide the pull).
// Call as the very first thing in setup(), before anything else could drive
// these pins in an unsafe direction.
void init_safe();

// Set / read individual command outputs.
void set_command(Command c, bool on);
bool get_command(Command c);

// Interlock SSR (silk 34, active-high). HIGH shorts DB-44 pins 1-2.
void set_interlock(bool on);
bool get_interlock();

// Console command entry points.
void cmd_status();               // "laser" / "laser status"
void cmd(const char* args);      // "laser <subcommand>..."
void cmd_interlock(const char* args);

}  // namespace laser_io
