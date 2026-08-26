#pragma once

#include <stdint.h>

// XY2-100 galvo command engine. FlexIO2 + eDMA scatter-gather ping-pong.
// See xy2_engine.cpp for the buffer/DMA scheme comment block.

namespace xy2 {

// Speed limits for pattern commands (mm/s). Exposed for the console.
constexpr float kDefaultSpeedMmS = 200.0f;
constexpr float kMinSpeedMmS     = 1.0f;
constexpr float kMaxSpeedMmS     = 5000.0f;

// One-time setup. Configures FlexIO2 timers/shifters/pins, primes both
// buffers with center frames, starts DMA scatter-gather. Returns false if
// the FlexIO pin mapping doesn't match what pins.h claims.
bool start_clk_sync();

// Print addresses of the two DMA buffers (bring-up diagnostic).
void print_buffer_addrs();

// Set the AM26C31 output-enable pin (silk 31). HIGH = outputs live.
void set_drv_oe(bool enable);
bool get_drv_oe();

// Retune the CLK bit-rate (100 kHz .. 2 MHz). No-op if out of range.
bool set_bit_clock_hz(uint32_t hz);

// Static position + pattern commands. Each installs the pattern under
// __disable_irq and then does a full-buffer refill so the transition is
// immediate and glitch-free.
void set_position(uint16_t x, uint16_t y);
// Freeze the beam at whatever position the running pattern is on right now.
void stop_at_current();
bool start_square  (float side_mm, float speed_mm_s, uint32_t* out_samples);
bool start_triangle(float side_mm, float speed_mm_s, uint32_t* out_samples);
bool start_circle  (float dia_mm,  float speed_mm_s, uint32_t* out_samples);

// Console-facing command handlers (all print to Serial).
void cmd_status();
void cmd_ringstat();
void cmd_sqcheck();
void cmd_patcheck();
void cmd_dumpring(uint32_t n);

// Kick off the "center -> corner -> center" step test. Returns immediately;
// the two 2 s holds and the state transitions run from testxy_tick().
// Calling while a run is already in progress is rejected (prints a message).
void cmd_testxy(uint16_t corner);

// Advance the testxy background state machine. Cheap when idle (a couple
// of comparisons). Call from loop().
void testxy_tick();

// Background health monitor (called from loop()). Silent unless something
// looks wrong; then prints DMA_ES / SHIFTERR / ring-drift diagnostics.
void monitor_tick();
void set_monitor_enabled(bool on);
void cmd_monitor_status();

}  // namespace xy2
