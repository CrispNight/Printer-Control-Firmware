# What a host needs from the machine

Raw material for the eventual UI specification and for the Teensy's job
sequencer. It is written **board by board as each one lands**, while the
reasoning is fresh, and assembled into the real UI document once the Teensy is
done — deciding the screens now, before the Teensy exists, would only get
redone.

Nothing here is a wire-format reference. That is `protocol/PROTOCOL.md`, which
this points at rather than repeats.

**Status:** Arduino Mega complete. Teensy and ESP32 outstanding.

---

## Arduino Mega

### Settings the host owns

These are dialled in once per machine and then rarely touched. The firmware
carries a default for each and the message overrides it, so a host that sends
nothing still gets sane behaviour. A settings page is the natural home.

| Setting | Where | Default | Why the host owns it |
|---|---|---|---|
| Purge target O2 | `purge_set_t.target_o2_ppm` | 3000 ppm (0.30 %) | depends on the alloy and how fussy the part is |
| Purge stage 2 timeout | `purge_set_t.timeout_s` | 1800 s | depends on chamber volume and argon supply |
| Purge minimum mixing | `purge_set_t.min_mix_s` | 60 s | per machine and per gas; the pockets it clears are what ruin a part |
| Light settle, per mode | `light_set_t.settle_ms` | ambient 1500 ms, shadow 1000 ms | a property of the camera, not the machine |
| Recoat settle | `recoat_cycle_t.settle_ms` | 2000 ms | reason for the value is no longer known; **measure before reducing** |
| Recoat park mode | `recoat_cycle_t.park_mode` | overflow | a real surface-quality vs. time tradeoff, still unmeasured |
| Recoat clearance | `recoat_cycle_t.clearance_um` | 0 | only meaningful with supply park |
| Recoat passes | `recoat_cycle_t.passes` | 1 | |
| Layer increments | `recoat_cycle_t.feed_um` / `bed_um` | — | per job, not per machine |

**Known gap: there is no settings read-back.** The Mega holds all of the above
but nothing can ask it what they currently are, and it reverts to defaults on
reboot. Today the host knows only because it sent them. That is fine while one
host owns the machine and wrong as soon as two do, or after an unexpected
reset. A read-back path is a protocol design question that spans both boards
and should be settled once, not twice.

### Indicators

**Per axis** (`axis_status_t`, broadcast at 10 Hz while moving and on every
completion) — feed, bed, wipe:

- position and target, micrometres
- **position trust**, which is the one that is easy to get wrong in a UI:

  | Flags | Show as | Meaning |
  |---|---|---|
  | neither | **unknown** | no zero. Travel limits cannot be applied; only the switches protect this axis. |
  | `HOMED` | **homed** | verified against a switch this power cycle. |
  | `HOMED` + `POS_RESTORED` | **restored** | zero came back from EEPROM. Believed, not verified — nothing stops an axis being moved by hand while the machine is off. |

  Restored must be visibly different from homed. This is the "was it homed on
  startup" indicator: in normal use an axis reads homed once, then restored on
  every boot after, and never returns to unknown.
- moving / at-limit / fault / enabled

**Chamber:** oxygen ×2 and temperature ×6 with a validity bit each
(`sensor_report_t`), interlock chain state (`safety_status_t`: door, oxygen,
temperature, plus the chain's own green lamp), machine state and fault flags
(`state_report_t`, `sys_heartbeat_t`).

Only channels that are meant to have a sensor can fail. Four of the six
thermistor inputs are bare pins and read implausibly by design; they report
invalid permanently and that is correct, not a fault.

### Things that must be shown in red

Every one of these is a case where the machine is not telling you the plain
truth about itself, and the whole reason they are reported at all is so a UI
can say so.

| Condition | Source | Show |
|---|---|---|
| A sensor reading is substituted | `MSG_SENSOR_OVERRIDE` | the substituted value **and** the real one beside it |
| All sensors are faked | same, whole mask set | test mode — the machine is not measuring anything |
| A move ran with travel limits off | `MSG_LOG` warn, "move with travel limits off" | it happened, and when |
| Purge did not hold | `FAULT_OXYGEN_HIGH` + log | the chamber failed verification; printing anyway is a choice |

`MSG_SENSOR_OVERRIDE` is sent on connect and on change only, never on the
periodic report — so a host must latch it, not wait for it.

### Alerts

| Fault | Latched? | What it means to an operator |
|---|---|---|
| `FAULT_LIMIT_UNEXPECT` | yes | an axis hit a limit switch where none was expected. Axis is frozen and its position was NOT re-referenced. Home it. |
| `FAULT_SENSOR_INVALID` | yes | a channel that is supposed to have a sensor has read implausibly for 3 s. `detail` is the `valid_mask` bit index. |
| `FAULT_OXYGEN_HIGH` | no (purge) | either the purge failed verification, or oxygen is over the interlock threshold |
| `FAULT_DOOR_OPEN` | no | door interlock tripped; clears itself when the door shuts |
| `FAULT_OVERTEMP` | no | temperature interlock tripped |
| `FAULT_PROTOCOL_ERROR` | no | link CRC/framing errors, reported every 16. **This is a cable, connector or noise problem**, not a software one — it shows up as "commands sometimes do nothing". |

Latched faults need `MSG_FAULT_CLEAR`. Live ones clear themselves, and a UI
that demands a clear for an open door will be cleared after every powder load.
`FAULTBIT_ESTOP` never clears over the link — it needs a physical reset.

### Long-running operations that need progress, not a spinner

- **Purge — up to about forty minutes.** `state_report_t.substate` carries the
  stage while `STATE_PURGING`: 1 displace, 2 mix, 3 verify. Showing oxygen
  against the target over time is far more useful than a percentage.
- **Homing** — `STATE_HOMING`; three approach passes per axis, plus a travel
  measurement on the wiper.
- **Recoat** — several seconds. No substate today; progress is visible as axis
  movement, and the ACK arrives on completion. Worth revisiting if a UI wants
  step-level detail.

### Command semantics a host must get right

- **Motion and recoat ACK on completion, not acceptance.** A move that takes
  thirty seconds answers in thirty seconds. Rejections (`ACK_BUSY`,
  `ACK_BAD_PARAM`, `ACK_BAD_STATE`) come back immediately. Do not treat a slow
  ACK as a lost packet — this is what the old host's 120-second timeout was
  really working around.
- **`feed_um` and `bed_um` on a recoat are increments**, not targets.
- **`AXIS_MOVE_NO_BOUNDS` is per move and never sticky.** Refused while
  anything is moving. This is the maintenance move that runs a piston to the
  top of its rail to get the build plate adapters out — a UI should confirm it,
  and it is the obvious thing to put behind a user access level.
- **`MSG_RESET` is refused** on this board. See the working notes: it is a
  bootloader hazard, not an oversight.
- **`FAN_MODE_MAPPED` and `FAN_MODE_CLOSEDLOOP` are refused.** No scan speed
  reaches the Mega and no flow sensor is fitted.

### Values the machine measures and reports only as log text

No protocol field exists for these yet. They are real numbers worth keeping,
and a host that only parses structs will drop them.

- **Argon consumed**, as solenoid-open seconds per purge. The old host kept a
  running litre total from exactly this.
- **Wiper travel**, measured against both switches on every home. A belt that
  has slipped or a frame that has shifted shows up here first, so it is worth
  trending rather than reading once.

---

## Teensy

Outstanding. Will cover job upload and streaming, field correction, laser
arming and timing, the print log, and the job sequencer.

## ESP32

Outstanding, and the hardware does not exist yet. Airflow, and the venturi flow
meter that would make `FAN_MODE_CLOSEDLOOP` real.
