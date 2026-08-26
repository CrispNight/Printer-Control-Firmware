# Moiren SLM — machine communication protocol

**Version 2.** `protocol/protocol.h` is the single source of truth. This
document explains it; it does not define it. If the two disagree, the header
wins and this file is the bug.

`protocol/protocol.py` is **generated** from the header by
`tools/gen_protocol.py` and is what the PC-side repo imports. Regenerate after
every header change:

```
python tools/gen_protocol.py          # rewrite protocol.py
python tools/gen_protocol.py --check  # CI: fail if it is stale
python tools/test_protocol.py         # round-trip checks
```

---

## Design constraints

The protocol is shaped by five facts about this machine:

1. **The PC is optional.** The printer must complete a job with no host
   attached. Nothing in a print sequence may require a PC round-trip. The PC
   adds UI, job upload and camera monitoring on top; it is never in the safety
   or timing path.
2. **The job lives on the machine.** Print jobs are uploaded once to the
   Teensy's microSD card and streamed from there. An intricate layer can run to
   200,000 vectors — 1.8 MB — which does not fit in RAM, and streaming from the
   PC would put the host back in the real-time path.
3. **The topology is going to change.** Today the PC drives the Mega and the
   Teensy over two separate USB links. The Teensy is the intended master, and
   the next revision of the control card adds inter-board links. Every packet
   therefore carries an explicit `src` and `dst`, so that migration is a
   routing change rather than a protocol change.
4. **The Mega is small.** 8 KB of RAM, no FPU. Hence fixed-point integers
   rather than floats, a table-free CRC, and a 192-byte payload ceiling.
5. **A machine with a fibre laser and stepper drivers is electrically noisy.**
   Every packet is CRC-checked and the decoder resynchronises on its own; a
   corrupt packet costs one packet, never the link.

## Terminology

Two things are called a *frame* in this machine, thousands of times apart in
rate. This document uses only the second word for ours:

| Term | Meaning |
|---|---|
| **XY2-100 frame** | one position word to the galvo, every 10–40 µs. Defined by the galvo spec; not ours to rename. |
| **data packet** | one message envelope between nodes. What this document describes. |

A **message** is *what you are saying* (`MSG_AXIS_MOVE`). A **packet** is the
envelope it travels in.

## Nodes

| Constant | Value | Board | Owns |
|---|---|---|---|
| `NODE_BROADCAST` | `0x00` | — | addressed to everyone |
| `NODE_PC` | `0x01` | host PC | UI, job upload, camera. Optional. |
| `NODE_TEENSY_GALVO` | `0x02` | Teensy 4.1 | galvo, laser, job storage. Master in the target design. |
| `NODE_ARDUINO_MEGA` | `0x03` | Mega 2560 | steppers, O2/temperature, interlocks, lighting, **airflow today** |
| `NODE_ESP32_FAN` | `0x04` | ESP32 | EDF/ESC airflow. Not built yet. |

`NODE_AIRFLOW` is an alias, currently `NODE_ARDUINO_MEGA`. Address `MSG_FAN_*`
to the alias and moving airflow to the ESP32 is a one-line change in the header
rather than an edit at every call site.

### Current vs target wiring

```
TODAY                                TARGET
  PC                                   PC (optional)
  ├── USB ──> Teensy (galvo)           └── USB ──> Teensy 4.1 ── galvo + laser + SD
  └── USB ──> Mega  (motion,                          │
              sensors, lighting,                      └── CAN bus ──┬──> Mega   (motion, sensors)
              airflow)                                              └──> ESP32  (airflow)
```

The message definitions are identical in both. Only the transport and who
forwards packets changes.

## Packet format

All multi-byte fields are **little-endian**. Header is 9 bytes, trailer is 2.

```
 offset  size  field
   0      1    SOF0    0xA5
   1      1    SOF1    0x5A
   2      1    VER     PROTOCOL_VERSION the sender was built against
   3      1    SRC     node_id_t of the sender
   4      1    DST     node_id_t of the recipient, or NODE_BROADCAST
   5      1    MSG     msg_id_t
   6      1    FLAGS   FLAG_* bits
   7      1    SEQ     rolling per-sender sequence number, wraps at 256
   8      1    LEN     payload length, 0..192
   9    LEN    PAYLOAD
  9+LEN   2    CRC16   CRC-16/CCITT-FALSE over bytes [2, 9+LEN)
```

Maximum packet is 203 bytes (`PACKET_MAX_LEN`).

**CRC coverage deliberately excludes the SOF bytes** so the decoder can discard
a false start and resynchronise without recomputing over a shifting window.

| Flag | Value | Meaning |
|---|---|---|
| `FLAG_NEEDS_ACK` | `0x01` | sender expects `MSG_ACK` quoting this `SEQ` |
| `FLAG_IS_RESPONSE` | `0x02` | this packet answers an earlier `SEQ` |
| `FLAG_IS_ERROR` | `0x04` | payload is a `fault_report_t` |
| `FLAG_NO_ROUTE` | `0x08` | do not forward; consume at `DST` only |

### Receiving

1. Scan for `A5 5A`.
2. Read 7 header bytes. If `LEN > 192`, this was not a packet start — discard
   one byte and rescan.
3. Read `LEN` payload bytes and 2 CRC bytes.
4. Verify the CRC. On mismatch, count it, discard one byte, rescan.
5. Drop the packet unless `DST` is this node or `NODE_BROADCAST`.
6. If `VER` differs from `PROTOCOL_VERSION`, reply `MSG_FAULT` with
   `FAULT_VERSION_MISMATCH` and drop it. **Nodes do not attempt to interoperate
   across protocol versions.**

`MoirenLink` (`lib/moiren_link/`) implements exactly this for the firmware;
`PacketDecoder` in the generated `protocol.py` implements it for the PC. Both
CRC routines pass the standard CCITT-FALSE check value `0x29B1` for input
`"123456789"`.

## Transports

**The packet is the logical unit; each transport encodes it natively.** Message
definitions are shared; framing is not.

### USB serial

The layout above, verbatim. Used PC↔Teensy and PC↔Mega today.

### CAN

The 9-byte header and 2-byte CRC would more than double CAN traffic and
duplicate a CRC the hardware already computes. So on CAN:

- the **29-bit extended identifier** carries `src`, `dst`, `msg` and flags
- the **8 data bytes** are pure payload
- the **hardware CRC** replaces ours
- payloads longer than 8 bytes **segment** across consecutive frames

Classic CAN only — the ESP32's TWAI controller and the MCP2515 are CAN 2.0, so
CAN FD is unavailable. Payloads needing segmentation: `sys_log_t` (49 B),
`sys_hello_t` (26), `sensor_report_t` and `sensor_override_t` (18),
`recoat_cycle_t` (18), `axis_move_t` (14), `state_report_t` (12).

Bandwidth is not a concern: realistic traffic is about **500 frames/s** against
roughly 1,900 available at 500 kbit/s under half load. That holds only because
**the vector stream never touches CAN** — jobs go to the Teensy's SD card over
USB. Putting layer data on CAN would cost seconds per layer.

## Message catalogue

Ids are grouped so the range identifies the concern.

### `0x0X` — system and link

| Id | Name | Payload | Notes |
|---|---|---|---|
| `0x01` | `MSG_PING` | — | |
| `0x02` | `MSG_PONG` | — | |
| `0x03` | `MSG_HELLO` | `sys_hello_t` | broadcast on boot, and in reply |
| `0x04` | `MSG_ACK` | `sys_ack_t` | quotes the acknowledged msg id and seq |
| `0x05` | `MSG_LOG` | `sys_log_t` | diagnostics, forwarded to the PC when present |
| `0x06` | `MSG_HEARTBEAT` | `sys_heartbeat_t` | periodic; silence is `FAULT_COMMS_TIMEOUT` |
| `0x07` | `MSG_RESET` | — | soft reset |

### `0x1X` — machine state, job control, job upload

| Id | Name | Payload |
|---|---|---|
| `0x10` | `MSG_STATE_REQUEST` | — |
| `0x11` | `MSG_STATE` | `state_report_t` |
| `0x12` | `MSG_JOB_START` | `job_start_t` — names a job already on the card |
| `0x13` | `MSG_JOB_ABORT` | — |
| `0x14` | `MSG_JOB_PAUSE` | — |
| `0x15` | `MSG_JOB_RESUME` | — |
| `0x16` | `MSG_LAYER_BEGIN` | `layer_begin_t` |
| `0x17` | `MSG_LAYER_END` | `layer_end_t` |
| `0x18` | `MSG_JOB_COMPLETE` | — |
| `0x19` | `MSG_JOB_UPLOAD_BEGIN` | `job_upload_begin_t` |
| `0x1A` | `MSG_JOB_UPLOAD_DATA` | `job_upload_data_t` + opaque file bytes |
| `0x1B` | `MSG_JOB_UPLOAD_END` | `job_upload_end_t` |

### `0x2X` — safety

| Id | Name | Payload |
|---|---|---|
| `0x20` | `MSG_SAFETY_STATUS` | `safety_status_t` |
| `0x21` | `MSG_ESTOP` | — |
| `0x22` | `MSG_FAULT` | `fault_report_t` |
| `0x23` | `MSG_FAULT_CLEAR` | — |

`MSG_ESTOP` is acted on by every node the instant it is decoded, regardless of
state, and before any other work in the handler. It is the one message that is
never queued.

**It is not the primary stop path.** The door, oxygen and temperature
interlocks are a hardware chain that cuts the laser without consulting
firmware; the Mega only observes it. `MSG_ESTOP` is a software path layered on
top, and a busy main loop can delay it by tens of milliseconds. Emergency stop
must never depend on it alone.

### `0x3X` — motion (Mega)

| Id | Name | Payload |
|---|---|---|
| `0x30` | `MSG_AXIS_HOME` | `axis_home_t` |
| `0x31` | `MSG_AXIS_MOVE` | `axis_move_t` |
| `0x32` | `MSG_AXIS_STOP` | `axis_home_t` (axis field only) |
| `0x33` | `MSG_AXIS_STATUS` | `axis_status_t` |
| `0x34` | `MSG_RECOAT_CYCLE` | `recoat_cycle_t` |

Axes: `AXIS_FEED` (powder piston, closed loop), `AXIS_BED` (build platform Z,
closed loop), `AXIS_WIPE` (recoater blade, **open loop** — it drifts and needs
periodic re-homing), `AXIS_BLADE_LIFT` (reserved, not fitted).

**Position authority lives on the Mega.** It tracks where each axis is; the
host does not. This is the difference that makes PC-free operation possible.

`axis_move_t.flags` can force a direction of approach
(`AXIS_MOVE_APPROACH_NEG` / `_POS`). The mechanics have real backlash, so the
bed always descends into position during a recoat, overshooting and returning.

#### The recoat sequence

`MSG_RECOAT_CYCLE` is one message for the whole cycle. **The Mega executes all
of it** — it owns the axes and limit switches, and the sequence must complete
correctly even if the link hiccups. Parameters travel in the message, so the
shape of the cycle is selectable without reflashing.

With `PARK_OVERFLOW`, the default:

```
1. mark layer
2. build plate drops                    <- this IS the return clearance
3. recoater returns overflow -> supply  <- passes over the lowered build
4. supply cylinder rises
5. recoater sweeps supply -> build -> overflow, spreading
6. mark layer
```

The build-plate drop does double duty: it makes room for the next layer *and*
clears the return traverse, so powder is spread only on the forward pass.

`PARK_SUPPLY` spreads on the forward pass and must then return over freshly
spread powder, which costs an extra drop-and-raise — hence `clearance_um`.

`settle_ms` pauses after the pistons move, before the sweep. The old firmware
used 2000 ms; **the reason is no longer remembered, so measure before
reducing it.**

**Nothing may be over the print bed while marking.** The laser would strike the
recoater.

### `0x4X` — process sensing and chamber (Mega)

| Id | Name | Payload |
|---|---|---|
| `0x40` | `MSG_SENSOR_REPORT` | `sensor_report_t` |
| `0x41` | `MSG_PURGE_SET` | `purge_set_t` |
| `0x42` | `MSG_LIGHT_SET` | `light_set_t` |
| `0x43` | `MSG_SENSOR_OVERRIDE` | `sensor_override_t` |

`sensor_report_t.valid_mask` marks which readings are trustworthy: bits 0–1 for
`oxygen_ppm[0..1]`, bits 8–13 for `temp_c_x10[0..5]`. Unfitted or known-bad
channels report cleared bits rather than a plausible-looking number — the old
firmware learned this the hard way with a floating thermistor input reading
~95 °C.

`MSG_SENSOR_OVERRIDE` reports channels being **substituted** rather than
measured, together with what the hardware actually reads underneath. Overrides
are compile-time on the sensing node so they cannot be set by accident, but
they must be *visible*: the UI should show the substituted value next to the
real one, in red. Sent on connect and on change only — never on the periodic
report, which would spend bandwidth ten times a second on something that
almost never changes.

Lighting has three modes. `LIGHT_SHADOW` is side-lighting, which is how surface
topology — and therefore recoat defects — becomes visible to the camera.
`light_set_t.settle_ms` is how long a caller should wait before capturing: the
webcam has a physical lens, and autofocus, white balance and exposure all need
time to adapt.

### `0x5X` — laser, galvo and calibration (Teensy)

| Id | Name | Payload |
|---|---|---|
| `0x50` | `MSG_LASER_ARM` | `laser_arm_t` |
| `0x51` | `MSG_LASER_PARAMS` | `laser_params_t` |
| `0x53` | `MSG_MARK_ABORT` | — |
| `0x54` | `MSG_GALVO_STATUS` | `galvo_status_t` |
| `0x55` | `MSG_TIMING_OFFSET` | `timing_offset_t` |
| `0x56` | `MSG_FIELD_CORRECTION_BEGIN` | `field_corr_begin_t` |
| `0x57` | `MSG_FIELD_CORRECTION_DATA` | `field_corr_data_t` + entries |
| `0x58` | `MSG_FIELD_CORRECTION_END` | — |

`0x52` is **retired**. It was `MSG_MARK_BATCH`, which both carried vectors and
started emission. Vectors now reach the machine as a job file on the SD card,
so no single message does both. The id is not reused.

`laser_arm_t.key` must equal `LASER_ARM_KEY` (`0x4D4F4152`) or the message is
refused with `ACK_BAD_PARAM`. A single corrupted byte should not be able to arm
a laser, and the packet CRC alone is not a strong enough guarantee for that one
message.

**Two different calibrations** are easy to confuse:

| | What it is | Message | Changes |
|---|---|---|---|
| **Timing offset** | laser lead/lag vs mirror, in samples | `MSG_TIMING_OFFSET` | once per machine, on the bench |
| **Field correction** | 65×65 grid of position offsets | `MSG_FIELD_CORRECTION_*` | rarely, on optics change |

Only the *difference* between the two path delays matters for the timing
offset: equal delay on both shifts everything uniformly and is invisible in the
part. It is distinct from the commanded dwells in `laser_params_t`, which are
deliberate settling pauses rather than a hardware correction.

## Job files and the SD card

The printer must run with no PC attached, so **the job lives on the Teensy's
microSD card.** Upload and printing are cleanly separated:

| Phase | Path | Real-time? |
|---|---|---|
| **Upload** | PC → Teensy → microSD, once | No. Verify, retry, take as long as needed. |
| **Print** | SD → vector buffer → interpolator → sample ring → DMA | Yes, but no PC in it. |

Because verification never competes with marking, the commit is atomic:
transfer, verify, and only then mark the job valid. **A failed transfer never
becomes a printable job.**

### Upload

`MSG_JOB_UPLOAD_BEGIN` announces the job id, total byte count, layer count and
whole-file CRC. `MSG_JOB_UPLOAD_DATA` carries **184 opaque file bytes** per
packet with a 0-based `chunk_index` that must arrive in order — a gap is
detected immediately rather than silently shifting the rest of the file.
`MSG_JOB_UPLOAD_END` commits, and the `MSG_ACK` carries the result.

The transport does not parse the file. Its layout is defined by
`job_file_header_t` and friends so both sides agree, but it can evolve without
a protocol version bump.

### File layout

```
job_file_header_t          magic "MOIRENJB", layer count, whole-file CRC
  per layer:
    layer_header_t         layer index, z, byte count, PER-LAYER CRC
      per parameter group:
        vector_group_t     point count
        laser_params_t     speed, power, delays for this group
        vector_point_t[]   the points
```

**Two CRCs, doing different jobs.** The whole-file CRC proves the *upload*
arrived intact and is checked once. The **per-layer CRC is the one that
protects a print**: it is verified at *read* time, because a card can develop
bad sectors weeks after a correct write. A layer that fails it faults before
anything is melted.

The layer records also make seeking possible — jump to any layer, which is what
resuming after a pause or fault needs.

### Grouping by parameters

Each vector in the source DXF carries its own speed and power, but most share
values and a curved contour holds constant power throughout. So points are
**grouped by parameter set**: one `laser_params_t` per group, then all the
points that use it. A constant-power contour costs one parameter record
regardless of how many vectors it contains — far cheaper than a power field on
every point.

### Streaming

Layers stream from the card into a vector buffer of roughly **8,192 points**,
refilled below half. Worst-case consumption is about 4,000 vectors/second
(0.5 mm vectors at 2000 mm/s), so that is seconds of cushion against the
occasional 100–250 ms stall a card takes for internal housekeeping.

**SD reads happen in the main loop, never in the DMA refill ISR.**

## Field correction

A 65×65 grid of position offsets correcting f-theta lens distortion. The Teensy
applies it per *sample*, not per vector endpoint — correcting only the ends of
a vector and drawing a straight line between them leaves the true path bowed.

The grid is 65 nodes because that is 64 intervals across a 16-bit field, making
the spacing exactly 1024 counts — a power of two, so the lookup is a shift
rather than a division.

### Upload

Same atomic pattern as job upload. `MSG_FIELD_CORRECTION_BEGIN` carries the
grid size, the **field scale**, and a CRC over the whole table.
`MSG_FIELD_CORRECTION_DATA` carries **47 points per packet** with an in-order
chunk index; a 65×65 table is **90 packets**. `MSG_FIELD_CORRECTION_END`
commits only if the count and CRC verify — any missing chunk or bad CRC rejects
the **entire** table and keeps the old one.

This matters: the previous system streamed 4,225 fire-and-forget writes with no
index and no checksum, and its only check confirmed the card was *responding*
rather than that every line had arrived. A single dropped line shifted every
subsequent point by one grid position, silently.

### Field scale

`field_corr_begin_t.scale_mcpmm` is the field scale in **milli-counts per
millimetre**, and it drives the firmware's counts/mm rather than a hardcoded
constant — change the lens, load the matching correction file, and the scale
follows.

```
65536 / scale = field width in mm
374500 mcpmm  = 374.5 counts/mm = a 175 mm lens
```

Valid range is `FIELD_SCALE_MIN_MCPMM`..`FIELD_SCALE_MAX_MCPMM`, i.e. **300 mm
down to 33 mm** of field. Anything outside is not this machine.

**A scale of 1.0 counts/mm is the known placeholder** written by the old
tooling, and would compute a 65,536 mm field. Reject it loudly and propose the
scale implied by the table's own ramp, then re-check that against the band —
never silently accept it, and never silently fix it.

Values in `field_corr_point_t` are ordinary signed offsets. The `.cor` file on
disk stores sign-magnitude with bit 15 as a sign flag; that is decoded once
when the file is read, never inside the interpolation path.

### `0x6X` — airflow (Mega today, ESP32 later)

| Id | Name | Payload |
|---|---|---|
| `0x60` | `MSG_FAN_SET` | `fan_set_t` |
| `0x61` | `MSG_FAN_STATUS` | `fan_status_t` |

Two independent fans, addressed by `fan_id_t`: `FAN_CHAMBER_BLOWER` (argon
circulation) and `FAN_RADIATOR` (build-plate water cooling — **not wired** on
the current machine; the plate adapter is plastic).

Modes: `FAN_MODE_OFF`, `FAN_MODE_MANUAL` (hold `duty_pm`), `FAN_MODE_MAPPED`
(map from scan speed — what the Mega does today), `FAN_MODE_CLOSEDLOOP` (hold
`target_flow_cm_s`; needs a flow sensor that is not fitted yet).

`rpm` and `flow_cm_s` report `0` when no tach or flow sensor is present.

## Units

Every quantity is a fixed-point integer. No floats cross the wire — the AVR,
ARM and Xtensa builds must agree byte for byte, and the AVR has no FPU. The
unit is in the member name:

| Suffix | Meaning |
|---|---|
| `_um`, `_um_s`, `_um_s2` | micrometres, µm/s, µm/s² |
| `_mm_s` | millimetres/second |
| `_pm` | per-mille, 0–1000 (so `power_pm = 750` is 75.0 %) |
| `_c_x10` | degrees Celsius × 10 (`251` is 25.1 °C) |
| `_ppm` | parts per million |
| `_mcpmm` | milli-counts per millimetre |
| `_us`, `_ns`, `_ms`, `_s` | microseconds, nanoseconds, milliseconds, seconds |

## Machine states

```
BOOT ──> IDLE ──> HOMING ──> PURGING ──> READY ──> PRINTING ──> (complete) ──> READY
                                                     │  ▲
                                                     ▼  │
                                                   PAUSED

any state ──(recoverable problem)──> FAULT ──(MSG_FAULT_CLEAR)──> IDLE
any state ──(estop)───────────────> ESTOP ──(physical reset only)
```

`STATE_READY` is the only state in which `MSG_JOB_START` is accepted; anything
else answers `ACK_BAD_STATE`.

## Faults

`fault_code_t` names a specific cause and travels in `fault_report_t`.
Separately, `FAULTBIT_*` is a bitfield of fault *domains* (door, oxygen, temp,
motion, laser, galvo, airflow, comms, estop) carried in `state_report_t`,
`sys_heartbeat_t` and `galvo_status_t` — a node can report several concurrent
problems in one 16-bit field without needing several packets.

## Changing the protocol

1. Edit `protocol/protocol.h`.
2. Bump `PROTOCOL_VERSION` for **any** change to the packet layout, message ids
   or payload structs. Nodes refuse to talk across versions, which is the
   intended behaviour: a mismatched pair should fail loudly at link-up rather
   than misinterpret a struct mid-print.
3. Run `python tools/gen_protocol.py`.
4. Run `python tools/gen_protocol.py --check` — **not just the test script.**
   The test prints the *stored* hash and will pass on a stale generated file.
5. Run `python tools/test_protocol.py`.
6. Commit the header, this document and the regenerated `protocol.py` together.

Adding a new message id without touching any existing struct still needs a
version bump — a node that does not know the id answers `ACK_UNKNOWN_MSG`,
which is a worse diagnostic than a clean version mismatch at link-up.

### Constraints the generator enforces

`tools/gen_protocol.py` parses only the region between the `PROTOCOL-GEN
BEGIN`/`END` markers, and only a deliberately small subset of C: `#define`d
integer constants, `typedef enum { NAME = value, ... } name_t;` and
`typedef struct { ... } name_t;` with fixed-width members. Anything else in
that region is a hard error rather than a silent skip. Keep helper functions,
macros with expressions, and anything clever outside the markers.
