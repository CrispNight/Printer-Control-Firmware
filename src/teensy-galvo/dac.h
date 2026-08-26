#pragma once

// 0-10 V DAC driving the laser's analog power input.
//
// v0.1 PCB has TWO DAC chips populated in parallel; JP1 selects which drives
// the output line. Compile-time flag DAC_USE_SPI in config.h chooses which
// driver is built:
//
//   DAC_USE_SPI = 1 (default): MCP4921 12-bit SPI DAC on SPI1
//                              + REF3025 2.5 V ref + OPA192 4x gain
//   DAC_USE_SPI = 0          : GP8211S 15-bit I2C DAC (address 0x58 default)
//
// The compile-time flag MUST match the hardware jumper position. If it
// doesn't, DAC commands go to a chip that isn't wired to the output.
//
// Only one channel exists on either variant; the protoboard's channel
// argument is gone.

namespace dac {

// Init the active DAC path. Sets output to 0 V immediately for boot-safe.
// For I2C variant: probes address and reports NO ACK on failure. For SPI
// variant: no probe -- SPI has no ACK -- but the first write should force
// the output rail to 0 V, which the ADS1115 (Pass 4) will let us verify.
void init();

// Non-blocking state-machine tick called from loop(). Advances step/sweep.
void tick();

// Direct set. Any running sequence is aborted.
void set_volts(float volts);

// Console command entry points.
void cmd_dac      (const char* args);  // "dac <v>"
void cmd_dacstep  (const char* args);  // "dacstep <n>"
void cmd_dacsweep (const char* args);  // "dacsweep [sec]"
void cmd_dacoff   ();
void cmd_dacstat  ();
void cmd_dacscan  ();                   // I2C-only; no-op with a note on SPI

}  // namespace dac
