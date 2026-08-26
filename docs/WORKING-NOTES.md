# Working notes — open design questions

**Status: scratchpad, not a specification.** Nothing here is decided. This
records what we worked through on 2026-08-26 while setting up the repo and
porting the galvo firmware, so the reasoning isn't lost between sessions.

Expect this file to be deleted or broken up once the questions in it are
settled — decisions belong in `protocol/PROTOCOL.md`, the firmware docs, or
`NOTES.md` in the Galvo-Control-Board repo. Nothing should come to depend on
this document.

Reference numbers used throughout, from `FIRMWARE_OVERVIEW.md` and the board
notes:

| Quantity | Value |
|---|---|
| Field scale | 374.5 counts/mm → 1 count = 2.67 µm |
| Position limit | ±30 000 counts ≈ ±80 mm |
| XY2-100 frame | 20 bits |
| Bit clock, boot default | 500 kHz → **25 kHz** frame rate (40 µs/frame) |
| Bit clock, design target | 2 MHz → **100 kHz** frame rate (10 µs/frame) |
| DMA buffers | 2 × 8192 frames, 64 KB each |
| MCP4921 write | 16 bits @ 5 MHz ≈ 3.2 µs |
| GP8211S write | I²C 400 kHz, multi-byte ≈ 25 µs |

---

## 1. "Frame" is overloaded — rename the protocol one

Two unrelated things currently share the word, thousands of times apart in rate:

- **XY2-100 frame** — one position word to the galvo, every 10–40 µs.
- **Protocol frame** — one message packet between boards or to the PC.

**Intended change:** rename the protocol-level one to **packet** throughout
`PROTOCOL.md`. Not yet done. The C identifiers (`FRAME_SOF0`, `MoirenFrame`)
would follow, which is a `PROTOCOL_VERSION` bump — cheap now, annoying later.

## 2. Per-vector power — group by parameters, don't tag every point

The slicer exports a DXF stack; each vector carries its own speed and power in
extended DXF data. Most vectors share values, and a curved contour holds
constant power across the whole series.

An earlier suggestion to add a `power_pm` field to `vector_point_t` was wrong.
It would spend 2 bytes on every point to carry a rarely-changing value, and
drop the packet payload from 20 points to 16.

**Better, and already supported by the protocol as written:** sort each layer's
vectors by their (speed, power) pair, emit one `MSG_LASER_PARAMS` per group,
then the points for that group. A constant-power contour costs one parameter
message regardless of vector count. No protocol change needed.

The only case that would force per-point power is ramping power *within* a
single vector. Not currently required — confirm before designing for it.

## 3. Buffer depth and power timing are the same question

Total buffered time **is** the latency between deciding something and it
reaching the galvo. Section count determines how much of that total is
available as refill-ISR slack, and how finely anything can be scheduled.

At 8192 frames/buffer the lookahead is **82 ms** at the 100 kHz target
(328 ms at the current 500 kHz boot clock). A DAC write issued during refill
lands that far ahead of the position it belongs to. Bandwidth was never the
constraint here; alignment is.

If power is applied at section boundaries rather than by DMA, section size sets
the achievable alignment:

| Alignment @ 1000 mm/s | Time | Section size @ 100 kHz | Refill ISR rate |
|---|---|---|---|
| ±1 mm | 1 ms | 100 frames | 1 kHz |
| ±0.1 mm | 100 µs | 10 frames | 10 kHz |
| sample-exact | 10 µs | 1 frame | needs DMA |

**The open question is which of these the parts actually need.** That single
measurement resolves the section count, the DMA-DAC question and the DAC choice
together — they are one decision seen from three sides.

Note the ping-pong scheme was built for *continuous pattern generation*
(circles, squares), where lookahead costs nothing. The deep buffer is not a
mistake; it is a good fit for the problem it was written for and a poor fit for
vector marking with per-vector power.

### Two vs more sections

For the same total latency, more sections give more ISR slack: with ping-pong,
one half must be refilled while the other drains, so slack is half the total.
With N sections, N−1 remain queued.

Equivalently: for the same slack, more sections allow lower total latency.

**Deliberately unchanged during the port.** That DMA path carries real hard-won
fixes — the single-DMA/scatter-gather race fix, the D-cache flush for coherent
reads, and the drift monitor. Changing the buffering during a port would mean
changing two things at once. Make section count and size named constants first,
then change buffering as its own step with the monitor watching.

## 4. Fractional position accumulation

At 1000 mm/s and the 100 kHz target the galvo advances **3.75 counts per
frame** — a fraction, not an integer.

Below the crossover the same coordinate repeats across consecutive frames,
which is expected and correct:

| Frame rate | Repeats below |
|---|---|
| 25 kHz (current boot default) | ~67 mm/s |
| 100 kHz (design target) | ~267 mm/s |

The interpolator must carry sub-count precision and round only at output.
Stepping in whole counts gives stair-stepping at speed and stalling when slow.
Easy to get wrong, cheap to get right first time.

## 5. Delay compensation — one signed number, not several

There are unknown latencies on both paths: Teensy → analog voltage at the laser
power input, and Teensy → mirror actually moving. Neither is well characterised.

**Only the difference between the two matters.** Equal delay on both paths
shifts the whole picture uniformly in time and is invisible in the part. What
shows up is the mismatch — power arriving early or late relative to where the
mirror is.

So the calibration is a single signed value ("emit power N samples ahead of or
behind position"), measured as a pair on the bench.

This is distinct from `on_delay_us` / `off_delay_us` / `poly_delay_us` in
`laser_params_t`, which are *commanded* dwells deliberately inserted to let the
mirror settle. The offset here is a *systematic hardware* correction, set once
per machine, not per job.

**Intended change:** add `MSG_GALVO_CAL` carrying this offset, plumbed but
defaulting to zero. Needs a `PROTOCOL_VERSION` bump.

If power ends up stored as a parallel array alongside position, applying the
offset is an array index shift — essentially free.

## 6. DMA-driven DAC — possible, unverified, not the first move

`dac.cpp` today does a blocking `SPI1.transfer16()` from `loop()`, with CS
toggled by `digitalWriteFast`. No DMA anywhere.

Sample-exact power alignment would need the DAC fed by DMA in lockstep with the
position stream.

### The DMA side is available

The RT1062 has 32 eDMA channels; the galvo engine uses one. Channel count is not
a constraint.

Trigger the power channel by **eDMA channel linking** — the position channel's
minor-loop completion hardware-triggers the power channel, giving one power word
per galvo frame with no drift. Do *not* point two DMAMUX channels at the same
FlexIO request: the shifter request is cleared when serviced, so the channels
would race and silently drop transfers.

*To verify in the reference manual:* with `ELINK` set, `CITER`/`BITER` narrow
from 15 bits to 9 (the link channel number takes the space), capping the major
loop at **511** iterations instead of 32767. The major loop is 8192 today, so
linking would force sections ≤511 frames — 5.1 ms lookahead at 100 kHz. The
constraint and §3's goal point the same way.

Two channels can share one buffer (interleaved `[X, Y, POWER]` records, or
parallel arrays). One channel cannot feed both peripherals: FlexIO2 and LPSPI3
are far apart in the address map and `DMOD` only wraps inside a small
power-of-two window.

Apply the §5 lead/lag offset by writing power values **pre-shifted** during
refill, rather than by staggering DMA start times. Exact, runtime-adjustable,
no DMA complexity.

### The blocker is the CS pin — VERIFIED

The MCP4921 latches on CS rising edge, so every word needs its own pulse. LPSPI
does that natively: set `TCR` once with `CONT=0` and each `TDR` write produces a
framed transfer with PCS pulsing. **But only on a hardware PCS pin.**

Teensy 4.1 hardware CS pins, from `SPI.cpp` in the Teensy core
(`libraries/SPI/SPI.cpp`, the `ARDUINO_TEENSY41` hardware structs):

| Port | Peripheral | Hardware CS pins |
|---|---|---|
| `SPI` | LPSPI4 | 10, 37, 36 |
| **`SPI1`** | **LPSPI3** | **0, 38** |
| `SPI2` | LPSPI1 | 44 |

The DAC sits on SPI1 (MOSI1 = 26, SCK1 = 27) with **CS on pin 32, which is not
a PCS pin for LPSPI3** — hence the `digitalWriteFast` in `dac.cpp`. Moving to
another LPSPI is not possible either, since 26/27 are LPSPI3-only.

**This is a board routing question, not a chip choice.** The MCP4921 is fine.

Of the two candidates, `pins.h` shows **pin 38 is taken**
(`kPinLmInterlockN`) and **pin 0 is free**. A bodge from 32 to 0 on v0.1 would
unlock hardware PCS and therefore DMA; v0.2 could route it properly.

Weigh before claiming pin 0: it is **Serial1 RX**, and Serial1 is the obvious
candidate for the inter-board link once the Teensy becomes master. Serial2
(pins 7/8) is free and unused on this board, so there is an out — but it should
be a deliberate choice.

### Cheaper first step

Shrink the buffers and apply power at refill boundaries. No new DMA plumbing, no
LPSPI register work, no bodge wire. Escalate to DMA only if the bench shows it
isn't tight enough.

## 7. Reconciling the 100 kHz figure in board NOTES.md §8b

§8b argues for SPI over I²C on the basis that per-vector power updates could
approach 100 kHz.

That figure is best read as a **ceiling**, and as a ceiling it is correct —
power can never need to change faster than positions update, so the galvo frame
rate bounds it.

Realistic rates are far below it. A 0.5 mm vector at 1500 mm/s lasts 333 µs,
about **3000 vectors/s**. Reaching 100 kHz would need 15 µm vectors, which is
not geometry a slicer emits. Even I²C's ~25 µs write is ~8% duty at 3000/s.

So the bandwidth argument alone does not decide it. Resolution favours the
15-bit GP8211S over the 12-bit MCP4921; DMA-ability (§6) favours the MCP4921.
**Worth adding to §8b so the comparison isn't made on throughput alone.**

## 8. Where layer data lives

A ~20 000-vector layer is roughly **180 KB**. The build currently leaves
~380 KB free in RAM2, so a whole layer fits alongside the 128 KB of DMA
buffers.

The print cycle already provides the window: recoat, motor moves, camera
capture — seconds of dwell, during which pushing 180 KB over USB takes a
fraction of a second.

**Proposed:** load the entire layer into Teensy RAM during the recoat dwell,
then mark it with the PC doing nothing. No streaming while the laser is on, no
timing dependency on the host.

This makes `MSG_MARK_BATCH` a **load** operation, not a **play** one. That
distinction should be explicit in `PROTOCOL.md`; it currently is not.

The microSD slot becomes the answer later, for running with no PC at all —
same layer format, different source, same code path downstream.

## 9. Carried-over items from the port

Recorded in the port commit, repeated here so they aren't lost:

- `.clangd` and `.vscode/settings.json` hardcode `C:/Users/bradf/...`, the
  author's other machine. Will need a per-machine path.
- `platformio.ini` sets `upload_protocol = teensy-cli`; the source repo used
  the platform default.
- `docs/teensy-galvo/FIRMWARE_OVERVIEW.md` is stale on the DAC — it describes a
  GP8403 at I²C `0x5F`. The board now carries a GP8211S (I²C, addr `0x58`) and
  an MCP4921 (SPI), selected by the **JP1 solder jumper**. `config.h` already
  has a `DAC_USE_SPI` compile-time flag for this, and it must match JP1's
  physical position. A boot banner reporting which DAC the build expects would
  turn a silent dead output into an obvious message.
