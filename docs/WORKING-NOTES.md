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

**Decided 2026-08-26:** the XY2-100 "frame" is externally defined by the galvo
spec, so ours is the one that moves. It becomes a **data packet**.

- Docs: "data packet" on first use, "packet" thereafter.
- Code: `packet_t`, `PACKET_SOF0`, `PACKET_MAX_PAYLOAD`.
- `MoirenLink` keeps its name — that is the transport object, not the packet.
- Rejected: "USB packet" (transport-specific, and CAN is now in the picture),
  "Moiren packet" (the project name is wanted for higher-level things).

Gives a clean two-level split: a **message** is what you are saying
(`MSG_AXIS_MOVE`); a **packet** is the envelope it travels in. Not yet done —
it is a `PROTOCOL_VERSION` bump, cheap now.

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

**Intended change:** add `MSG_TIMING_OFFSET` carrying this offset, plumbed but
defaulting to zero. Needs a `PROTOCOL_VERSION` bump.

Called `MSG_GALVO_CAL` in an earlier draft, which was wrong twice over: it is an
*offset*, not a calibration (you calibrate once and transmit the result), and
"galvo cal" collided with the **field correction** table (section 15), which is
a completely different, spatial thing.

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

## 10. Verified Teensy 4.1 pin facts

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

## 11. Two-head expansion — what v0.2 should leave open

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

## 12. PSRAM — for job data, not DMA buffers

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


## 13. CAN as the inter-board transport

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

## 14. Recoat sequence

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

A future blade-lift axis would resolve it properly: raise the blade and do the
return traverse *during* marking, so it costs no wall-clock at all. That is a
mechanical change rather than firmware, but the axis enum should leave room.

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

## 15. Field correction table - format, and the bug to avoid

Distinct from the timing offset (section 5). This one is **spatial**: a 65 x 65
grid of position offsets correcting f-theta lens distortion. Recalculated
rarely.

### .cor file format (LMC1COR_1.0)

Decoded from `meerk40t/balormk/controller.py` in the old repo and checked
against `identity.cor` - the arithmetic closes exactly at 68,128 bytes.

| Offset | Size | Contents |
|---|---|---|
| `0x000` | 22 | `LMC1COR_1.0` in UTF-16LE |
| `0x016` | 506 | header - **scale factor at double index 43** |
| `0x210` | 67,600 | 65 x 65 points x 2 doubles (dx, dy) |

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

### Two open questions

- **The scale factor at header double index 43** - how it relates to the
  firmware's `374.5 counts/mm` field scale. If the .cor file's scale should
  drive that constant, the two must not be set independently.
- **Interpolation.** 65 points across +/-30,000 counts is ~937 counts between
  nodes, so the firmware must interpolate between grid points. The BJJCZ card
  did this internally; ours will have to, and the method affects accuracy.
