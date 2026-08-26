# DMLS_Galvo_protoboard — firmware overview

**Target:** Teensy 4.1 (NXP i.MX RT1062), Arduino/Teensyduino via PlatformIO. Single translation unit: `src/main.cpp` (~1480 lines).

**Role in the system:** bring-up firmware for a custom galvo/laser control card. On the finished PCB the Teensy is the *main controller*; a companion motion-control subsystem (motors, temperature, misc I/O) will hang off it. Slicing (Lachesis) is off-box.

## Peripheral responsibilities today

| Function | Peripheral used | Pins | Notes |
|---|---|---|---|
| XY2-100 galvo command | FlexIO2 + eDMA (scatter-gather ping-pong) | CLK=6, SYNC=7, X=8, Y=9 | Continuous 20-bit frames @ 100 kHz frame rate. Boots at 500 kHz bit clock (SN75174N margin); design target 2 MHz once uA9638/AM26C31 arrives. |
| RS-422 driver enable | GPIO | 23 | Boots LOW (outputs tri-stated). `enable`/`disable` command. |
| Analog outputs (0–10 V, e.g. laser power / aux) | I2C1 to GP8403 DFRobot DAC | SCL=19, SDA=18, addr 0x5F | 2 channels, boot-safe 0 V, plus stepping/sweep test modes. |
| Console | USB Serial @ 115200 | — | Full line editor: history ring (8), arrow keys, home/end, insert, delete. |
| Heartbeat | GPIO | LED_BUILTIN | 1 Hz toggle. |

## Data flow — XY2-100 engine

1. Two 8192-frame DMAMEM buffers (`g_frames_a`, `g_frames_b`), interleaved `[X_word, Y_word]`. 64 KB each.
2. Two aligned TCDs in DTCM (`g_tcd_a`, `g_tcd_b`), each `DLASTSGA`→the other. eDMA ping-pongs A→B→A forever via `ESG`. `INTMAJOR` fires the ISR on every swap.
3. ISR (`main.cpp:1101`) reads DMA `SADDR`, determines which buffer just went idle, and refills it via `xy2_refill_buffer()` → `xy2_gen_sample()`. `arm_dcache_flush()` on the refilled buffer, since OCRAM is WBWA-cached.
4. eDMA minor loop: `NBYTES=8`, `DMOD=4`, `DOFF=8` → alternates writes to `SHIFTBUFBIS[0]` (shifter 0 = X) and `SHIFTBUFBIS[2]` (shifter 2 = Y). Only shifter 0 requests DMA; shifter 2 empties in lockstep via shared Timer 0.
5. FlexIO2 Timer 0 (TIMOD=1 baud, 20-shift burst) drives CLK; Timer 1 (TIMOD=2 PWM, TRGSRC=Timer0) drives SYNC.
6. Frame encoding: `[001 | pos<15:0> | even-parity]` shifted left by 12 to sit at the MSB end of the 32-bit shifter register.

## Pattern engine

- `PatternState` (kind, total_samples, next_sample, param1, static_x/y) — pure function `xy2_gen_sample(idx, x, y)` consumed by both the ISR-driven streaming refill and the diagnostic `patcheck` tool.
- Kinds: `NONE` (static hold), `SQUARE`, `TRIANGLE`, `CIRCLE`. Field scale = 374.5 counts/mm; hard safety limit ±30 000 counts (~80 mm).
- Command handlers (`xy2_set_position`, `xy2_start_square/triangle/circle`) update pattern state under `__disable_irq`, then call `xy2_full_buffer_refill()` which fills both buffers + disable/reload TCD_A/enable to guarantee no partial-refill jump on transition.

## DAC (GP8403) subsystem

- `gp8403_init`, `gp8403_set_code`, `gp8403_set_volts`; range 0–10 V forced at boot.
- `DacState` machine with `IDLE`, `STEPPING` (`dacstep <n> [ch]`, 1 s/step), `SWEEPING` (`dacsweep [sec] [ch]`, triangle wave). Ticked from `loop()` non-blocking.

## Console commands

- **Motion/engine:** `help`, `ping`, `uptime`, `status`, `xy <X> <Y>`, `center`, `square`, `triangle`, `circle`, `stop`, `enable`, `disable`, `clkrate`, `testxy`.
- **Diagnostics:** `mon [on|off]`, `ringstat`, `sqcheck`, `patcheck`, `dumpring`, `gpio9test`, `gpio9low`.
- **DAC:** `dac`, `dacstep`, `dacsweep`, `dacoff`, `dacstat`, `dacscan`, `dacraw`.

## Background health monitor

`monitor_tick()` runs every 500 ms from `loop()`. Silent unless it observes:
- DMA_ES change
- FlexIO SHIFTERR latch
- Ring-drift during static hold (32 sample points × 2 buffers, compared against expected encoded word)

## Structure at a glance (all in main.cpp)

- **L1–24** — high-level comment block explaining the buffer/DMA scheme.
- **L25–204** — namespace: pin/timer/shifter constants, buffer & TCD storage, pattern state, DAC state, monitor state, console state.
- **L206–293** — GP8403 driver + `dac_tick`.
- **L295–815** — `handle_line()`: command dispatcher (this is where most of the surface area lives).
- **L820–977** — Console line editor (`poll_serial`, insert/delete/history/ANSI).
- **L981–1063** — XY2 encode + `xy2_gen_sample` pattern math.
- **L1071–1195** — `xy2_refill_buffer`, `xy2_dma_isr`, `monitor_tick`.
- **L1208–1324** — `xy2_full_buffer_refill` + position/pattern command entry points.
- **L1338–1443** — FlexIO+DMA setup (`xy2_build_tcd`, `xy2_setup_dma_pingpong`, `xy2_start_clk_sync`).
- **L1447–1482** — Arduino `setup()` / `loop()`.

## Constraints & gotchas worth carrying into PCB planning

- OCRAM is D-cache write-back; every DMA-visible buffer write must be followed by `arm_dcache_flush`.
- Full-buffer refill takes ~10 register writes + memcpy; disable/reload/enable of the DMA channel is sub-µs and completes inside one bit-shift period even at 500 kHz.
- SN75174N is the current bit-rate ceiling — pick a faster line driver on the PCB (uA9638 / AM26C31 or similar).
- RC1001 needs 100 Ω termination at the *driver* end and a common ground back to the card.
- FlexIO2 D10/D11/D16/D17 are hard-consumed on the pinout — reserve those on the PCB, or migrate to a different FlexIO instance if the pinout demands it.
- I2C1 is shared with the DAC and will be shared with any additional I2C on the PCB — plan for pull-ups and address planning if adding sensors.
- No sub-controller / motion / temperature interface exists today. `loop()` polls three ticks (`poll_serial`, `monitor_tick`, `dac_tick`) — plenty of headroom to add an inter-MCU link (UART/SPI/CAN) without disturbing the DMA engine, which is fully autonomous.
- No persistent storage, no config, no error log. All state is boot-defaults.
