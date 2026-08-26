# Teensy 4.1 DMAMEM variables are NOT initialised at boot

**Status:** empirically confirmed 2026-07-26 during step-4 (DMA) bring-up of the
XY2-100 engine. Directly contradicts a natural reading of both C++ semantics and
much of the Teensy documentation.

## TL;DR

On Teensy 4.1, variables declared with the `DMAMEM` attribute end up in the OCRAM
(RAM2) `.dmabuffers` section. **The startup code does not copy their initialisers
from flash to RAM.** At runtime they contain whatever happened to be in that OCRAM
location at power-up — typically zero, but not guaranteed. Any C++ initialiser
written on a `DMAMEM` variable is silently ignored.

```c++
// BROKEN on Teensy 4.1: g_pattern is NOT 0xAAAAAAAA at runtime.
DMAMEM uint32_t g_pattern = 0xAAAAAAAA;
```

```c++
// WORKS: default RAM1 (DTCM) is initialised from flash at boot.
uint32_t g_pattern = 0xAAAAAAAA;
```

If you need a variable that is (a) initialised and (b) DMA-accessible, just don't
use `DMAMEM`. The default RAM1 / DTCM is both. If you need OCRAM specifically (for
large buffers or for coherency reasons with a cached region), assign the value
explicitly in `setup()` or a runtime initialiser — don't rely on the C++ initialiser.

## How this bit us

Step 4 of the XY2-100 bring-up used two eDMA channels, each re-transmitting a
fixed 32-bit pattern from RAM to a FlexIO2 SHIFTBUF register. The patterns were
declared:

```c++
DMAMEM __attribute__((aligned(32))) uint32_t g_pattern_x = 0xAAAAAAAA;
DMAMEM __attribute__((aligned(32))) uint32_t g_pattern_y = 0xCCCCCCCC;
```

At runtime the actual values in those locations were **not** 0xAAAAAAAA and
0xCCCCCCCC — they were whatever OCRAM contained at power-up. On our board:

- `g_pattern_x` came up as `0x00000000`. DMA faithfully copied zeros into
  SHIFTBUF[0] on every request. Shifter 0 shifted out all zeros. Pin 8 stayed LOW.
- `g_pattern_y` came up as some non-zero garbage value. DMA copied that same
  garbage into SHIFTBUF[2] on every request. Shifter 2 shifted it out as a
  repeating 32-bit pattern with a 16 µs period (= 32 bits / 2 MHz shift rate).
  Because the garbage wasn't the intended 0xCCCCCCCC (`11001100`) pattern, it
  looked "nonuniform" on the analyzer rather than a clean 500 kHz square wave.

Every register looked healthy the whole time: `SHIFTERR = 0` (no underruns),
`SHIFTSTAT = 0` (both buffers full), `DMA_ERQ = 0xC` (both channels enabled),
`DMA_ES = 0` (no DMA errors), `SHIFTSDEN = 0x5` (correct shifter mask). The DMA
peripheral was doing exactly what it was told; the problem was that it was being
told to transfer the wrong source data.

The clue that gave it away was the 16 µs period on Y. That's exactly one full
32-bit shifter cycle at 2 MHz shift rate — which meant the DMA WAS refilling
SHIFTBUF and the shifter WAS shifting out the buffer contents at the expected
rate. The refill mechanism was fine. Only the value being refilled was wrong.

## Why it's easy to miss

Nothing in the C++ code hints that the initialiser is being discarded — no compiler
warning, no linker warning, no diagnostic at boot. `sizeof()`, `offsetof()`, and
static-analysis tools all treat the variable exactly like a normal `.data` variable.
Debuggers show the "correct" initialiser value in source view. Only reading the
actual RAM location at runtime reveals the discrepancy.

Teensyduino documentation describes `DMAMEM` primarily in terms of "puts variables
in RAM2 (OCRAM) instead of RAM1 (DTCM)" and emphasises that OCRAM is bigger and
handled differently by DMA. The consequence for initialisation is easy to overlook.

## The Teensy memory-region cheat sheet

For future reference, when picking where a variable should live on Teensy 4.1
(IMXRT1062):

| Region | Attribute | Init from flash? | Cached? | DMA-friendly? | Notes |
|--------|-----------|------------------|---------|---------------|-------|
| RAM1 / DTCM | (default) | **Yes** | No (tightly-coupled) | Yes, slightly slower for DMA than OCRAM | Best default for small variables and initialised data. |
| RAM2 / OCRAM `.dmabuffers` | `DMAMEM` | **No** — value is garbage at boot | No (region is uncached in Teensy config) | Yes, fastest for DMA | Use for large DMA buffers where init doesn't matter, or manually initialise in setup(). |
| RAM2 / OCRAM heap | `malloc()`/`new` | n/a | No | Yes | Runtime allocation, contents undefined until you write. |
| PROGMEM (flash) | `PROGMEM` or `const` | Lives in flash | n/a (flash cached) | Yes on Teensy 4.1 (DMA can read flash) | Read-only. Good for lookup tables. |

For our XY2-100 case the fix was trivial: drop `DMAMEM`, put the patterns in the
default RAM1. They're tiny (4 bytes each), they need initialisation, and DTCM's
"slightly slower" DMA speed is completely irrelevant for a 4-byte transfer every
16 µs. `DMAMEM` was just the wrong tool for the job.

## The fix

```c++
// Before:
DMAMEM __attribute__((aligned(32))) uint32_t g_pattern_x = 0xAAAAAAAA;
DMAMEM __attribute__((aligned(32))) uint32_t g_pattern_y = 0xCCCCCCCC;

// After:
__attribute__((aligned(32))) uint32_t g_pattern_x = 0xAAAAAAAA;
__attribute__((aligned(32))) uint32_t g_pattern_y = 0xCCCCCCCC;
```

Also added the current values of `g_pattern_x` and `g_pattern_y` to the `status`
serial command's output, so this failure mode would be caught next time by
inspection rather than by inferring from the analyzer trace.

## References

- Our commit that introduced the fix: drop `DMAMEM` on `g_pattern_x` and
  `g_pattern_y` in `src/main.cpp`.
- Teensyduino source: `framework-arduinoteensy/cores/teensy4/imxrt1062.ld` (linker
  script) — `.dmabuffers` section has no `AT>FLASH` or startup copy.
- Teensyduino documentation on DMAMEM (mentions the region change but not the
  initialisation caveat).
