# FlexIO timer modes and shifter shift-clock routing (Teensy 4.1 / IMXRT1062)

**Status:** empirically established during XY2-100 bring-up, 2026-07-26. Not documented
in the IMXRT1062 reference manual as of the version we consulted.

## TL;DR

On the IMXRT1062 FlexIO peripheral, the timer mode selected by `TIMCTL.TIMOD`
determines whether the timer's internal "shift clock" reaches multiple shifters that
select it via `SHIFTCTL.TIMSEL`. In practice:

| `TIMOD` | Mode name | Multi-shifter friendly? | Notes |
|--------:|-----------|-------------------------|-------|
| 0 | Disabled | n/a | Timer off. |
| **1** | **Dual 8-bit baud** | **Yes** | Use this whenever a timer needs to drive shifters. This is what UART/SPI FlexIO drivers use. Clean 1:1 output-frequency to shift-clock ratio. |
| 2 | Dual 8-bit PWM | **No** | Only Shifter 0 responds. Shifters 1+ receive no shift clock. Also produces a 2× shift-rate multiplier vs. its output pin frequency, which makes rate math confusing. |
| 3 | Single 16-bit counter | **No** | Does not generate a periodic shift clock at all. Designed for trigger-driven counting (e.g. UART start-bit detection). Both shifters go dead. |

**Rule of thumb:** if a FlexIO timer needs to drive any shifter, put it in TIMOD=1
(baud mode). TIMOD=2 is only for pure PWM output pins with no shifter attached.

## How this bit us

For the XY2-100 engine, we needed one FlexIO2 timer to serve two roles simultaneously:

1. Emit a continuous 2 MHz square wave on Teensy pin 6 as the XY2-100 CLK signal.
2. Provide the shift-clock for two data shifters (X on pin 8 and Y on pin 9), so both
   data lines change in lockstep with CLK.

The natural first choice was TIMOD=2 (dual 8-bit PWM), because it produces a clean
50/50 duty output waveform which is exactly what CLK should look like. The topology
was:

```
FlexIO2 Timer 0 (TIMOD=2) --> pin 6 (CLK, visible waveform)
                          --> Shifter 0 --> pin 8 (X data)
                          --> Shifter 1 --> pin 9 (Y data)
```

Observed behavior on the logic analyzer:

- CLK on pin 6: clean 2 MHz square wave, as expected.
- Pin 8 (X, Shifter 0 with pattern 0xAAAAAAAA): 2 MHz square wave. **Wrong rate** —
  0xAAAA alternating shifted at the expected 2 MHz should give a 1 MHz output. The
  2× multiplier comes from TIMOD=2 emitting a shift event on *both* PWM phase
  transitions (end of high phase and end of low phase), giving a 4 MHz effective shift
  rate.
- Pin 9 (Y, Shifter 1 with pattern 0xCCCCCCCC): **stuck LOW**. Shifter 1 was not
  outputting anything.

## Isolation testing

Rather than assume the config was wrong, we systematically ruled out alternative
causes:

**Test 1: pin 9 hardware is fine.** Added a temporary serial command that briefly
disabled FlexIO on pin 9 and configured it as a plain GPIO output. Both HIGH and LOW
drives were confirmed on the analyzer. Pin 9 pad is not damaged and nothing on the
breadboard is holding it low.

**Test 2: not shifter-index specific.** Reassigned Y to Shifter 2 instead of Shifter 1
(kept the same pin 9). Y still didn't work, but `FLEXIO2_SHIFTERR` bit 2 latched to
1 — a buffer underrun. That was a useful new signal: Shifter 2 *was* being clocked
(had to be, for the buffer to underrun) but the ISR-based refill wasn't visibly
sticking. This weakly suggested the issue wasn't purely "shifter is idle."

**Test 3: swap PINSEL between shifters.** Made Shifter 0 drive pin 9 and Shifter 1
drive pin 8. Result:

- Pin 9 now showed the X pattern (Shifter 0's output). ✓
- Pin 8 was stuck LOW. ✗

This was the decisive result. Whichever pin **Shifter 0** was assigned to worked.
Whichever pin **Shifter 1** was assigned to didn't. The problem followed the shifter,
not the pin.

## Root cause

Comparing our TIMOD=2 configuration to the FlexIOSPI implementation shipped with the
Teensyduino core (`FlexIO_t4/src/FlexIOSPI.cpp`), one important structural difference
stood out: **FlexIOSPI uses TIMOD=1 (dual 8-bit baud mode), not TIMOD=2, and it
successfully shares one timer across two shifters** (its TX and RX shifters). If a
shared-timer topology works with TIMOD=1 in a widely used library but fails with
TIMOD=2 in our code, the timer mode itself is the differentiator.

The IMXRT reference manual describes each mode's output-pin behavior clearly but is
vague on the internal shift-clock generation and routing. The empirical observation is:

- TIMOD=1 produces a shift-clock signal that is broadcast to every shifter with
  `TIMSEL = <this timer index>`.
- TIMOD=2 does not — only Shifter 0 receives the shift clock (or possibly only the
  shifter matching the timer's own index, we did not exhaustively test).

We did not attempt to find an NXP document explaining the internal difference; it is
enough to know the empirical rule.

## The fix

Change Timer 0 from TIMOD=2 to TIMOD=1 (dual 8-bit baud mode). The rest of the
configuration (shifter setup, PINSEL, TIMSEL, ISR refill) needed no change.

Before (broken for Shifter 1):

```cpp
// TIMOD=2 (dual 8-bit PWM). CMP = (high << 8) | low.
p.TIMCMP[kTimerClk] = (kCmpHalf << 8) | kCmpHalf;
p.TIMCFG[kTimerClk] = 0;
p.TIMCTL[kTimerClk] = FLEXIO_TIMCTL_PINCFG(3)
                    | FLEXIO_TIMCTL_PINSEL(kFlexClk)
                    | FLEXIO_TIMCTL_TIMOD(2);      // <-- the problem
```

After (both shifters work):

```cpp
// TIMOD=1 (dual 8-bit baud). CMP = (burst << 8) | shift_clock_divider.
// low byte  = FlexIO cycles per output edge - 1 (kCmpHalf = 29 -> 2 MHz output)
// high byte = 2*N - 1 for N shifts per burst (255 = 128 shifts, auto-reloads
//             continuously when TIMCFG.TIMDIS = 0)
p.TIMCMP[kTimerClk] = (255u << 8) | kCmpHalf;
p.TIMCFG[kTimerClk] = 0;
p.TIMCTL[kTimerClk] = FLEXIO_TIMCTL_PINCFG(3)
                    | FLEXIO_TIMCTL_PINSEL(kFlexClk)
                    | FLEXIO_TIMCTL_TIMOD(1);      // <-- fix
```

After the fix:

- CLK on pin 6: clean 2 MHz square wave (unchanged — TIMOD=1's output is also a
  50/50 waveform when the divider is symmetric).
- Pin 8 (X, 0xAAAAAAAA): **1 MHz** square wave. Correct: 0xAAAA alternating pattern
  shifted at 2 MHz = 1 MHz output.
- Pin 9 (Y, 0xCCCCCCCC): **500 kHz** square wave. Correct: 11001100 pattern shifted
  at 2 MHz = 500 kHz output.
- Both data lines transition on falling CLK edges, stable across the rising CLK edge
  where an XY2-100 receiver samples.

## Confusion caveat during debugging

When the TIMOD=1 firmware was first uploaded, the Teensy appeared bricked: LED
stopped blinking, USB serial didn't enumerate, subsequent auto-uploads failed. This
was *not* caused by the TIMOD=1 change; it was a USB re-enumeration hiccup that
resolved itself as soon as the physical PROGRAM MODE button was pressed and the same
firmware was re-flashed. Whenever a Teensy appears bricked after an upload, the first
recovery step should be the physical button — not reverting the code.

## References

- IMXRT1060 Reference Manual, Chapter 50 "FLEXIO" — for TIMCTL, TIMCFG, TIMCMP,
  SHIFTCTL, SHIFTCFG field definitions. Vague on internal shift-clock routing;
  clear on register layout.
- `framework-arduinoteensy/libraries/FlexIO_t4/src/FlexIOSPI.cpp` — reference
  implementation of a two-shifter / one-timer FlexIO peripheral that works correctly.
  Uses TIMOD=1.
- Our commit that introduced the fix: change `FLEXIO_TIMCTL_TIMOD(2)` →
  `FLEXIO_TIMCTL_TIMOD(1)` in `xy2_start_clk_sync()` and update `TIMCMP[kTimerClk]`
  accordingly. See `src/main.cpp`.
