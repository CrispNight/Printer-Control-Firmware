# FlexIO shifter DMA sources are paired on IMXRT1062

**Status:** empirically identified 2026-07-26 during step-4 (DMA) bring-up of the
XY2-100 engine. Documented directly in the IMXRT1062 reference manual's DMAMUX
source table, but *not* in the FlexIO chapter that describes SHIFTSDEN and shifter
DMA requests, and not in any FlexIO example code we consulted.

## TL;DR

On the IMXRT1062, the four DMA request signals from each FlexIO peripheral are
**paired two-and-two into DMAMUX sources**. Two shifters share one source. If you
route two independent DMA channels to the same source expecting them to service two
shifters independently, they will fight over every request and one will silently
lose. Concretely (from `imxrt.h`):

```c
#define DMAMUX_SOURCE_FLEXIO2_REQUEST0   1     // Shifter 0
#define DMAMUX_SOURCE_FLEXIO2_REQUEST1   1     // Shifter 1 -- SAME SOURCE
#define DMAMUX_SOURCE_FLEXIO2_REQUEST2   65    // Shifter 2
#define DMAMUX_SOURCE_FLEXIO2_REQUEST3   65    // Shifter 3 -- SAME SOURCE
```

The same pattern applies to FlexIO1 and FlexIO3.

**Rule:** if a design needs one DMA channel per shifter, the shifters must straddle
the pair boundary. Two shifters that share a DMAMUX source cannot be independently
serviced by two DMA channels. Valid two-shifter/two-DMA-channel choices on FlexIO2:

- Shifter 0 + Shifter 2
- Shifter 0 + Shifter 3
- Shifter 1 + Shifter 2
- Shifter 1 + Shifter 3

Invalid (both trigger from the same DMAMUX source):

- Shifter 0 + Shifter 1
- Shifter 2 + Shifter 3

## How this bit us

Step 4 of the XY2-100 bring-up needed two shifters (X on pin 8, Y on pin 9) each
being refilled by its own eDMA channel. The natural choice — reusing the same
shifter indices from step 3 — was Shifter 0 for X and Shifter 1 for Y. Both DMA
channels were set up with `.triggerAtHardwareEvent()`: X on
`DMAMUX_SOURCE_FLEXIO2_REQUEST0` (value 1), Y on `DMAMUX_SOURCE_FLEXIO2_REQUEST1`
(also value 1 — same source).

Observed:

- `FLEXIO2_SHIFTSTAT = 0x1` — Shifter 0's buffer needed refill, but the refill wasn't
  happening.
- `FLEXIO2_SHIFTERR = 0x1` — Shifter 0 latched an underrun.
- Pin 8 (X): stuck LOW. Shifter 0 was shifting (had to be, to underrun) but its
  shift register drained to zero because SHIFTBUF was never refilled.
- Pin 9 (Y): irregular pattern, not aligned to the SYNC frame boundary. Shifter 1
  was receiving *some* refills — enough to output data, but at the wrong cadence.

The pattern is consistent with two DMA channels competing for the same request
signal. Each time source 1 asserted, the arbiter picked one channel to service; the
other channel's request was lost. The channel that lost most often (Shifter 0's
refill channel) stalled its shifter completely.

The DMA channel state itself looked healthy: `DMA_ERQ = 0xC` (both channels
enabled), `DMA_ES = 0x0` (no error status), `SHIFTSDEN = 0x3` (both shifter DMA
requests enabled). The failure was invisible to any single register — you have to
look at DMAMUX sources for both channels *simultaneously* to see the collision.

## Why this isn't obvious from the FlexIO reference

The IMXRT reference manual's FlexIO chapter describes SHIFTSDEN and DMA requests in
per-shifter terms. Every diagram and register description treats each shifter as
having its own DMA request. The pairing only shows up in the DMAMUX chapter's
source table, which lists all ~130 DMA request sources across the whole SoC — a
place you'd typically consult only when picking a channel for an already-known
peripheral, not when planning a peripheral's DMA topology. The header file
(`imxrt.h`) makes the collision visible if you look, but the constant names
(`FLEXIO2_REQUEST0`, `REQUEST1`, etc.) suggest four distinct sources when there
are really two.

## The fix

Move Y from Shifter 1 to Shifter 2 (`kShifterY = 2`), and use
`DMAMUX_SOURCE_FLEXIO2_REQUEST2` for Y's DMA channel:

```c
constexpr uint8_t kShifterX = 0;
constexpr uint8_t kShifterY = 2;   // was 1; paired with shifter 0 on same source

// ... elsewhere ...

xy2_setup_dma_channel(g_dma_x, g_pattern_x, kShifterX,
                      DMAMUX_SOURCE_FLEXIO2_REQUEST0);
xy2_setup_dma_channel(g_dma_y, g_pattern_y, kShifterY,
                      DMAMUX_SOURCE_FLEXIO2_REQUEST2);
```

Shifter 2 still lives on the same FlexIO2 module and can use the same TIMSEL
(Timer 0) as Shifter 0 — the shift-clock broadcast we verified in step 3 with
TIMOD=1 reaches all shifters that select the timer, regardless of index. PINSEL is
still 11 (D11 = pin 9) — the physical wiring doesn't change.

No changes needed to pin muxes, FlexIO clock, timers, or SHIFTBUF preload code.

## Related caveats

- The same pairing exists for FlexIO shifter *interrupt* enables (`SHIFTSIEN`) — one
  IRQ handler dispatches for all shifters on the same FlexIO module. That's fine
  because our step-3 ISR simply checks `SHIFTSTAT` and refills whichever shifter has
  its bit set. But if you designed around per-shifter IRQ latency, the shared IRQ
  vector would surprise you.
- The pairing is a *DMAMUX* thing, not a FlexIO-internal thing. The shifter STAT
  signals themselves are per-shifter and distinct at the FlexIO peripheral. Only the
  external DMA request lines are paired. This means an interrupt-based refill (step
  3) works fine for shifters 0+1, but a DMA-based refill (step 4+) requires
  straddling the pair boundary.

## References

- IMXRT1060 Reference Manual, DMAMUX source table (in the DMAMUX chapter, not the
  FlexIO chapter). Enumerates all DMA request sources by number.
- `framework-arduinoteensy/cores/teensy4/imxrt.h` — search for
  `DMAMUX_SOURCE_FLEXIO2_REQUEST` to see the numeric collisions.
- Our commit that introduced the fix: change `kShifterY = 1` → `kShifterY = 2` and
  `DMAMUX_SOURCE_FLEXIO2_REQUEST1` → `DMAMUX_SOURCE_FLEXIO2_REQUEST2` in
  `xy2_start_clk_sync()`. See `src/main.cpp`.
