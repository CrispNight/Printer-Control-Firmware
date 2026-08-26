#pragma once

// TPS3820 hardware watchdog + SN74LVC1G74 arm latch.
//
// Wiring (per NOTES.md §7):
//   Teensy silk 28 -> TPS3820 WDI (falling edge kicks the timer)
//   Teensy silk 29 -> SN74LVC1G74 CLK (rising edge latches D=1 -> Q)
//   SN74LVC1G74 D    = 3.3 V (always latch 1)
//   SN74LVC1G74 /PRE = 3.3 V (inactive)
//   SN74LVC1G74 /CLR = TPS3820 /RESET (async clear on any reset event)
//   SN74LVC1G74 Q    = FIRMWARE_ALIVE (gates modulation through U24)
//
// TPS3820 spec (per datasheet SLVS165O):
//   Watchdog timeout: 112 ms min, 200 ms typ, 300 ms max
//   WDI pulse width min: 1 µs
//   Not windowed (simple max-only timeout)
//
// Design rules:
//   - Kicker runs on PIT (IntervalTimer) so a stuck loop() can't trip it.
//   - Kick period 50 ms gives >2x margin against the 112 ms worst case.
//   - Arm latch is clocked ONCE at boot, after critical self-checks pass.
//   - Firmware never deasserts FIRMWARE_ALIVE; only hardware reset does.
//   - Software faults (SHIFTERR, DMA_ES) do NOT trip the arm latch --
//     the watchdog scope is hardware/firmware-alive only.

namespace watchdog {

// Configure both pins to safe idle state and start the PIT kicker.
// Call as the very first thing in setup(), before Serial init or any
// slow bring-up work, so the TPS3820 stays happy through the full boot.
void init();

// Pulse the arm latch (rising edge on silk 29) -> Q latches to 1 ->
// FIRMWARE_ALIVE goes high -> modulation gate opens at U24.
// Call ONCE at the end of setup(), only after all critical self-checks pass.
// After this point, Q stays high until a hardware reset event clears the FF.
void arm();

// Was arm() called this boot cycle?
bool is_armed();

// Console command: print kick period, kick count, arm state.
void cmd_status();

// Diagnostic: stop the PIT kicker. TPS3820 will trip within its timeout
// (max 300 ms), which asserts /RESET on the SN74LVC1G74 (FIRMWARE_ALIVE
// drops to LOW -> modulation gate closes at U24). Teensy keeps running --
// the /RESET line is NOT wired to the Teensy on v0.1, only to the D-FF
// /CLR. To recover after starving, press the Teensy PROGRAM/RESET button
// or power-cycle the board. Used to verify the safety chain end-to-end
// with a scope on the FIRMWARE_ALIVE test point.
void cmd_starve();

}  // namespace watchdog
