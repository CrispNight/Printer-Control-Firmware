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


## Status index

Read this table first. The detail is below; each heading carries its state.

**Where things stand:** protocol version 2 landed in `3abc782` and everything
decided in this document is expressed in `protocol.h` and `PROTOCOL.md`. The
**Arduino Mega is implemented**; the Teensy speaks no protocol yet and the
ESP32 has no hardware. The remaining work is collected in section 18.

| State | Meaning |
|---|---|
| `[DONE]` | finished - the detail here is history |
| `[FIRMWARE]` | protocol side landed; firmware implementation outstanding |
| `[OPEN]` | needs a decision or a bench measurement |
| `[FACT]` | verified hardware or code truth; nothing to implement |
| `[ELSEWHERE]` | belongs in the board or PC repo, not this one |
| `[SUPERSEDED]` | kept only so the old conclusion does not mislead |

**Companion document:** `docs/UI-REQUIREMENTS.md` collects what a host needs
from each board - settings, indicators, alerts, and the command semantics that
are easy to get wrong. It is filled in as each board lands and becomes the UI
specification once the Teensy is done.

| # | Section | State |
|---|---|---|
| 1 | Frame renamed to data packet | `[DONE]` |
| 2 | Group vectors by laser parameters | `[DONE]` |
| 3 | Buffer depth and power timing are one question | `[OPEN]` + `[FIRMWARE]` |
| 4 | Fractional position accumulation | `[FIRMWARE]` |
| 5 | Delay compensation is one signed number | `[FIRMWARE]` |
| 6 | DMA-driven DAC, and the chip-select pin | `[FACT]` `[ELSEWHERE]` |
| 7 | The 100 kHz figure reconciled | `[FACT]` |
| 8 | Where layer data lives | `[SUPERSEDED]` by section 19 |
| 9 | Carried-over items from the port | `[OPEN]` three small follow-ups |
| 10 | Verified Teensy 4.1 pin tables | `[FACT]` `[ELSEWHERE]` |
| 11 | Two-head expansion, v0.2 pin checklist | `[ELSEWHERE]` |
| 12 | PSRAM is for job data | `[ELSEWHERE]` |
| 13 | CAN as the inter-board transport | `[FIRMWARE]` + `[ELSEWHERE]` |
| 14 | Recoat sequence | `[DONE]` + `[OPEN]` tradeoff |
| 15 | Field correction format and the silent bug | `[FACT]` + `[DONE]` |
| 16 | The .cor files hold no distortion data | `[FACT]` + `[OPEN]` |
| 17 | What the old handler revealed | `[FACT]` + `[DONE]` |
| 18 | **Firmware work queue** | Mega done; Teensy next |
| 19 | Job file on SD, and the print log | `[FIRMWARE]` |

**Lifecycle:** when something is implemented it leaves this file. The decision
lives in `PROTOCOL.md` or the code from then on, and its section collapses to a
one-line pointer. This document should shrink as work lands, not grow.

---

## 1. Frame renamed to data packet  `[DONE]`

Done in commit `d8b41b4`. `packet_t`, `PACKET_SOF0`, `PACKET_MAX_PAYLOAD`;
`MoirenLink` kept as the transport object. Verified mechanical by diffing the
encoded bytes before and after, so `PROTOCOL_VERSION` stayed at 1.

`PROTOCOL.md` is now the reference. Rejected alternatives, for the record:
"USB packet" (transport-specific, and CAN is now in the picture) and "Moiren
packet" (the project name is wanted for higher-level things).

## 2. Per-vector power — group by parameters, don't tag every point  `[DONE]`

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

## 3. Buffer depth and power timing are the same question  `[OPEN]` `[FIRMWARE]`

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

## 4. Fractional position accumulation  `[FIRMWARE]`

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

## 5. Delay compensation — one signed number, not several  `[FIRMWARE]`

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

**Intended change:** add `MSG_TIMING_OFFSET` carrying this offset, plumbed but
defaulting to zero. Needs a `PROTOCOL_VERSION` bump.

Called `MSG_GALVO_CAL` in an earlier draft, which was wrong twice over: it is an
*offset*, not a calibration (you calibrate once and transmit the result), and
"galvo cal" collided with the **field correction** table (section 15), which is
a completely different, spatial thing.

If power ends up stored as a parallel array alongside position, applying the
offset is an array index shift — essentially free.

## 6. DMA-driven DAC, and the chip-select pin  `[FACT]` `[ELSEWHERE]`

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

## 7. Reconciling the 100 kHz figure in board NOTES.md §8b  `[FACT]`

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

## 8. Where layer data lives  `[SUPERSEDED]` — see section 19

> **Superseded.** This section concluded that a whole layer should be loaded
> into Teensy RAM. That holds only for simple parts: an intricate layer can run
> to 200,000 points, which is 1.8 MB and nowhere near fits. The job now lives on
> the microSD card and layers stream from there. See section 19. The reasoning
> below is kept because the arithmetic is still correct for the simple case.

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

## 9. Carried-over items from the port  `[OPEN]`

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

## 10. Verified Teensy 4.1 pin facts  `[FACT]` `[ELSEWHERE]`

Extracted from the Teensy core, scoped to the `ARDUINO_TEENSY41` block of
`cores/teensy4/core_pins.h` (the file holds separate tables for 4.0, 4.1 and
MicroMod — reading across them gives wrong answers).

### FlexIO2 — only 7 pins on this chip

| FlexIO2 pin | Silk | Use |
|---|---|---|
| 00 | 10 | XY2-100 CLK, head 1 |
| 01 | 12 | XY2-100 SYNC, head 1 |
| 02 | 11 | XY2-100 X, head 1 |
| 03 | 13 | XY2-100 Y, head 1 |
| 10 | **6** | free — reserve for head 2 X |
| 11 | **9** | free — reserve for head 2 Y |
| 12 | 32 | DAC CS — frees up if CS moves to pin 38 |

Pins 40 and 41 are **not** FlexIO2; they are `AD_B1_04/05`, which is FlexIO3.

**FlexIO3 has no DMA.** Only `DMAMUX_SOURCE_FLEXIO1_*` and `FLEXIO2_*` exist in
`imxrt.h` — there is no FlexIO3 request line. So despite FlexIO3 having 16 pins,
it cannot stream. FlexIO1 does have DMA if a third instance is ever needed.

### Hardware serial ports

| Port | RX/TX | Status on this board |
|---|---|---|
| Serial1 | 0 / 1 | free |
| Serial2 | 7 / 8 | free |
| Serial3 | 15 / 14 | taken |
| Serial4 | 16 / 17 | taken |
| Serial5 | 21 / 20 | taken |
| Serial6 | 25 / 24 | free |
| Serial7 | 28 / 29 | taken |
| Serial8 | 34 / 35 | taken |

Three free UARTs. Putting the DAC CS on pin 38 rather than pin 0 keeps all three.

### SPI ports — only one usable chip select

| Port | Pins | Status |
|---|---|---|
| `SPI` / LPSPI4 | MOSI 11, MISO 12, SCK 13, CS 10/37/36 | consumed by the galvo |
| `SPI2` / LPSPI1 | CS 44 | inside the 42–47 microSD range |
| `SPI1` / LPSPI3 | MOSI 26, SCK 27, PCS0 → 0 *or* 38 | the only one available |

**Pins 0 and 38 are the same chip select**, not two. Both map to
`IOMUXC_LPSPI3_PCS0_SELECT_INPUT`, the only PCS the core exposes for LPSPI3.
One at a time.

Pins 42–47 are the microSD socket.

## 11. Two-head expansion — what v0.2 should leave open  `[ELSEWHERE]`

Two heads is the natural maximum on this MCU. Beyond that it is a different
board and probably a different microcontroller.

**Galvo:** a second head needs only **2** pins, not 4. Both heads should share
one clock and sync anyway to stay synchronised, so head 2 needs just its own X
and Y. FlexIO2 has 8 shifters and 2 are in use — add shifters 4 and 6 driven by
the same timers. The existing DMA trick extends: widen the `DMOD` wrap from
`SHIFTBUFBIS[0],[2]` to `[0],[2],[4],[6]` and one channel interleaves
X1,Y1,X2,Y2 from a single buffer.

**Laser power:** two sources need two analog outputs, but there is only one
usable hardware chip select (§10). Rather than two MCP4921s, use one
**MCP4922** — the dual-channel sibling, same command format, one CS. Bit 15 of
the word is already the A/B channel select, hardcoded to 0 today because the
4921 has one output. Alternate words in the buffer:

```
[ 0x7A00 ]  head 1
[ 0xFA80 ]  head 2   (bit 15 set)
```

One DMA channel, one buffer, one CS.

Per-head delay compensation (§5) stays independent and free: the offset is
applied when the ISR fills the buffer, so each channel's values shift by their
own amount.

Two caveats:
- **Raise the SPI clock.** Two writes per galvo frame at 5 MHz is 6.4 µs against
  a 10 µs frame — 64% duty. The MCP4922 accepts 20 MHz; 10 MHz gives ~32%.
- **Packages differ.** MCP4921 is 8-pin, MCP4922 is 14-pin — not a drop-in.
  Either pick one for v0.2 or lay both footprints, as was already done for the
  GP8211S/MCP4921 pair with JP1.

### v0.2 pin checklist

1. DAC CS → **pin 38**; move `kPinLmInterlockN` to a plain GPIO (22, 23 or 30).
2. **Reserve FlexIO2 pins 6 and 9** for head 2 X/Y — the entire expansion budget.
3. Do not spend pins 6, 9 or 32 on ordinary GPIO. They are the last three
   FlexIO2 pins, and only FlexIO2 can generate XY2-100.
4. Keep pin 0 free — alternate PCS0 routing, and Serial1 RX.
5. Consider an MCP4922 (or dual) footprint so two heads need no second SPI port.

## 12. PSRAM — for job data, not DMA buffers  `[ELSEWHERE]`

Teensy 4.1 has two underside QSPI footprints, so 8 MB or 16 MB of PSRAM.

**Not for the DMA buffers.** QSPI is far slower than internal RAM and carries
the same D-cache coherency traps the DMA path already had to solve once with
OCRAM.

**Good for layer and job data.** Sequential reads, no hard real-time deadline;
the refill ISR copies from PSRAM into the small internal DMA buffer. At ~180 KB
per layer, 16 MB holds roughly 90 layers uncompressed.

That makes an entire print job resident in RAM with no PC and no SD card in the
marking path — the SD slot becomes a *loading* mechanism rather than a
real-time one, which is a much easier thing to get right.


## 13. CAN as the inter-board transport  `[FIRMWARE]` `[ELSEWHERE]`

UART over 1-2 m in a machine with steppers, a fibre laser and pumps is a poor
bet. The ESP32 in particular has to sit within inches of the airflow sensor
(I2C), so it is a long run through noise. **CAN** is differential, built for
exactly this, and the data rates involved are tiny.

### Bandwidth - verified, huge headroom

| Traffic | Rate | CAN frames/s |
|---|---|---|
| Mega: axis status, 3 axes @ 50 Hz | | ~300 |
| Mega: sensor report @ 10 Hz (18 B -> 3 frames) | | 30 |
| Mega: safety status @ 50 Hz | | 50 |
| ESP32: fan set + status @ 20 Hz | | 40 |
| Heartbeats, commands, acks | | ~80 |
| **Total** | | **~500** |

Classic CAN at 500 kbit/s carries ~3,850 frames/s flat out; under 50% bus load
that is ~1,900. So roughly **a quarter of a conservative budget**, and axis
status at 50 Hz is the dominant term and easily reduced.

It fits because **the vector stream never touches CAN.** That is the ~180 KB per
layer and it goes PC to Teensy over USB. CAN carries only motion commands,
telemetry, safety and fan control. Putting layer data on CAN would be a
five-second-per-layer mistake.

### Classic CAN, 8 data bytes

CAN FD is out: the Teensy's CAN3 supports it, but the ESP32's TWAI controller
and the MCP2515 are CAN 2.0 only. So **8 data bytes per frame**.

Payloads needing segmentation: `SysLog` (49 B -> 7 frames), `SysHello` (26 -> 4),
`SensorReport` (18 -> 3), `AxisMove` (14 -> 2), `RecoatCycle` (12 -> 2),
`StateReport` (12 -> 2). Everything else fits in one frame. At these rates the
overhead is irrelevant.

### Framing is per-transport - a correction to the original design

The 9-byte packet header plus 2-byte CRC would be pure waste on CAN: it would
more than double the traffic and duplicate a CRC the hardware already computes.

**The packet is the logical unit; each transport encodes it natively.**

- **USB serial:** `A5 5A` + header + payload + CRC (what exists today).
- **CAN:** the 29-bit extended ID carries src/dst/msg/flags, the 8 data bytes
  are pure payload, hardware CRC, and long payloads segment across frames.

Shared message *definitions*, per-transport *framing*. That is a better
expression of the transport-agnostic goal than one framing everywhere.

### Hardware

| Node | Controller | Transceiver |
|---|---|---|
| Teensy 4.1 | built-in FlexCAN (CAN1/2/3) | needs 3.3 V - SN65HVD230 or TJA1051T/3 |
| ESP32 | built-in TWAI | needs 3.3 V - same |
| Mega | **none** - needs MCP2515 over SPI | the module's TJA1050 is fine at 5 V |

**Do not wire an MCP2515 module directly to the Teensy or ESP32.** Those modules
run the MCP2515 at 5 V and carry a TJA1050, which needs a 5 V supply and whose
RXD output swings to 5 V. Neither MCU is 5 V tolerant. Use the built-in
controllers with a 3.3 V transceiver instead.

Check the crystal on any MCP2515 module - they ship with 8 MHz or 16 MHz, and
the bit-timing registers differ. Wrong value looks like a dead bus.

## 14. Recoat sequence  `[DONE]` `[OPEN]`

Implemented in `src/arduino-mega/recoat.cpp`. The park-mode tradeoff below is
still unmeasured, which is why it is a per-job parameter.

Confirmed with the machine owner on 2026-08-26. Order matters and the struct
alone does not convey it, so it belongs in `PROTOCOL.md`.

**Overflow park - the default:**

```
1. mark layer
2. build plate drops                    <- this IS the clearance
3. recoater returns overflow -> supply  <- passes over the lowered build, touches nothing
4. supply cylinder rises
5. recoater sweeps supply -> build -> overflow, spreading
6. mark layer
```

The build-plate drop does double duty: it makes room for the new layer *and*
clears the return traverse. Powder is spread only on the forward pass, leaving
one clean surface.

**Supply park costs two extra moves.** Spreading happens on the forward pass,
then the recoater must come back over that fresh even layer, needing an extra
drop-and-raise for clearance.

**The tradeoff is not settled.** Overflow parking is better for surface quality
but forecloses *pre-staging* - keeping a powder pile just ahead of the print
area to cut inter-layer time. The owner estimates pre-staging could save 10-20 s
per layer, which over hundreds or thousands of layers is hours. So park mode
must be a **per-job parameter, not a compile-time choice**, so the tradeoff can
be measured rather than argued.

**Nothing may be over the print bed while marking.** The laser would strike the
recoater - destroying the blade and ruining the print. Any scheme that parks or
traverses over the bed during a mark is a non-starter. (An earlier draft of this
note suggested traversing during marking with the blade raised. That was wrong
and dangerous; it is recorded here so nobody re-proposes it.)

Pre-staging therefore means keeping the powder pile just *outside* the print
area on the near side, shortening the traverse without ever being over the bed.

A future blade-lift axis serves a different purpose: tilt the blade ~1 mm above
the print layer so the return traverse clears the powder **without moving the
build plate at all**. The gain is *positional repeatability*, not overlapped
time - the build plate is the axis that defines layer thickness and is the most
accuracy-critical in the machine, so not cycling it down and back up avoids
backlash and lost position. That is a mechanical change rather than firmware,
but the axis enum should leave room for `AXIS_BLADE_LIFT`.

**Struct changes:** `recoat_cycle_t.flags` becomes an explicit **park mode**
(`PARK_OVERFLOW` / `PARK_SUPPLY` / `PARK_STAGED`), plus a **clearance drop**
value for the supply-park return.

**Ownership:** the Mega executes the entire sequence on one `MSG_RECOAT_CYCLE`.
It owns the axes and limit switches, and the cycle must complete correctly even
if the link hiccups. Micro-managing six moves over CAN would add a failure point
at every step. Parameters travel in the message, so the sequence *shape* stays
selectable without reflashing the Mega - logic with the hardware, policy in the
message.

Commanding the recoater to an arbitrary absolute position already works via
`MSG_AXIS_MOVE` with `AXIS_WIPE`.

## 15. Field correction table - format, and the bug to avoid  `[FACT]` `[FIRMWARE]`

Distinct from the timing offset (section 5). This one is **spatial**: a 65 x 65
grid of position offsets correcting f-theta lens distortion. Recalculated
rarely.

### .cor file format (LMC1COR_1.0)

Decoded from `meerk40t/balormk/controller.py` in the old repo and checked
against `identity.cor` - the arithmetic closes exactly at 68,128 bytes.

| Offset | Size | Contents |
|---|---|---|
| `0x000` | 22 | `LMC1COR_1.0` in UTF-16LE |
| `0x016` | 2 | unknown, skipped by both readers |
| `0x018` | 504 | 63 doubles - **scale at index 43, i.e. offset `0x170`** |
| `0x210` | 67,600 | 65 x 65 points x 2 doubles (dx, dy) |

**Corrected 2026-08-26.** An earlier version of this table said the header was
506 bytes from `0x016` with the scale at index 43 of *that*, which puts it at
`0x176` - six bytes wrong. `get_scale_from_correction_file()` reads the 22-byte
label, then **two unknown bytes**, and only then 63 doubles. Reading at `0x176`
returns 0.0 and makes the neighbouring double come out as a denormal
(`8.087e-320`), which is the tell: a denormal where a scale should be means the
read is misaligned, not that the field is empty. Verified against all four
files on disk; at `0x170` every one reads exactly 1.0.

An older int variant also exists: 14-byte header, 4-byte signed ints.

**Values are 16-bit sign-magnitude, not two's complement.** Doubles are rounded
to int, then `dx if dx >= 0 else -dx + 0x8000` - bit 15 is a sign *flag*. Easy
landmine; get it wrong and the field mirrors itself.

The file is 68 KB because it stores doubles. What actually needs transmitting is
4225 x 2 x 2 bytes = **16.9 KB**, roughly 89 packets at 192-byte payloads.

**Keep the .cor format.** `save_correction_file()` already writes valid files
readable by EZCAD2, so existing calibration files stay usable. Only the
transport needs replacing.

### Why the old upload failed silently

```python
self.write_cor_table(True)
for dx, dy in table:
    self.write_cor_line(dx, dy, 0 if first else 1)   # read=False - fire and forget
status = self.get_list_status()
if status != ERR: return
```

4,225 individual USB commands with **no reply read at all**. No index, no
per-line ack, no checksum. The card just counts them as they arrive.

The single verification is `get_list_status()` at the end, commented as
*"a live read confirms USB didn't drop packets mid-upload"*. **It does not.** It
confirms the card is still *responding*, which is a different thing.

Drop one line out of 4,225 and the card accepts 4,224. Every point after the
drop shifts one grid position. The card answers `get_list_status()` perfectly
happily. The check passes. The field is silently wrong, with no error anywhere.

This cost about a week to find. The `max_retries=3` wrapper retries on the wrong
condition and would not have caught it either.

### Replacement: BEGIN / DATA / END

```
MSG_FIELD_CORRECTION_BEGIN   grid size, table id, CRC of the whole table
MSG_FIELD_CORRECTION_DATA    chunk index + entries      (repeated)
MSG_FIELD_CORRECTION_END     commit
```

The Teensy assembles into a scratch buffer and verifies the whole-table CRC at
END before swapping it in. Any missing chunk, bad CRC or wrong count rejects the
**entire** table and keeps the old one.

Three properties, each killing part of the old failure:

1. per-packet CRC - corruption detected rather than absorbed
2. per-chunk index - a *missing* chunk detected, which is the specific killer
3. atomic commit - never half-applied

Persist to SD or flash on the Teensy so the calibration survives without a PC.

### The header scale factor - resolved

Traced through `meerk40t/balormk/gui/balorconfig.py`:

```python
scale = GalvoController.get_scale_from_correction_file(self.context.corfile)
self.context.lens_size = f"{65536.0 / scale:.03f}mm"
```

So **scale is counts per millimetre**, and `65536 / scale` is the lens field
width in mm. It is re-read whenever the correction file changes.

And it closes the loop on the firmware constant:

```
65536 / 175 mm = 374.49  ->  the firmware's 374.5 counts/mm is a 175 mm lens
```

The +/-30,000 count safety limit is 30000 / 374.5 = 80.1 mm half-field, which
sits inside a 175 mm nominal field. All consistent.

**So the .cor file should drive the field scale, not a hardcoded constant.**
Change the lens, load the matching correction file, and counts/mm updates with
it. Two independently-set values would eventually disagree, and the failure
would be a quietly mis-scaled part.

Carry the scale in `MSG_FIELD_CORRECTION_BEGIN` alongside the grid size and CRC.

> **Caveat: the existing files do not use this.** `identity.cor`,
> `4_30_test1.cor` and `4_30_test3.cor` all carry `header[43] = 1.0`, the
> default placeholder from `save_correction_file()`, which would compute a
> nonsense 65,536 mm field. The scale is baked into the *table* as a ramp
> instead (see below). When regenerating files, separate the concerns: **scale
> in the header, distortion in the table.** Otherwise the ramp and the firmware
> constant are two expressions of the same quantity and will drift apart.

Exact offset, for the parser: after the 22-byte label, skip **2** bytes, then 63
doubles - scale is index 43. That lands at `0x210`, matching the 506-byte header
figure above (2 + 63*8 = 506).

### Interpolation - resolved

**Why 65 and not 64.** 65 nodes means 64 intervals, and a 16-bit field is 65536
counts, so the grid spacing is exactly **1024 counts** - a power of two. That is
almost certainly why the grid size was chosen, and it makes the lookup
division-free:

```c
i  = x >> 10;      /* cell index, 0..63 */
fx = x & 0x3FF;    /* fraction within the cell, 0..1023 */
```

**Bilinear, two-step, integer only** (per axis, so 6 multiplies for dx and dy):

```c
a = T00 + (((T10 - T00) * fx) >> 10);
b = T01 + (((T11 - T01) * fx) >> 10);
d = a   + (((b   - a  ) * fy) >> 10);
```

**Cost is a non-issue.** Roughly 40-60 cycles per sample including the eight
table loads and index arithmetic. At 100 kHz that is 4-6 M cycles/s out of 600 M
- **under 1% CPU.**

**The optimisation that matters is cell caching, not arithmetic.** At 1000 mm/s
the position advances ~3.75 counts per sample while a cell is 1024 counts wide,
so roughly **270 consecutive samples land in the same cell**. Compare `x >> 10`
and `y >> 10` against the cached cell and reload the four corners only when it
changes. That removes almost all the memory traffic; the multiplies were never
the problem.

**Storage:** `int16_t table[65][65][2]` interleaved as (dx, dy) pairs so both
components of a corner share a cache line - 16.9 KB. Put it in **DTCM**, which
is single-cycle and *not* cached, sidestepping the D-cache coherency traps the
DMA path already had to solve once with OCRAM.

**Decode sign-magnitude once, at load time.** Convert to ordinary two's
complement `int16_t` when reading the file into the table, so the hot path never
touches the `0x8000` sign-flag encoding.

**Apply per sample, not per vector endpoint.** Correcting only the endpoints and
drawing a straight line between them is wrong: the true corrected path bows,
and on a 175 mm field a long vector's midpoint would visibly miss. Correction
belongs in the sample loop, after fractional position accumulation and before
XY2-100 encoding, with the result clamped to the +/-30,000 limit.

**Decision: bilinear, for now.** f-theta distortion is smooth and low-order, and
bilinear on a 2.73 mm grid is what the BJJCZ and EZCAD cards do. Bicubic costs
~4x and needs 16 corners; revisit only if a measured table fails the criterion
below.

### The criterion for revisiting bicubic

For a function sampled at spacing h, linear interpolation error is about
`h^2 * f'' / 8`. In terms of the discrete grid that is simply:

> **bilinear error (counts) ~ (largest second difference between adjacent grid
> nodes) / 8**

Position quantisation is 1 count = 2.67 um - nothing finer can be commanded. So:

| Max second difference | Verdict |
|---|---|
| **< 8 counts** | bilinear error is below the quantisation floor; bicubic is provably pointless |
| 8-20 counts | marginal; measure before deciding |
| **> 20 counts** | bilinear is costing real accuracy; revisit bicubic |

A one-line check against any real `.cor`. Given SLM spot sizes of 50-100 um and
layer thickness in the tens of microns, bilinear is expected to clear this
comfortably - but it is now a measurement rather than an opinion.

## 16. The existing correction files contain no distortion data  `[FACT]` `[OPEN]`

Measured 2026-08-26 against all three files in the old repo. Every one is a
**pure linear ramp** - constant first difference, second difference exactly
zero:

| File | First difference | Second difference | What it actually is |
|---|---|---|---|
| `identity.cor` | 0 | 0 | all zeros, no correction at all |
| `4_30_test1.cor` | 190.509, constant | **0.000** | pure linear stretch, ~18.6% |
| `4_30_test3.cor` | 185.296, constant | **0.000** | pure linear stretch, ~18.1% |

A real f-theta table is curved - that is its entire purpose. Exactly-constant
first differences mean the 65 x 65 grid is being used to **scale the field**,
not to correct distortion. test1 and test3 differ by ~0.5%, consistent with
iterating toward a correct field size from 9-point measurements.

**The lens distortion has never been characterised.** That work is still ahead.

### Two different distortions - only one is the table's job

| | What it is | Fixed by |
|---|---|---|
| **Positional** | f-theta pincushion/barrel; spot lands off where commanded | the correction table |
| **Spot size / focus** | spot grows and deforms toward field edges | **not** the table - an optical property |

Spot-size variation is compensated at *slice* time by varying power, speed or
hatch spacing with field position. **This already works architecturally**: the
DXF carries per-vector speed and power, so field-dependent compensation can be
baked in by the slicer with no firmware change at all (see section 2).

### Characterising the positional distortion

Nine points can only fit a low-order model - scale, and perhaps keystone. They
cannot populate a 65 x 65 grid. Two better routes, probably combined:

1. **Compute a first-pass table analytically.** Two-mirror galvo systems have an
   inherent, calculable pincushion from the two mirrors sitting at different
   distances from the field, and f-theta residual distortion is a published
   lens spec. Focal length + working distance + mirror separation gets most of
   the way there.
2. **Refine by dense measurement.** A marked grid measured optically, rather
   than nine points.

Vendor distortion data, where supplied, is often *spot size* across the field
rather than positional - useful, but it feeds the slicer (row above), not the
table.

**Before building any of this**, read `meerk40t/balormk/correction_fitting.py`
in the old repo - it may already fit a table from measured points, and would be
better reused than reinvented.


## 17. What the old PC handler and Mega firmware revealed  `[FACT]` `[DONE]`

Read 2026-08-26: `Laser_controller_and_arduino/Arduino_HandlerV2.py` (the PC to
Arduino command layer) and the command parser in `Arduino_Trimmed_Program.ino`.

Treated as **evidence of what the machine needed, not as a design to copy** -
the machine owner notes it was written by students working things out. The
command set and the machine facts are the valuable parts; the structure is not.

### The recoat cycle as actually implemented

```
1. feeder.moveTo(supply)                       supply rises
2. bed: overshoot by BACKLASH_MM, then return  anti-backlash
3. bed.moveTo(build)                           bed drops
4. delay(2000)                                 RECOAT_SETTLE_MS
5. wiper -> RECOAT_DISTANCE_MM (285 mm)        forward sweep
6. delay(500)
7. wiper -> HOME_OFFSET_MM                     sweeps BACK
8. every N cycles, re-home the wiper
```

So the machine currently runs **supply-park with no clearance** - it drags back
over the freshly powdered bed every layer. That is the variant section 14 says
costs surface quality.

### Three architectural problems, all confirmed as must-fix

**1. The PC is the position authority.** `build_cur` / `supply_cur` live in the
Python handler, which computes `build_cur - recoat_build_step` and sends an
*absolute* target. The Mega does not track position between commands.

The current machine therefore **structurally cannot run without the PC** - not
as policy, but because the truth lives there. `axis_status_t` carrying position
on the Mega is the fix.

**2. Motion is fully blocking.** Every move is `while (distanceToGo()) { run(); }`.
During a move the Mega reads no sensors, checks no interlocks, and **cannot
receive an emergency stop**; only the limit switch in the loop condition is
consulted.

**This is a hard porting constraint, not a protocol item.** "`MSG_ESTOP` handled
first in every handler" is meaningless unless the motion loop is non-blocking.
The port must restructure motion, not translate it.

**3. `sendSensorData()` discards pending commands.**

```cpp
while (Serial.available() > 0) {
    String line = Serial.readStringUntil('\n');
    line.trim();   // read and thrown away
}
```

Any command arriving while a sensor report is being sent is **silently dropped**.
That is the origin of both the 120-second timeout in the Python handler and the
"drain stale completions" hack above it - scar tissue from this bug. Sequence
numbers and acks make it structurally impossible.

### Machine facts worth keeping

- **Build and supply cylinders have encoders and are closed loop. The recoater
  is an open-loop stepper.** That is why only the wiper needs periodic
  re-homing - it is the only axis that can drift. May become closed loop later.
- **Re-homing can move to during the mark.** The recoater's home is at the
  supply end, off the print bed, so homing it while the laser marks costs zero
  wall-clock and violates nothing. Requires non-blocking motion (above). The
  every-N-cycles interval can probably also be relaxed.
- **The anti-backlash overshoot is real mechanical compensation**, not
  superstition - the mechanics have genuine backlash. Preserve it.
- **`RECOAT_SETTLE_MS = 2000`: purpose no longer remembered.** Two seconds per
  layer is ~33 minutes over 1000 layers, so it is worth understanding. Plausible
  reason: letting powder and structure settle after the piston moves so stepper
  vibration does not disturb the bed before the blade sweeps. **Keep the current
  value as the default and make it a parameter; do not remove it blindly.**
  Measure before reducing.
- **The radiator fan is for water cooling the build plate.** Not currently
  connected - the cylinder adapter is plastic, so water cooling was not
  feasible. It also served the old laser, which now has its own chiller, so it
  may be dropped entirely. **Define it in the protocol (cheap) but do not design
  around it.**
- **`OXYGEN_OVERRIDE = true` was a testing aid**, so work could continue with a
  bad sensor. Right handling: keep it compile-time so it cannot be set
  accidentally, and report it explicitly so the PC UI can show a red warning
  with *both* the substituted value and the real reading.
- **The safety pins are inputs.** The Mega *observes* an independent hardware
  interlock chain rather than driving it, so firmware failure cannot defeat the
  interlock. Preserve that. Expected to migrate onto the Teensy board later.
- **Lights have three modes: `ambient`, `shadow`, `off`** (two relays).
  `shadow` is side-lighting so the camera can see surface topology - that is how
  recoat defects are spotted.
- **Lighting needs a settle delay before capture.** The webcam has a physical
  lens; autofocus, white balance and brightness all take time to adapt. Was
  handled PC-side. Should become a machine setting the PC can update, since it
  gets dialled in once and then rarely changes.

### Protocol gaps this exposed

| Gap | Fix |
|---|---|
| No light control at all | `MSG_LIGHT_SET` with `ambient`/`shadow`/`off` plus settle delay |
| One fan, but there are two | split chamber blower from radiator fan (the latter unwired for now) |
| No approach-direction concept | anti-backlash needs a direction-of-approach flag on moves |
| Settle time hardcoded | parameter on `recoat_cycle_t`, defaulting to 2000 ms |
| No re-home policy | interval expressible, and schedulable during a mark |
| `valid_mask` cannot express "overridden" | add an override mask, and report the true reading alongside the substituted one |

## 18. Firmware work queue

Protocol version 1 shipped in `a383eaa`; version 2 in `7e53d55` (rename) and
`3abc782` (semantics). **The protocol is done.** The Mega is implemented; the
Teensy and the ESP32 are not.

### Porting constraints - fix these while porting, not after

These are structural. Retro-fitting any of them means rewriting the code that
was built without them.

| # | Constraint | Why |
|---|---|---|
| P1 | **Motion must be non-blocking** | The old code sits in `while (distanceToGo()) { run(); }`. During a move the Mega reads no sensors, checks no interlocks and cannot receive `MSG_ESTOP`. Handling estop "first in every handler" is meaningless while the loop is blocked. |
| P2 | **Never drain and discard the input buffer** | `sendSensorData()` ended by reading and throwing away everything pending, silently dropping any command that arrived during a report. Delete that loop; feed every byte to the decoder. |
| P3 | **Position authority lives on the Mega** | The PC held `build_cur` / `supply_cur` and sent absolute targets, so the machine could not run without it. The Mega must track position itself and publish `axis_status_t`. |
| P4 | **SD reads in the main loop, never in the DMA refill ISR** | Filesystem access from interrupt context is a non-starter. The ISR consumes from a RAM ring; the main loop keeps it fed. |

P1, P2 and P3 are **done** - they are built into the Mega port. P4 is still
ahead, on the Teensy.

### Arduino Mega - done

Ported and building. Everything that was queued here has landed: the protocol
replaces the `CMD|...` text lines, motion is a non-blocking state machine,
position authority is on the board, the recoat cycle runs both park modes with
anti-backlash and periodic re-homing, and sensors, interlocks, lighting,
override reporting and airflow are all live. The code is the record now; the
detail that used to live here is in `src/arduino-mega/` and `PROTOCOL.md`.

Three things about it are decisions rather than transcription, and are worth
knowing before touching that board:

- **Software travel limits apply only to a homed axis.** An un-homed axis has
  no trustworthy zero, so only the switches protect it. This is what the old
  firmware's always-on `BED_OVERRIDE`/`FEED_OVERRIDE`/`WIPE_OVERRIDE` were
  really working around: with the pistons never homed, bounds against a
  meaningless zero would have blocked every descent.
- **A limit switch hit outside homing does not re-reference position.** It
  freezes the axis and raises `FAULT_LIMIT_UNEXPECT`. Re-zeroing there would
  let a glitching switch silently redefine zero.
- **Live interlock trips are reported but not latched.** Only a real fault
  needs `MSG_FAULT_CLEAR`; an open door clears itself when the door shuts,
  otherwise a clear would be needed after every powder load.

The three-stage argon purge moved onto the board as part of this. It had been
living in the PC controller (`purge_system()` in `LaserController.py`) and takes
up to ~40 minutes, so a machine that is supposed to run unattended could not
have it there. Stage order is physical - displace without stirring, then mix,
then shut the valve and prove it holds - and is documented in `PROTOCOL.md`.

Still open on the Mega, and none of it blocks a print:

- **`FAN_MODE_MAPPED` is premature, not wrong.** It was specified from a verbal
  description of a speed-to-fan map, and the PC never implemented one - it sends
  two fixed levels, `BLOWER_PURGE_SPEED` (PWM 50) and `BLOWER_PRINT_SPEED`
  (PWM 94), both Mega constants now. But the idea is sound: faster scanning
  throws more spatter and wants more flow. What is missing is only the input -
  no node that owns a fan currently sees scan speed. It becomes implementable
  the moment the Teensy tells the airflow node what speed it is marking at, so
  it is kept and refused rather than retired. **No decision needed now**; it
  resolves itself when the Teensy-to-airflow link exists.
- **`MSG_RESET` is refused.** A watchdog reset is the only clean way to do it on
  an AVR, and the stock Mega2560 bootloader does not reliably clear `WDRF`, so
  the board can come up in a reset loop needing a manual reflash. Revisit only
  if the bootloader is confirmed watchdog-safe.
- **The fan tach is TO BE REMOVED**, not implemented. It is still physically
  hooked up on pin 20 and is one of several tachometers, none of which is
  needed; the radiator fan it belongs to is itself likely to be deleted, since
  the build plate adapter is plastic and water cooling was never feasible.
  `fan_status_t.rpm` reports 0 and should keep doing so. Flagged for removal at
  the next hardware revision - do not build a pulse counter for it.
- **Argon consumption** is measured (solenoid-open seconds) but there is no
  protocol field to report it in.

#### Position across a power cycle - done, and it is a safety feature

**The build and supply pistons have a limit switch at the BOTTOM ONLY.**
Confirmed by the machine owner 2026-08-26. Driven far enough up they push the
weight out of the cylinder and break things, so the software travel limit is
the only protection at that end - and a software limit needs a zero to measure
from. An axis with no zero has an unprotected top of travel. That is the real
hole the old firmware's permanently-on `BED_OVERRIDE` / `FEED_OVERRIDE` left
open, and it is why position persistence is not a convenience feature.

`src/arduino-mega/persist.cpp` keeps the piston positions in EEPROM, so they
are homed **once** and come back on every boot after that with the upper bound
enforced throughout. Three trust levels, reported in `axis_status_t.flags`:
unknown / homed / restored, the last being `HOMED | POS_RESTORED`. Bounds apply
to restored as well as homed. Details in `PROTOCOL.md`.

The recoater is deliberately not persisted - open loop, drifts, moves several
times per layer, and has a switch at each end so homing it is quick.

Writes go one EEPROM byte per `service()` call, gated on `eeprom_is_ready()`:
a byte takes ~3.3 ms to burn, so writing a 14-byte record in one go would stall
the stepper service for ~45 ms. A record is only queued once every axis has
been still for two seconds and something actually changed, which works out at
roughly one per layer. 128 slots of wear levelling puts that over 12 million
layers against the 100k-cycle endurance.

**`AXIS_MOVE_NO_BOUNDS` is built**, after the machine owner supplied the
concrete need I had assumed away: the pistons must be driven to the very top of
their rail to get the build plate adapters out. There is no switch there
because there was no room for one in this build - that changes in the next
machine - and homing first is slow and disturbs whatever else is being reset.

Per move, never sticky. The Mega refuses it while any axis is moving or a job
is printing, and logs `LOG_WARN` every time it honours one. Refusing it during
a mark is the Teensy's job; the Mega cannot see that. Whether an operator may
set the flag is a UI question - mechanism on the board, policy upstream.

The limit switches remain unconditional: nothing in the firmware can stop a
switch halting an axis in the direction of travel.

#### EEPROM endurance, spelled out

An EEPROM cell tolerates about **100,000 write/erase cycles**. Writing the same
bytes every layer would burn through that in 100,000 layers - roughly **100
thousand-layer prints** - and then the store starts silently returning wrong
data. That is the whole reason for the rotating slots.

With 128 slots the record moves on each time, so any one slot is rewritten only
once every 128 records: 128 x 100,000 = **12.8 million records**, or about
**12,800 full prints**. That is the "12 million layers" figure - it is the
endurance ceiling, not a target.

The 3.3 ms is a different thing entirely. It is how long the chip physically
takes to burn one byte, during which a normal `eeprom_write` call just sits and
waits. A 14-byte record written that way would block for ~45 ms - the same
mistake as the old blocking motion loop, and it would show up as a stutter in
the steppers. So `persist::service()` writes at most one byte per pass and only
when `eeprom_is_ready()`, and the record trickles out over the next few
milliseconds of ordinary looping.

Write spacing follows the machine owner's guidance: a record is worth writing
once per layer, but not once per jog. `PERSIST_MIN_INTERVAL_MS` (10 s) means a
burst of setup jogs coalesces into one record, while a print's per-layer moves
are far enough apart to each get saved.

**This is a hardware requirement for the next board revision, not just a
firmware note.** `[ELSEWHERE]` - it belongs in the Galvo-Control-Board KiCad
repo alongside the other board decisions, and should move there with the
sections listed at the end of this document.

EEPROM is the wrong medium for position storage and is only just adequate here.
The Mega has 4 KB of it and a 100k-cycle endurance, and the whole rotating-slot
scheme exists to work around that. **Do not port this scheme onto the control
board.** If position tracking migrates to the Teensy, the board should carry
either:

- **FRAM** (e.g. an SPI part such as the FM25 family) - effectively unlimited
  write endurance, byte-writable, no burn delay at all, so none of the wear
  levelling, the settle timer, the write spacing or the byte-at-a-time
  trickling is needed. A few dollars and one SPI chip select.
- or **the SD card** that is already on the Teensy 4.1 for job files - free in
  hardware terms, but it shares the P4 constraint (no filesystem access from an
  ISR) and a card can be pulled or fail, so it wants the same
  write-then-verify discipline as the job file.

The Teensy 4.1's own "EEPROM" is emulated in flash and is *worse* than the
Mega's for this pattern, so it is not the answer.

**Why it matters enough to design in:** the stored position is the only thing
protecting the top of the piston travel, where there is no limit switch. A
storage medium that silently returns wrong data after wearing out is a storage
medium that silently removes that protection.

### Teensy - speaks the protocol; the laser and job surfaces are not built

Protocol wiring landed: `src/teensy-galvo/node.cpp` carries identity, liveness,
state, galvo status and a real `MSG_ESTOP`. Everything that reaches the laser or
the galvo is refused rather than stubbed.

The console and the protocol share one USB port. `console::poll()` became
`console::feed(char)` so `node::poll()` owns the reads and routes each byte;
the link reports whether it is mid-packet, so no framing knowledge is
duplicated. A truncated packet is abandoned after 250 ms, which is what stops
it swallowing everything typed afterwards. `tools/test_demux.py` covers the
routing rules.

| Item | Notes | Section |
|---|---|---|
| Buffer depth as named constants | one-line experiment instead of surgery | 3 |
| Fractional position accumulation | sub-count precision, round at output | 4 |
| `MSG_TIMING_OFFSET` applied | pre-shift power during refill | 5 |
| Job upload to SD, atomic commit | `MSG_JOB_UPLOAD_*` | 19 |
| Job streaming with per-layer CRC at read | ~8192-point vector buffer (P4) | 19 |
| Print log | 512-record ring, flush in the dwell | 19 |
| Job sequencer | the state machine that drives a print | - |

### Later

- **CAN transport** once transceivers are fitted - per-transport framing is
  documented but not written (section 13).
- **ESP32 airflow node** - the hardware does not exist yet.
- **Bench measurements** that unblock section 3 and section 16.

## 19. Job file on SD, and the print log  `[FIRMWARE]`

The printer must run a job with no PC attached, so **the job lives on the
Teensy's microSD card**. The PC uploads it once; printing reads from the card.

This replaces the load-into-RAM model of section 8, which only worked for
simple parts.

### Two phases, cleanly separated

| Phase | Path | Real-time? |
|---|---|---|
| **Upload** | PC -> Teensy -> microSD, whole job, once | No. Verify, retry, take as long as needed. |
| **Print** | SD -> vector buffer -> interpolator -> sample ring -> DMA | Yes, but no PC anywhere in it. |

Because verification no longer competes with marking, the atomic-commit
guarantee holds without compromise: transfer, verify, and only then mark the
job valid. **A failed transfer never becomes a printable job.**

### Job file structure

```
header        printer metadata, layer count, layer offset index
layer 0       CRC + vector data
layer 1       CRC + vector data
...
```

**Two CRCs, doing different jobs:**

- **Whole-file CRC** - proves the *upload* arrived intact. Checked once at
  `UPLOAD_END`.
- **Per-layer CRC** - proves the layer is intact **when read back**, which is
  the one that actually protects a print. A card can develop bad sectors weeks
  after the file was written correctly; a check at upload time says nothing
  about a read three weeks later. Checked at read, so the Teensy faults
  *before* melting a corrupt layer.

The layer offset index also gives seeking - jump to any layer, which is what
resuming after a pause or fault needs.

### Buffer chain and sizing

```
SD card  ->  vector buffer  ->  interpolator  ->  sample ring  ->  DMA
             ~8192 points                        small, see section 3
```

Worst-case consumption is 0.5 mm vectors at 2000 mm/s, about **4,000
vectors/second**; typical is nearer 1,000.

| Vector buffer | RAM | Cushion (typical / worst) |
|---|---|---|
| 4,096 points | 48 KB | 4 s / 1 s |
| **8,192 points** | **96 KB** | **8 s / 2 s** |
| 16,384 points | 192 KB | 16 s / 4 s |

**8,192 points is the starting figure.** SD stalls are typically under 100 ms
and occasionally 250 ms, so two seconds of worst-case cushion is a wide margin.
Refill below half, reading ~12 KB at a time - 24 sectors, efficient for the
card and short enough not to stall the loop.

**It is effectively free.** The sample ring is 128 KB today and section 3
already wants it far smaller for power-timing alignment. Dropping to 512 frames
per half is 8 KB, releasing **120 KB** - more than the 96 KB this needs. The
change wanted for laser timing funds the buffer needed for SD streaming.

Make both sizes named constants so they stay tunable.

### Print log

Recorded per job on the card, for correlating with layer images and video
afterwards, and for working out where the time actually goes.

- **Fixed-size binary records**, not text - trivial to parse, cheap to write,
  no formatting cost in the loop.
- **Periodic samples at ~1 Hz**: oxygen, airflow, temperatures, fan state.
  Nothing here changes fast enough to need more.
- **Timestamped events**: mark start and end, recoat start and end, faults.
  These are what yield the efficiency analysis - mark time versus recoat time
  versus idle, per layer, straight out of the timestamps.
- Motor load slots in later as another field once encoders report it.

**Sizing:** ~32 bytes per record at 1 Hz is ~115 KB/hour, so a 10-hour print is
about 1 MB.

**Ring of 512 records - 16 KB, about 8.5 minutes of buffer. Flush
preferentially during the recoat dwell; force-flush only above 90% full.**

Since a dwell occurs every layer and the buffer holds several minutes,
essentially every flush lands in a dwell where a stall costs nothing. The
force-flush is a backstop for a pathologically long layer, not the normal path.

### What an SD stall actually costs

Only the **main loop** blocks. Interrupts keep running, so:

- **Galvo marking never glitches** - DMA plus refill ISR.
- **The watchdog never trips** - `watchdog.h` states the kicker runs on a PIT
  timer specifically so a stuck `loop()` cannot trip it.
- **No incoming packet is lost** - receive is interrupt-driven and buffered;
  packets are processed late, not dropped.
- **The hardware safety chain is unaffected** - door, oxygen and temperature
  interlocks cut the laser without consulting firmware (section 17).
- **The Mega is unaffected** - it has no card, so sensing and interlock checks
  continue regardless.

What is delayed is the Teensy's *software* reaction, such as acting on a
received `MSG_ESTOP`, by up to ~250 ms. That is a degradation of a secondary
path, not a hole in the primary one - but it is the reason emergency stop must
stay a hardware path and never depend on the main loop.

Chunking the writes helps only slightly: the stall comes from the card's
internal erase and wear-levelling, not from write size, so smaller writes lower
the average but not the worst case.

Vector refills do happen during marking by necessity, and are covered by the
2-8 second cushion above. Even a log flush and a vector refill serialising into
a ~500 ms worst case sits comfortably inside it.

**Related, worth revisiting separately:** because the watchdog is kicked from a
timer ISR, it proves interrupts are alive but not that `loop()` is making
progress. That is deliberate and documented, but it means a hung main loop is
not detected. A progress-gated watchdog would need a timeout longer than the
worst SD stall.

### Rejected: exposing the card as a USB drive

Teensyduino can present the card as mass storage or MTP alongside serial, which
would let the PC copy a job by drag and drop. Rejected: the Teensy loses control
of validation, the card must be unmounted before printing, and transferring a
DXF stack over the protocol is quick enough that the convenience is not worth
it.
