# Printer-Control-Firmware

Machine-side firmware for the Moiren SLM metal 3D printer.

One PlatformIO project, three boards. The printer is designed to run a job to
completion **with no PC attached** — the host is optional and adds UI, print
file upload and camera monitoring on top.

> **Status: skeleton.** The structure, build and communication protocol are in
> place and all three environments compile. No board driver code has been
> ported in yet, so nothing here moves an axis or fires the laser. See
> [Porting](#porting) for what comes next.

## Layout

```
platformio.ini          three environments, one per board
protocol/
  protocol.h            SINGLE SOURCE OF TRUTH for the wire format
  PROTOCOL.md           human-readable protocol documentation
  protocol.py           GENERATED Python bindings — the PC repo imports these
lib/
  moiren_link/          framing + dispatch shared by all three boards
src/
  teensy-galvo/         Teensy 4.1  — galvo + laser control
  arduino-mega/         Mega 2560   — steppers, O2/temp, safety interlocks
  esp32-fan-esc/        ESP32       — EDF fan / ESC airflow control
tools/
  gen_protocol.py       regenerates protocol.py from protocol.h
  test_protocol.py      round-trip checks for the generated bindings
```

## Boards

| Environment | Board | Responsibility |
|---|---|---|
| `teensy-galvo` | Teensy 4.1 | Galvo positioning, field correction, laser source control. **Intended master** of the machine. |
| `arduino-mega` | Arduino Mega 2560 | Feed / bed / wipe steppers, oxygen and temperature sensing, door and interlock safety. Owns airflow **today**. |
| `esp32-fan-esc` | ESP32 (`esp32dev`) | EDF fan speed via ESC. **Hardware does not exist yet** — the environment builds so work can start against a fixed protocol. |

### Topology, current and target

Today the PC drives the Mega and the Teensy over two separate USB links. The
Teensy is the intended master, and the next revision of the control card adds
inter-board ports so the Teensy can drive the other two directly.

Every frame carries an explicit source and destination node, so that migration
is a routing change and **not** a protocol change. Nothing in the message
definitions has to move when it happens.

Airflow is a live example: it runs on the Mega now (PWM to the blower
controller, mapped from scan speed, no flow sensor). Code addresses it as
`NODE_AIRFLOW`, an alias that currently resolves to the Mega. Moving airflow to
the ESP32 is a one-line change in `protocol.h`.

## Building

Requires the [PlatformIO](https://platformio.org/) VSCode extension, or
`pio` on the path.

```
pio run                        # build all three environments
pio run -e teensy-galvo        # build one
pio run -e arduino-mega -t upload
pio device monitor -e teensy-galvo
```

`pio run -e esp32-fan-esc` downloads the Espressif toolchain on first use.

## The protocol

`protocol/protocol.h` is the single source of truth for the machine
communication protocol. `protocol/protocol.py` is generated from it and is
what the PC-side repo imports, so the two sides cannot silently drift apart.

After **any** change to the header:

```
python tools/gen_protocol.py      # regenerate protocol.py
python tools/test_protocol.py     # round-trip checks
```

Commit the header, `PROTOCOL.md` and the regenerated `protocol.py` together.
CI fails the build if `protocol.py` is stale.

Read [protocol/PROTOCOL.md](protocol/PROTOCOL.md) for the frame format,
the message catalogue, units and the state machine.

## Porting

This repo replaces the firmware halves of two earlier codebases. Both stay
untouched as fallbacks; code is being lifted out of them, not migrated with
history.

| Source | What comes across | Destination |
|---|---|---|
| [Galvo-Control-Firmware](https://github.com/CrispNight/Galvo-Control-Firmware) | working Teensy 4.1 galvo + laser control (verified up to the output board, not yet against the laser source) | `src/teensy-galvo/` |
| [Histos](https://github.com/CrispNight/Histos) — modified MeerK40t, local copy at `../meerk40t_tests` | `Laser_controller_and_arduino/Arduino_Trimmed_Program/` | `src/arduino-mega/` |
| — | airflow control, new | `src/esp32-fan-esc/` |

Each `main.cpp` carries `TODO(port):` markers naming the modules to be split
out. Until a subsystem is ported, its messages are answered `ACK_REFUSED`
rather than silently accepted, so a half-ported build cannot fire the laser or
drive an axis.

The PC-side application (HMI, file transfer, webcam) lives in its own repo and
pulls `protocol/` from here.

## Safety

This machine has a fibre laser source and an argon-purged chamber. Two rules
hold in this codebase:

1. **`MSG_ESTOP` is acted on before anything else** in every message handler,
   in every state.
2. **Unported subsystems refuse commands.** An `ACK_REFUSED` is always
   preferable to a partially-wired output.
