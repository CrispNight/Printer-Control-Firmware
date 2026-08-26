# SN75174N validation on XY2-100 breadboard, and why we're replacing it

**Status:** validated 2026-07-26 for bring-up use, disqualified for production. Full
scope measurements below. Replacement parts ordered (uA9638, AM26C31).

## Context

The Lachesis galvo protoboard uses an SN75174N quad RS-422 differential line driver
to convert four 3.3 V single-ended Teensy 4.1 FlexIO outputs (CLK, SYNC, X, Y) into
four differential pairs suitable for the RC1001 galvo head. During bring-up we saw
what looked like severe ringing, amplitude collapse, and duty asymmetry on the
'174 outputs. Scope work sorted the real problems from the false alarms.

## What was actually wrong

**Diagnosis:** the SN75174N is rated for 4 megabaud maximum output. That is exactly
one 2 MHz square wave — which is what XY2-100 requires as its CLK. **We are running
the part at 100 % of its rated speed, with zero margin.** Datasheet transition
time tt(OD) is 80 ns typ / 120 ns max.

Measured with a Rigol DS1054Z on the '174 output (isolated single driver, 100 Ω
differential termination across output pair, MATH CH2−CH3 for differential):

| Measurement | Value | Spec / expected |
|---|---|---|
| Differential VOD into 100 Ω | 3.1 V | 2 V min (pass) |
| Rise time (10 % → 90 %) | 78–83 ns | ~80 ns typical |
| Fall time (10 % → 90 %) | 85–89 ns | ~80 ns typical |
| Total transition width | ~105 ns | equal to ~42 % of a 250 ns bit period |
| Vrms of the differential signal | 2.64 V | independent cross-check of the ~42 % transition duty |

So the driver IS producing valid differential levels, but its transitions eat almost
half of every bit period. Any receiver looking for a stable data window in the middle
of a bit gets only ~145 ns of settled signal per bit — enough for the RC1001 to
decode reliably, but with no margin for temperature, PVT variation, cable length,
or the second-order effects that appear in real usage.

## Decoupling matters

Before adding a decoupling capacitor, both single-ended outputs showed heavy ringing
that made the differential waveform nearly triangular with plateau ripple. Adding a
0.1 µF ceramic between the '174's VCC pin (16) and its GND pin (8), with the
shortest possible return path routed through the rail above the chip and a direct
wire back to pin 8, cleaned up the rising edge substantially. What overshoot
remained is flying-lead inductance from breadboard construction. That's a layout
constraint, not a chip fault, and it resolves on the PCB spin.

**Takeaway:** every 5 V logic chip on this design needs local decoupling. Not
optional. Add it during the schematic phase, not as a rework.

## False alarms resolved along the way

Two things looked like real signal-integrity problems but turned out to be
measurement artifacts. Documented here so we don't waste time chasing them again:

- **500 mV output reading**: probe was set to 10× attenuation while the scope input
  was configured for 1×. Fix: match probe and scope attenuation settings before
  trusting amplitude measurements.
- **Contradictory pulse-width / duty-cycle readings** (e.g. 183 ns / 316 ns vs
  227 ns / 277 ns depending on which measurement was enabled): the scope's
  automatic Vtop / Vbase estimator was being corrupted by 13–15 % preshoot on the
  edges. The estimator assumed the preshoot excursion was the "top" of the signal,
  which shifted the ±10 %/90 % thresholds and gave junk width readings. Fix: set
  Vtop / Vbase manually on the scope when there's significant preshoot.

## The RC1001 termination question

The RC1001 galvo head's XY2-100 differential inputs are **not internally terminated**.
Multimeter across each pair reads ~2.1 kΩ same-pair, ~1.7 kΩ cross-pair, unchanged
on reversed leads. That's a **failsafe bias network** — roughly 1 kΩ pull-up / 1 kΩ
pull-down — that holds each pair in a defined idle state when the driver isn't
present. It is not the 120 Ω differential termination that RS-422 nominally expects.

Consequence: **termination has to live at the driver end**, not the receiver end.
That's the reverse of textbook RS-422 practice. We added 100 Ω across each pair at
the '174 outputs; this needs to persist into any PCB revision.

## Why SN75174N is disqualified for production

The disqualifier is not the marginal transition time (though that alone would be
enough reason to want a faster part). It's this:

> **The SN75174N datasheet specifies NO channel-to-channel skew (t_sk_(o) or
> t_sk_(p)) at all.**

XY2-100 is a synchronous parallel bus. The receiver samples X, Y, and SYNC on the
same clock edge as CLK. If CLK's rising edge arrives at the receiver even a few
nanoseconds ahead of or behind the corresponding X/Y/SYNC transitions, the receiver
may sample a data bit that hasn't stabilized yet. Unspecified channel-to-channel
skew is a real hazard on a bus like this. It might work fine on any given chip and
fail on the next one from a different fab run.

## Replacements ordered

Two candidates on the way:

| Part | Rise/fall | Channel skew | Packages needed |
|---|---|---|---|
| **uA9638** | 10 / 20 ns | t_sk(o) = 1 ns | 2 packages for 4 diff pairs |
| **AM26C31** | 7 ns | t_sk(p) = 0.5 ns | 1 package for all 4 |

Either gives us ~4×–10× the transition-time margin plus a specified skew number.
AM26C31 wins on packaging (one chip does the whole job) and skew (0.5 ns vs 1 ns).
uA9638 is a common alternative already validated in similar galvo controller
designs; keeping both on hand for redundancy during PCB layout.

## Diagnostic worth remembering during position testing

If the galvo shows intermittent bit errors during motion testing on the current
SN75174N build, and those errors look like frame-encoding bugs — try running CLK at
1 MHz instead of 2 MHz. If the errors disappear at half rate, they are being caused
by the driver's marginal transition time and not by anything in the firmware. That's
worth 30 seconds of firmware change to save hours of chasing an encoding "bug"
that isn't real.

## Current instrumentation setup

Logic analyzer channels during ongoing bring-up:

| Ch | Signal |
|----|--------|
| 0 | CLK− (differential negative) |
| 1 | CLK+ |
| 2 | SYNC− |
| 3 | SYNC+ |
| 4 | X− |
| 5 | X+ |
| 6 | CLK input to '174 (Teensy pin 6, monitoring) |
| 7 | SYNC input to '174 (Teensy pin 7, monitoring) |

Y differential pair not currently probed — swap to it as needed by moving the CLK
input / SYNC input probes.

All four drivers in use, all inputs live, both '174 enable pins high, all four
pairs wired through the DB15 breakout. Galvo not yet connected.
