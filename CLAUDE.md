# Working in this repo

Machine-side firmware for an SLM metal 3D printer: three boards, one
PlatformIO project. Read `README.md` for the layout and `protocol/PROTOCOL.md`
for the wire format before changing anything under `protocol/`.

## Hard rules

- **`protocol/protocol.h` is the single source of truth.** Never hand-edit
  `protocol/protocol.py` — it is generated. After any header change, run
  `python tools/gen_protocol.py` and `python tools/test_protocol.py`, and
  commit the header, `PROTOCOL.md` and the regenerated `protocol.py` together.
- **Bump `PROTOCOL_VERSION`** for any change to the frame layout, message ids
  or payload structs, including adding a new message id.
- **No floats on the wire.** The Mega has no FPU; every quantity is a
  fixed-point integer with its unit in the member name (`_um`, `_pm`,
  `_c_x10`, `_ppm`). See the units table in `PROTOCOL.md`.
- **`MSG_ESTOP` is handled first** in every handler, in every state, before
  any other work.
- **A subsystem that is not ported refuses its messages** with `ACK_REFUSED`.
  Do not stub a laser or motion command as a silent no-op that returns
  `ACK_OK` — on this machine that reads as "it worked".
- **The PC is optional.** Nothing in a print sequence may require a host
  round-trip. If a decision has to be made mid-layer, it belongs on a board.

## Build and check

```
pio run                        # all three environments
pio run -e teensy-galvo        # one board
python tools/gen_protocol.py --check
python tools/test_protocol.py
```

The Teensy and AVR toolchains are installed locally; `esp32-fan-esc` downloads
the Espressif toolchain on first build and needs network.

`lib_deps` for AccelStepper is commented out in `platformio.ini` — uncomment
it when `src/arduino-mega/motion.cpp` lands.

## Porting notes

Code is being lifted out of two older repos, which stay untouched as
fallbacks. The old Mega firmware is at
`../meerk40t_tests/Laser_controller_and_arduino/Arduino_Trimmed_Program/`.

When porting it, **carry the pin comments across verbatim**. Several of them
record hardware faults that were expensive to find — the pull-down on A2, the
`WIPE_STEP` move to pin 49 off Timer 5, the fan tach on pin 20. Do not tidy
them into a table that loses the reason.

Each `main.cpp` has `TODO(port):` markers naming the modules to split out.
Follow that split rather than dropping a 33 KB `.ino` in whole.

## Style

Match the surrounding code: 4-space indent, `snake_case_t` types, `UPPER_CASE`
constants, comments that explain *why* rather than restating the line. Keep
`protocol.h` to plain C — no C++-only constructs, since the code generator and
potential C consumers both parse it.
