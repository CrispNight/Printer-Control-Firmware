# Moiren SLM — machine communication protocol

**Version 1.** `protocol/protocol.h` is the single source of truth. This
document explains it; it does not define it. If the two disagree, the header
wins and this file is the bug.

`protocol/protocol.py` is **generated** from the header by
`tools/gen_protocol.py` and is what the PC-side repo imports. Regenerate after
every header change:

```
python tools/gen_protocol.py          # rewrite protocol.py
python tools/gen_protocol.py --check  # CI: fail if it is stale
```

---

## Design constraints

The protocol is shaped by four facts about this machine:

1. **The PC is optional.** The printer must complete a job with no host
   attached. Nothing in a print sequence may require a PC round-trip. The PC
   adds UI, file upload and camera monitoring on top; it is never in the
   safety or timing path.
2. **The topology is going to change.** Today the PC drives the Mega and the
   Teensy over two separate USB links. The Teensy is the intended master, and
   the next revision of the control card adds inter-board ports. Every packet
   therefore carries an explicit `src` and `dst`, so that migration is a
   routing change and not a protocol change.
3. **The Mega is small.** 8 KB of RAM, no FPU. Hence fixed-point integers
   rather than floats, a table-free CRC, and a 192-byte payload ceiling.
4. **Wires in a machine with a 200 W laser and stepper drivers are noisy.**
   Every packet is CRC-checked and the decoder resynchronises on its own; a
   corrupt packet costs one packet, never the link.

## Nodes

| Constant | Value | Board | Owns |
|---|---|---|---|
| `NODE_BROADCAST` | `0x00` | — | addressed to everyone |
| `NODE_PC` | `0x01` | host PC | UI, file upload, camera. Optional. |
| `NODE_TEENSY_GALVO` | `0x02` | Teensy 4.1 | galvo, laser source. Master in the target design. |
| `NODE_ARDUINO_MEGA` | `0x03` | Mega 2560 | steppers, O2/temperature, interlocks, **airflow today** |
| `NODE_ESP32_FAN` | `0x04` | ESP32 | EDF/ESC airflow. Not built yet. |

`NODE_AIRFLOW` is an alias, currently `NODE_ARDUINO_MEGA`. Address `MSG_FAN_*`
to the alias and moving airflow to the ESP32 is a one-line change in the
header rather than an edit at every call site.

### Current vs target wiring

```
TODAY                              TARGET
  PC                                 PC (optional)
  ├── USB ──> Teensy (galvo)         └── USB ──> Teensy 4.1  ── galvo + laser
  └── USB ──> Mega  (motion,                       ├── UART ──> Mega   (motion, sensors)
              sensors, airflow)                    └── UART ──> ESP32  (airflow)
```

The message definitions are identical in both. Only who forwards packets
changes.

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

**CRC coverage deliberately excludes the SOF bytes** so that the decoder can
discard a false start-of-packet and resynchronise without the CRC having to be
recomputed over a shifting window.

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
`PacketDecoder` in the generated `protocol.py` implements it for the PC. The two
are kept behaviourally identical on purpose — the CRC routine in each passes
the standard CCITT-FALSE check value `0x29B1` for input `"123456789"`.

## Message catalogue

Ids are grouped so the range identifies the concern.

### `0x0X` — system and link

| Id | Name | Payload | Notes |
|---|---|---|---|
| `0x01` | `MSG_PING` | — | |
| `0x02` | `MSG_PONG` | — | |
| `0x03` | `MSG_HELLO` | `sys_hello_t` | broadcast on boot, and in reply to `MSG_HELLO` |
| `0x04` | `MSG_ACK` | `sys_ack_t` | quotes the acknowledged msg id and seq |
| `0x05` | `MSG_LOG` | `sys_log_t` | diagnostics, forwarded to the PC when present |
| `0x06` | `MSG_HEARTBEAT` | `sys_heartbeat_t` | periodic; silence is `FAULT_COMMS_TIMEOUT` |
| `0x07` | `MSG_RESET` | — | soft reset |

### `0x1X` — machine state and job control

| Id | Name | Payload |
|---|---|---|
| `0x10` | `MSG_STATE_REQUEST` | — |
| `0x11` | `MSG_STATE` | `state_report_t` |
| `0x12` | `MSG_JOB_START` | `job_start_t` |
| `0x13` | `MSG_JOB_ABORT` | — |
| `0x14` | `MSG_JOB_PAUSE` | — |
| `0x15` | `MSG_JOB_RESUME` | — |
| `0x16` | `MSG_LAYER_BEGIN` | `layer_begin_t` |
| `0x17` | `MSG_LAYER_END` | `layer_end_t` |
| `0x18` | `MSG_JOB_COMPLETE` | — |

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

### `0x3X` — motion (Mega)

| Id | Name | Payload |
|---|---|---|
| `0x30` | `MSG_AXIS_HOME` | `axis_home_t` |
| `0x31` | `MSG_AXIS_MOVE` | `axis_move_t` |
| `0x32` | `MSG_AXIS_STOP` | `axis_home_t` (axis field only) |
| `0x33` | `MSG_AXIS_STATUS` | `axis_status_t` |
| `0x34` | `MSG_RECOAT_CYCLE` | `recoat_cycle_t` |

`MSG_RECOAT_CYCLE` is one message for a whole powder recoat — drop the bed,
raise the feed piston, sweep the blade — rather than three coordinated moves.
The sequence has to complete correctly with no host attached, so it belongs on
the board that owns the axes.

Axes: `AXIS_FEED` (powder piston), `AXIS_BED` (build platform Z),
`AXIS_WIPE` (recoater blade).

### `0x4X` — process sensing (Mega)

| Id | Name | Payload |
|---|---|---|
| `0x40` | `MSG_SENSOR_REPORT` | `sensor_report_t` |
| `0x41` | `MSG_PURGE_SET` | `purge_set_t` |

`sensor_report_t.valid_mask` marks which readings are trustworthy: bits 0–1
for `oxygen_ppm[0..1]`, bits 8–13 for `temp_c_x10[0..5]`. Unfitted or
known-bad channels report cleared bits rather than a plausible-looking number —
the old firmware learned this the hard way with a floating thermistor input
reading ~95 °C.

### `0x5X` — laser and galvo (Teensy)

| Id | Name | Payload |
|---|---|---|
| `0x50` | `MSG_LASER_ARM` | `laser_arm_t` |
| `0x51` | `MSG_LASER_PARAMS` | `laser_params_t` |
| `0x52` | `MSG_MARK_BATCH` | `mark_batch_header_t` + `vector_point_t[count]` |
| `0x53` | `MSG_MARK_ABORT` | — |
| `0x54` | `MSG_GALVO_STATUS` | `galvo_status_t` |

`laser_arm_t.key` must equal `LASER_ARM_KEY` (`0x4D4F4152`) or the packet is
refused with `ACK_BAD_PARAM`. A single corrupted byte should not be able to
arm a laser, and the CRC alone is not a strong enough guarantee for that one
message.

`MSG_MARK_BATCH` carries a header followed by up to
`MARK_BATCH_MAX_POINTS` (**20**) `vector_point_t` records in the same packet.
Coordinates are absolute bed micrometres; the Teensy applies the field
correction table. **The PC never sends raw DAC counts** — correction data
belongs with the board that owns the galvo, so a recalibration does not
require a matching PC-side update.

### `0x6X` — airflow (Mega today, ESP32 later)

| Id | Name | Payload |
|---|---|---|
| `0x60` | `MSG_FAN_SET` | `fan_set_t` |
| `0x61` | `MSG_FAN_STATUS` | `fan_status_t` |

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
| `_us`, `_ns`, `_ms`, `_s` | microseconds, nanoseconds, milliseconds, seconds |

## Machine states

```
BOOT ──> IDLE ──> HOMING ──> PURGING ──> READY ──> PRINTING ──> (job complete) ──> READY
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
4. Commit the header, this document and the regenerated `protocol.py`
   together. CI fails the build if `protocol.py` is stale.

Adding a new message id without touching any existing struct still needs a
version bump — a node that does not know the id answers `ACK_UNKNOWN_MSG`,
which is a worse diagnostic than a clean version mismatch at link-up.

### Constraints the generator enforces

`tools/gen_protocol.py` parses only the region between the `PROTOCOL-GEN
BEGIN`/`END` markers, and only a deliberately small subset of C:
`#define`d integer constants, `typedef enum { NAME = value, ... } name_t;`
and `typedef struct { ... } name_t;` with fixed-width members. Anything else
in that region is a hard error rather than a silent skip. Keep helper
functions, macros with expressions, and anything clever outside the markers.
