#include "xy2_engine.h"

#include <Arduino.h>
#include <FlexIO_t4.h>
#include <DMAChannel.h>
#include <math.h>

#include "config.h"
#include "pins.h"

// Two separate 8K-frame buffers (g_frames_a, g_frames_b) hold interleaved
// X/Y data. eDMA uses scatter-gather to auto-swap between them at each
// major loop boundary: g_tcd_a --SG--> g_tcd_b --SG--> g_tcd_a --SG--> ...
// The DMA ISR fires on each SG swap and refills the buffer that just went
// idle. CPU only ever touches the idle buffer, DMA only ever touches the
// active buffer -- zero shared access -- eliminating steady-state pattern
// tearing (previously seen when CPU refill of one half raced DMA read of
// the same shared ring across cache/AXI).
// Single DMA channel feeds both shifters via destination modulo (DMOD=4):
// each minor loop writes X to SHIFTBUFBIS[0] then Y to SHIFTBUFBIS[2].
// Frame format (20 bits, MSB first):
//   [ 001 | 16-bit position D15..D0 | even parity ], shifted left by 12
//   to sit at the MSB end of the 32-bit shifter register.

namespace xy2 {

namespace {

constexpr uint16_t kCenterPosition = 0x8000;

constexpr uint32_t kFlexClockHz = 120000000;

// Two 8K-frame buffers, interleaved [X, Y] per frame. 64 KB each. At 100 kHz
// frame rate, playing one buffer takes 81.92 ms.
constexpr uint32_t kBufSamples = 8192;
DMAMEM __attribute__((aligned(32))) uint32_t g_frames_a[kBufSamples * 2];
DMAMEM __attribute__((aligned(32))) uint32_t g_frames_b[kBufSamples * 2];

// Scatter-gather TCD descriptors. Each buffer's TCD has DLASTSGA pointing
// at the other's, so DMA ping-pongs A -> B -> A -> B forever. 32-byte
// aligned for eDMA. Live in DTCM.
alignas(32) DMABaseClass::TCD_t g_tcd_a;
alignas(32) DMABaseClass::TCD_t g_tcd_b;

DMAChannel g_dma;
bool g_dma_ready = false;
FlexIOHandler* g_flex = nullptr;
bool g_xy2_running = false;

uint32_t g_bit_clock_hz = kBootBitClockHz;

// Runtime-current position (updated by static xy or pattern gen).
volatile uint16_t g_pos_x = kCenterPosition;
volatile uint16_t g_pos_y = kCenterPosition;

// ---- Pattern engine -----------------------------------------------------
enum PatternKind : uint8_t {
  PATTERN_NONE = 0,   // static point
  PATTERN_SQUARE,
  PATTERN_TRIANGLE,
  PATTERN_CIRCLE
};

struct PatternState {
  volatile PatternKind kind;
  volatile uint32_t total_samples;
  volatile uint32_t next_sample;
  volatile int32_t  param1;          // half-side (square) or radius (tri/circle)
  volatile uint16_t static_x;
  volatile uint16_t static_y;
};

PatternState g_pattern = {
  .kind = PATTERN_NONE,
  .total_samples = 1,
  .next_sample = 0,
  .param1 = 0,
  .static_x = kCenterPosition,
  .static_y = kCenterPosition
};

// ---- ISR / DMA diagnostics ----------------------------------------------
volatile uint32_t g_isr_count = 0;
volatile uint32_t g_isr_last_saddr_was_a = 0;
volatile uint32_t g_isr_last_saddr_was_b = 0;

// ---- Background health monitor ------------------------------------------
constexpr uint32_t kMonPeriodMs = 500;
bool     g_mon_enabled       = true;
uint32_t g_mon_last_check_ms = 0;
uint32_t g_mon_last_dma_es   = 0;
uint32_t g_mon_last_shifterr = 0;
uint32_t g_mon_drift_events  = 0;
uint32_t g_mon_ticks         = 0;

// ---- testxy background state machine ------------------------------------
// Non-blocking version of the "center -> corner -> center" step test.
// cmd_testxy() sets state to TESTXY_CENTER and returns; testxy_tick()
// (called from loop()) does the two 2 s holds and prints the MARKs.
constexpr uint32_t kTestxyHoldMs = 2000;

enum TestxyState : uint8_t {
  TESTXY_IDLE = 0,
  TESTXY_CENTER,   // holding at center; when 2 s elapse -> jump to corner
  TESTXY_CORNER,   // holding at corner; when 2 s elapse -> return + done
};

TestxyState g_testxy_state       = TESTXY_IDLE;
uint32_t    g_testxy_entered_ms  = 0;
uint16_t    g_testxy_corner      = 0;

// ---- Encoding + safe-position clamp -------------------------------------

uint32_t encode_shifter_word(uint16_t position) {
  uint32_t frame20 = (0b001U << 17) | ((uint32_t)position << 1);
  frame20 |= __builtin_popcount(frame20) & 1u;
  return frame20 << 12;
}

uint16_t safe_pos(int32_t offset) {
  int32_t v = 0x8000 + offset;
  if (v < 0) v = 0;
  if (v > 0xFFFF) v = 0xFFFF;
  return (uint16_t)v;
}

// ---- Pattern sample generator -------------------------------------------
// Pure function of sample index + current pattern state. Called from the
// ISR-driven refill and from patcheck. Returns (x, y) uint16 for that
// sample.
void gen_sample(uint32_t sample_idx, uint16_t& x, uint16_t& y) {
  PatternKind kind = g_pattern.kind;
  uint32_t    total = g_pattern.total_samples;
  int32_t     p1    = g_pattern.param1;

  if (kind == PATTERN_NONE || total == 0) {
    x = g_pattern.static_x;
    y = g_pattern.static_y;
    return;
  }

  uint32_t idx = sample_idx;
  if (idx >= total) idx %= total;

  switch (kind) {
    case PATTERN_SQUARE: {
      uint32_t per_side = total / 4;
      if (per_side == 0) { x = g_pattern.static_x; y = g_pattern.static_y; return; }
      uint32_t side = idx / per_side;
      int32_t  sub  = (int32_t)(idx - side * per_side);
      int32_t  n    = (int32_t)per_side;
      int32_t  h    = p1;
      int32_t xo = 0, yo = 0;
      switch (side & 3) {
        case 0: xo = -h + (2 * h * sub / n); yo = -h;                       break;
        case 1: xo = +h;                     yo = -h + (2 * h * sub / n);   break;
        case 2: xo = +h - (2 * h * sub / n); yo = +h;                       break;
        case 3: xo = -h;                     yo = +h - (2 * h * sub / n);   break;
      }
      x = safe_pos(xo);
      y = safe_pos(yo);
      return;
    }
    case PATTERN_TRIANGLE: {
      uint32_t per_side = total / 3;
      if (per_side == 0) { x = g_pattern.static_x; y = g_pattern.static_y; return; }
      uint32_t side = idx / per_side;
      float frac = (float)(idx - side * per_side) / (float)per_side;
      constexpr float kTwoPi = 6.283185307f;
      float a0 = (float)M_PI * 0.5f + kTwoPi * (float)side       / 3.0f;
      float a1 = (float)M_PI * 0.5f + kTwoPi * (float)(side + 1) / 3.0f;
      float r = (float)p1;
      float sx = r * cosf(a0), sy = r * sinf(a0);
      float ex = r * cosf(a1), ey = r * sinf(a1);
      int32_t xo = (int32_t)(sx + (ex - sx) * frac);
      int32_t yo = (int32_t)(sy + (ey - sy) * frac);
      x = safe_pos(xo);
      y = safe_pos(yo);
      return;
    }
    case PATTERN_CIRCLE: {
      constexpr float kTwoPi = 6.283185307f;
      float a = kTwoPi * (float)idx / (float)total;
      float r = (float)p1;
      int32_t xo = (int32_t)(r * cosf(a));
      int32_t yo = (int32_t)(r * sinf(a));
      x = safe_pos(xo);
      y = safe_pos(yo);
      return;
    }
    default:
      x = g_pattern.static_x;
      y = g_pattern.static_y;
      return;
  }
}

// Refill one full buffer starting from g_pattern.next_sample. Called from
// the DMA ISR on the buffer that just went idle (DMA is now playing the
// other buffer). arm_dcache_flush() at the end because OCRAM is WBWA-cached.
void refill_buffer(uint32_t* buf) {
  uint32_t next = g_pattern.next_sample;
  uint32_t total = g_pattern.total_samples;
  uint16_t last_x = g_pos_x, last_y = g_pos_y;
  for (uint32_t i = 0; i < kBufSamples; i++) {
    uint16_t x, y;
    gen_sample(next, x, y);
    if (total > 0) {
      if (++next >= total) next = 0;
    }
    last_x = x;
    last_y = y;
    uint32_t idx = i * 2u;
    buf[idx + 0] = encode_shifter_word(x);
    buf[idx + 1] = encode_shifter_word(y);
  }
  arm_dcache_flush(buf, kBufSamples * 2u * sizeof(uint32_t));
  g_pattern.next_sample = next;
  g_pos_x = last_x;
  g_pos_y = last_y;
}

// DMA ISR. Fires at each SG-swap (INTMAJOR). The hardware TCD has already
// been reloaded from the incoming SG descriptor by the time this runs, so
// SADDR points into the buffer that is NOW active. Refill the OTHER one.
void dma_isr() {
  uint32_t saddr = (uint32_t)g_dma.TCD->SADDR;
  uint32_t a_lo = (uint32_t)g_frames_a;
  uint32_t a_hi = a_lo + sizeof(g_frames_a);
  bool on_a = (saddr >= a_lo) && (saddr < a_hi);
  if (on_a) g_isr_last_saddr_was_a++; else g_isr_last_saddr_was_b++;
  g_isr_count++;
  uint32_t* idle_buf = on_a ? g_frames_b : g_frames_a;
  refill_buffer(idle_buf);
  g_dma.clearInterrupt();
  asm volatile ("dsb");
}

// Refill BOTH buffers with fresh pattern samples starting from
// g_pattern.next_sample, then atomically reset DMA to read from A[0] with
// cache-coherent content. Called from command handlers after any pattern
// change so the transition is glitch-free.
//
// CRITICAL: uses a LOCAL next-sample counter throughout so that if the DMA
// ISR fires mid-refill, the ISR reads the OLD g_pattern.next_sample (still
// unchanged) and generates samples starting from the same base --
// eliminating the "8192-sample skip" bug that occurred with per-buffer
// next_sample publishing.
void full_buffer_refill() {
  uint32_t next = g_pattern.next_sample;
  uint32_t total = g_pattern.total_samples;
  uint16_t last_x = g_pos_x, last_y = g_pos_y;

  for (uint32_t i = 0; i < kBufSamples; i++) {
    uint16_t x, y;
    gen_sample(next, x, y);
    if (total > 0) { if (++next >= total) next = 0; }
    last_x = x; last_y = y;
    uint32_t idx = i * 2u;
    g_frames_a[idx + 0] = encode_shifter_word(x);
    g_frames_a[idx + 1] = encode_shifter_word(y);
  }
  arm_dcache_flush(g_frames_a, sizeof(g_frames_a));

  for (uint32_t i = 0; i < kBufSamples; i++) {
    uint16_t x, y;
    gen_sample(next, x, y);
    if (total > 0) { if (++next >= total) next = 0; }
    last_x = x; last_y = y;
    uint32_t idx = i * 2u;
    g_frames_b[idx + 0] = encode_shifter_word(x);
    g_frames_b[idx + 1] = encode_shifter_word(y);
  }
  arm_dcache_flush(g_frames_b, sizeof(g_frames_b));

  // Disable/reload/enable is ~10 register writes, sub-µs -- completes well
  // within one shifter shift cycle (10 µs @ 2 MHz, 40 µs @ 500 kHz). No
  // shifter underrun, no SHIFTERR, no glitch -- next DMA fetch reads A[0].
  __disable_irq();
  g_dma.disable();
  *((DMABaseClass::TCD_t*)g_dma.TCD) = g_tcd_a;
  g_dma.enable();
  __enable_irq();

  g_pattern.next_sample = next;
  g_pos_x = last_x;
  g_pos_y = last_y;
}

// ---- FlexIO + DMA setup helpers -----------------------------------------

// Build one buffer's TCD. tcd_next_sga is the OTHER buffer's TCD; on major
// loop end with ESG=1 the hardware TCD is atomically loaded from DLASTSGA.
void build_tcd(DMABaseClass::TCD_t& tcd, uint32_t* buffer,
               DMABaseClass::TCD_t* tcd_next_sga) {
  tcd.SADDR    = buffer;
  tcd.SOFF     = 4;
  tcd.ATTR     = DMA_TCD_ATTR_SSIZE(2)
               | DMA_TCD_ATTR_DSIZE(2)
               | DMA_TCD_ATTR_DMOD(4);
  tcd.NBYTES   = 8;
  tcd.SLAST    = -(int32_t)(kBufSamples * 8u);
  tcd.DADDR    = &IMXRT_FLEXIO2_S.SHIFTBUFBIS[pins::kShifterX];
  tcd.DOFF     = 8;
  tcd.CITER    = kBufSamples;
  tcd.BITER    = kBufSamples;
  tcd.DLASTSGA = (int32_t)tcd_next_sga;
  tcd.CSR      = DMA_TCD_CSR_ESG | DMA_TCD_CSR_INTMAJOR;
}

void setup_dma_pingpong(DMAChannel& ch, uint8_t dma_source) {
  ch.begin(true);
  build_tcd(g_tcd_a, g_frames_a, &g_tcd_b);
  build_tcd(g_tcd_b, g_frames_b, &g_tcd_a);
  *((DMABaseClass::TCD_t*)ch.TCD) = g_tcd_a;
  ch.attachInterrupt(&dma_isr);
  ch.triggerAtHardwareEvent(dma_source);
  ch.enable();
}

const __FlashStringHelper* pattern_name(PatternKind k) {
  switch (k) {
    case PATTERN_SQUARE:   return F("square");
    case PATTERN_TRIANGLE: return F("triangle");
    case PATTERN_CIRCLE:   return F("circle");
    default:               return F("none (static)");
  }
}

}  // namespace

// ---- Public API ---------------------------------------------------------

bool start_clk_sync() {
  uint8_t flex_pin_clk = 0xff;
  g_flex = FlexIOHandler::mapIOPinToFlexIOHandler(pins::kPinClk, flex_pin_clk);
  if (!g_flex || flex_pin_clk != pins::kFlexClk) return false;

  g_flex->setClockSettings(3, 0, 3);

  IMXRT_FLEXIO_t& p = g_flex->port();
  p.CTRL = FLEXIO_CTRL_SWRST;
  p.CTRL = 0;

  // Timer 0: CLK, TIMOD=1 baud, 20-shift burst.
  uint8_t cmp = (uint8_t)((kFlexClockHz / (2u * g_bit_clock_hz)) - 1u);
  p.TIMCMP[pins::kTimerClk] = ((2u * 20u - 1u) << 8) | cmp;
  p.TIMCFG[pins::kTimerClk] = 0;
  p.TIMCTL[pins::kTimerClk] = FLEXIO_TIMCTL_PINCFG(3)
                            | FLEXIO_TIMCTL_PINSEL(pins::kFlexClk)
                            | FLEXIO_TIMCTL_TIMOD(1);

  // Timer 1: SYNC PWM 19/1.
  p.TIMCMP[pins::kTimerSync] = ((2 - 1) << 8) | (38 - 1);
  p.TIMCFG[pins::kTimerSync] = FLEXIO_TIMCFG_TIMDEC(3);
  p.TIMCTL[pins::kTimerSync] = FLEXIO_TIMCTL_TRGSEL(4 * pins::kTimerClk + 3)
                             | FLEXIO_TIMCTL_TRGSRC
                             | FLEXIO_TIMCTL_PINCFG(3)
                             | FLEXIO_TIMCTL_PINSEL(pins::kFlexSync)
                             | FLEXIO_TIMCTL_TIMOD(2);

  // Shifters 0 (X) and 2 (Y). TX, clocked by Timer 0.
  const uint32_t shiftctl_common = FLEXIO_SHIFTCTL_TIMSEL(pins::kTimerClk)
                                 | FLEXIO_SHIFTCTL_TIMPOL
                                 | FLEXIO_SHIFTCTL_PINCFG(3)
                                 | FLEXIO_SHIFTCTL_SMOD(2);
  p.SHIFTCFG[pins::kShifterX] = 0;
  p.SHIFTCTL[pins::kShifterX] = shiftctl_common | FLEXIO_SHIFTCTL_PINSEL(pins::kFlexX);
  p.SHIFTCFG[pins::kShifterY] = 0;
  p.SHIFTCTL[pins::kShifterY] = shiftctl_common | FLEXIO_SHIFTCTL_PINSEL(pins::kFlexY);

  // Prime both buffers with center frames before FlexIO starts.
  uint32_t center_word = encode_shifter_word(kCenterPosition);
  for (uint32_t i = 0; i < kBufSamples; i++) {
    g_frames_a[i * 2 + 0] = center_word;
    g_frames_a[i * 2 + 1] = center_word;
    g_frames_b[i * 2 + 0] = center_word;
    g_frames_b[i * 2 + 1] = center_word;
  }
  arm_dcache_flush(g_frames_a, sizeof(g_frames_a));
  arm_dcache_flush(g_frames_b, sizeof(g_frames_b));

  p.SHIFTBUFBIS[pins::kShifterX] = center_word;
  p.SHIFTBUFBIS[pins::kShifterY] = center_word;

  setup_dma_pingpong(g_dma, DMAMUX_SOURCE_FLEXIO2_REQUEST0);
  g_dma_ready = true;

  // Route the pads to FlexIO2 (ALT4).
  *(portConfigRegister(pins::kPinClk))  = 0x14;
  *(portConfigRegister(pins::kPinSync)) = 0x14;
  *(portConfigRegister(pins::kPinX))    = 0x14;
  *(portConfigRegister(pins::kPinY))    = 0x14;

  p.CTRL = FLEXIO_CTRL_FLEXEN;
  // Only shifter 0 requests DMA; shifter 2 empties in lockstep via shared
  // Timer 0. Single DMA feeds both via destination modulo.
  p.SHIFTSDEN |= (1u << pins::kShifterX);

  g_xy2_running = true;
  return true;
}

void print_buffer_addrs() {
  Serial.print(F("g_frames_a=0x")); Serial.print((uint32_t)&g_frames_a[0], HEX);
  Serial.print(F(".."));            Serial.println((uint32_t)&g_frames_a[kBufSamples * 2 - 1], HEX);
  Serial.print(F("g_frames_b=0x")); Serial.print((uint32_t)&g_frames_b[0], HEX);
  Serial.print(F(".."));            Serial.println((uint32_t)&g_frames_b[kBufSamples * 2 - 1], HEX);
}

void set_drv_oe(bool enable) {
  digitalWrite(pins::kPinDrvOe, enable ? HIGH : LOW);
}

bool get_drv_oe() {
  return digitalRead(pins::kPinDrvOe) != 0;
}

bool set_bit_clock_hz(uint32_t hz) {
  if (hz < 100000 || hz > 2000000) return false;
  uint32_t cmp = kFlexClockHz / (2u * hz);
  if (cmp < 1 || cmp > 256) return false;
  g_bit_clock_hz = hz;
  if (g_flex) {
    g_flex->port().TIMCMP[pins::kTimerClk] = ((2u * 20u - 1u) << 8) | (uint8_t)(cmp - 1u);
  }
  return true;
}

void set_position(uint16_t x, uint16_t y) {
  __disable_irq();
  g_pattern.kind          = PATTERN_NONE;
  g_pattern.total_samples = 1;
  g_pattern.next_sample   = 0;
  g_pattern.static_x      = x;
  g_pattern.static_y      = y;
  __enable_irq();
  full_buffer_refill();
  g_pos_x = x;
  g_pos_y = y;
}

void stop_at_current() {
  // Snapshot the ISR-updated running position (volatile reads), then freeze.
  set_position(g_pos_x, g_pos_y);
}

bool start_square(float side_mm, float speed_mm_s, uint32_t* out_samples) {
  int32_t h = (int32_t)(side_mm * 0.5f * kCountsPerMm);
  if (h < 1 || h > kMaxOffsetCounts) return false;
  float perimeter = 4.0f * side_mm;
  uint32_t n = (uint32_t)(perimeter * kSampleRateHz / speed_mm_s + 0.5f);
  if (n < 4) n = 4;
  uint32_t per_side = n / 4;
  n = per_side * 4;
  __disable_irq();
  g_pattern.kind          = PATTERN_SQUARE;
  g_pattern.total_samples = n;
  g_pattern.next_sample   = 0;
  g_pattern.param1        = h;
  __enable_irq();
  full_buffer_refill();
  if (out_samples) *out_samples = n;
  return true;
}

bool start_triangle(float side_mm, float speed_mm_s, uint32_t* out_samples) {
  int32_t r = (int32_t)((side_mm / sqrtf(3.0f)) * kCountsPerMm);
  if (r < 1 || r > kMaxOffsetCounts) return false;
  float perimeter = 3.0f * side_mm;
  uint32_t n = (uint32_t)(perimeter * kSampleRateHz / speed_mm_s + 0.5f);
  if (n < 3) n = 3;
  uint32_t per_side = n / 3;
  n = per_side * 3;
  __disable_irq();
  g_pattern.kind          = PATTERN_TRIANGLE;
  g_pattern.total_samples = n;
  g_pattern.next_sample   = 0;
  g_pattern.param1        = r;
  __enable_irq();
  full_buffer_refill();
  if (out_samples) *out_samples = n;
  return true;
}

bool start_circle(float dia_mm, float speed_mm_s, uint32_t* out_samples) {
  int32_t r = (int32_t)(dia_mm * 0.5f * kCountsPerMm);
  if (r < 1 || r > kMaxOffsetCounts) return false;
  float perimeter = (float)M_PI * dia_mm;
  uint32_t n = (uint32_t)(perimeter * kSampleRateHz / speed_mm_s + 0.5f);
  if (n < 8) n = 8;
  __disable_irq();
  g_pattern.kind          = PATTERN_CIRCLE;
  g_pattern.total_samples = n;
  g_pattern.next_sample   = 0;
  g_pattern.param1        = r;
  __enable_irq();
  full_buffer_refill();
  if (out_samples) *out_samples = n;
  return true;
}

void cmd_status() {
  Serial.print(F("xy2_running="));
  Serial.println(g_xy2_running ? F("yes") : F("no"));
  Serial.print(F("pin"));
  Serial.print(pins::kPinDrvOe);
  Serial.print(F("_drv_oe="));
  Serial.println(digitalRead(pins::kPinDrvOe) ? F("HIGH") : F("LOW"));
  Serial.print(F("bit_clock_hz="));
  Serial.println(g_bit_clock_hz);
  Serial.print(F("pos_x=0x"));  Serial.print(g_pos_x, HEX);
  Serial.print(F(" pos_y=0x")); Serial.println(g_pos_y, HEX);
  Serial.print(F("pattern.static_x=0x")); Serial.print(g_pattern.static_x, HEX);
  Serial.print(F(" pattern.static_y=0x")); Serial.println(g_pattern.static_y, HEX);
  Serial.print(F("pattern="));
  Serial.println(pattern_name(g_pattern.kind));
  Serial.print(F("pattern_samples="));
  Serial.println(g_pattern.total_samples);
  Serial.print(F("buffer_size=")); Serial.print(kBufSamples);
  Serial.println(F(" (per buffer; 2 buffers ping-pong)"));
  if (g_flex) {
    IMXRT_FLEXIO_t& p = g_flex->port();
    Serial.print(F("FLEXIO2_SHIFTSTAT=0x"));  Serial.println(p.SHIFTSTAT, HEX);
    Serial.print(F("FLEXIO2_SHIFTERR=0x"));   Serial.println(p.SHIFTERR, HEX);
    Serial.print(F("DMA_ERQ=0x"));            Serial.println(DMA_ERQ, HEX);
    Serial.print(F("DMA_ES=0x"));             Serial.println(DMA_ES, HEX);
  }
  if (g_dma_ready) {
    uint32_t citer = g_dma.TCD->CITER;
    uint32_t saddr = (uint32_t)g_dma.TCD->SADDR;
    uint32_t a_lo = (uint32_t)g_frames_a;
    uint32_t a_hi = a_lo + sizeof(g_frames_a);
    bool on_a = (saddr >= a_lo) && (saddr < a_hi);
    uint32_t frame_idx = kBufSamples - citer;
    if (frame_idx >= kBufSamples) frame_idx = 0;
    uint32_t* buf = on_a ? g_frames_a : g_frames_b;
    uint32_t x_word = buf[frame_idx * 2 + 0];
    uint32_t y_word = buf[frame_idx * 2 + 1];
    Serial.print(F("dma active_buf=")); Serial.print(on_a ? F("A") : F("B"));
    Serial.print(F(" citer=")); Serial.print(citer);
    Serial.print(F(" saddr=0x")); Serial.println(saddr, HEX);
    Serial.print(F("isr_count=")); Serial.print(g_isr_count);
    Serial.print(F(" (saw A ")); Serial.print(g_isr_last_saddr_was_a);
    Serial.print(F(", saw B ")); Serial.print(g_isr_last_saddr_was_b);
    Serial.println(F(") -- if both increment, SG ping-pong is working"));
    Serial.print(F("next_frame_idx=")); Serial.println(frame_idx);
    Serial.print(F("ring[idx].x_word=0x")); Serial.print(x_word, HEX);
    Serial.print(F(" y_word=0x"));         Serial.println(y_word, HEX);
  }
}

void cmd_ringstat() {
  struct Bucket { uint32_t x; uint32_t y; uint32_t count; };
  constexpr uint32_t kMaxBuckets = 16;
  Bucket b[kMaxBuckets];
  uint32_t used = 0;
  uint32_t overflow = 0;
  for (int which = 0; which < 2; which++) {
    uint32_t* buf = (which == 0) ? g_frames_a : g_frames_b;
    for (uint32_t i = 0; i < kBufSamples; i++) {
      uint32_t xw = buf[i * 2 + 0];
      uint32_t yw = buf[i * 2 + 1];
      bool found = false;
      for (uint32_t k = 0; k < used; k++) {
        if (b[k].x == xw && b[k].y == yw) { b[k].count++; found = true; break; }
      }
      if (!found) {
        if (used < kMaxBuckets) { b[used].x = xw; b[used].y = yw; b[used].count = 1; used++; }
        else { overflow++; }
      }
    }
  }
  Serial.print(F("ringstat: 16384 frames scanned (A+B), "));
  Serial.print(used); Serial.println(F(" unique (x,y) pair(s):"));
  for (uint32_t k = 0; k < used; k++) {
    Serial.print(F("  count=")); Serial.print(b[k].count);
    Serial.print(F("  x_word=0x")); Serial.print(b[k].x, HEX);
    Serial.print(F(" y_word=0x")); Serial.println(b[k].y, HEX);
  }
  if (overflow > 0) {
    Serial.print(F("  (WARNING: "));
    Serial.print(overflow);
    Serial.println(F(" frames overflowed the 16-bucket table)"));
  }
}

void cmd_sqcheck() {
  if (g_pattern.kind != PATTERN_SQUARE) {
    Serial.println(F("sqcheck: pattern is not SQUARE right now. Set one with 'square <mm>'."));
    return;
  }
  int32_t h = g_pattern.param1;
  constexpr int32_t kTol = 2;
  uint32_t on_perim = 0, off_perim = 0;
  uint32_t first_off_shown = 0;
  uint32_t side_count[4] = {0, 0, 0, 0};
  uint32_t corner_count = 0;
  for (int which = 0; which < 2; which++) {
    uint32_t* buf = (which == 0) ? g_frames_a : g_frames_b;
    for (uint32_t i = 0; i < kBufSamples; i++) {
      uint32_t xw = buf[i * 2 + 0];
      uint32_t yw = buf[i * 2 + 1];
      uint32_t xf20 = xw >> 12;
      uint32_t yf20 = yw >> 12;
      int32_t px = (int32_t)((xf20 >> 1) & 0xFFFF);
      int32_t py = (int32_t)((yf20 >> 1) & 0xFFFF);
      int32_t xo = px - 0x8000;
      int32_t yo = py - 0x8000;
      int32_t ax = xo >= 0 ? xo : -xo;
      int32_t ay = yo >= 0 ? yo : -yo;
      bool on_x_edge = (ax >= h - kTol && ax <= h + kTol) && ay <= h + kTol;
      bool on_y_edge = (ay >= h - kTol && ay <= h + kTol) && ax <= h + kTol;
      if (on_x_edge || on_y_edge) {
        on_perim++;
        if (on_x_edge && on_y_edge) {
          corner_count++;
        } else if (on_y_edge && yo < 0) {
          side_count[0]++;
        } else if (on_x_edge && xo > 0) {
          side_count[1]++;
        } else if (on_y_edge && yo > 0) {
          side_count[2]++;
        } else {
          side_count[3]++;
        }
      } else {
        off_perim++;
        if (first_off_shown < 8) {
          Serial.print(F("  off#")); Serial.print(first_off_shown);
          Serial.print(F(" buf")); Serial.print(which == 0 ? 'A' : 'B');
          Serial.print(F(" frame=")); Serial.print(i);
          Serial.print(F(" pos=(0x")); Serial.print(px, HEX);
          Serial.print(F(", 0x")); Serial.print(py, HEX);
          Serial.print(F(") offset=(")); Serial.print(xo);
          Serial.print(F(", ")); Serial.print(yo);
          Serial.print(F(") vs h=")); Serial.println(h);
          first_off_shown++;
        }
      }
    }
  }
  Serial.print(F("sqcheck: h=")); Serial.print(h);
  Serial.print(F(", on=")); Serial.print(on_perim);
  Serial.print(F(" off=")); Serial.print(off_perim);
  Serial.print(F(" total=")); Serial.println(kBufSamples * 2u);
  Serial.print(F("  bottom=")); Serial.print(side_count[0]);
  Serial.print(F(" right=")); Serial.print(side_count[1]);
  Serial.print(F(" top=")); Serial.print(side_count[2]);
  Serial.print(F(" left=")); Serial.print(side_count[3]);
  Serial.print(F(" corner=")); Serial.println(corner_count);
}

void cmd_patcheck() {
  if (g_pattern.kind != PATTERN_SQUARE) {
    Serial.println(F("patcheck: pattern is not SQUARE right now."));
    return;
  }
  uint32_t total = g_pattern.total_samples;
  int32_t h = g_pattern.param1;
  uint32_t per_side = total / 4;
  Serial.print(F("patcheck: total=")); Serial.print(total);
  Serial.print(F(" per_side=")); Serial.print(per_side);
  Serial.print(F(" h=")); Serial.println(h);
  Serial.print(F("  expected corners: (-h,-h)=("));
  Serial.print(-h); Serial.print(F(",")); Serial.print(-h);
  Serial.print(F("), (+h,-h)=(")); Serial.print(h);
  Serial.print(F(",")); Serial.print(-h);
  Serial.print(F("), (+h,+h)=(")); Serial.print(h);
  Serial.print(F(",")); Serial.print(h);
  Serial.print(F("), (-h,+h)=(")); Serial.print(-h);
  Serial.print(F(",")); Serial.print(h); Serial.println(F(")"));
  struct T { const char* label; uint32_t idx; };
  T tests[] = {
    {"corner BL (idx=0)",              0},
    {"quarter    (idx=per_side/4)",    per_side / 4},
    {"midpoint   (idx=per_side/2)",    per_side / 2},
    {"three-qtr  (idx=3*per_side/4)",  3u * per_side / 4},
    {"corner BR (idx=per_side)",       per_side},
    {"midpoint R (idx=3*per_side/2)",  3u * per_side / 2},
    {"corner TR (idx=2*per_side)",     2u * per_side},
    {"midpoint T (idx=5*per_side/2)",  5u * per_side / 2},
    {"corner TL (idx=3*per_side)",     3u * per_side},
    {"midpoint L (idx=7*per_side/2)",  7u * per_side / 2},
    {"just before wrap (idx=total-1)", total - 1u},
    {"wrap point (idx=total)",         total},
  };
  for (auto& t : tests) {
    uint16_t x, y;
    gen_sample(t.idx, x, y);
    int32_t xo = (int32_t)x - 0x8000;
    int32_t yo = (int32_t)y - 0x8000;
    Serial.print(F("  ")); Serial.print(t.label);
    Serial.print(F("  x=0x")); Serial.print(x, HEX);
    Serial.print(F(" (offset=")); Serial.print(xo);
    Serial.print(F(") y=0x")); Serial.print(y, HEX);
    Serial.print(F(" (offset=")); Serial.print(yo);
    Serial.println(F(")"));
  }
}

void cmd_dumpring(uint32_t n) {
  if (n < 2) n = 2;
  if (n > 512) n = 512;
  Serial.print(F("dumpring: ")); Serial.print(n);
  Serial.println(F(" sample points across A+B (16384 frames total)"));
  uint32_t total = kBufSamples * 2u;
  for (uint32_t j = 0; j < n; j++) {
    uint32_t linear = j * (total / n);
    const char* which = (linear < kBufSamples) ? "A" : "B";
    uint32_t* buf = (linear < kBufSamples) ? g_frames_a : g_frames_b;
    uint32_t local = (linear < kBufSamples) ? linear : (linear - kBufSamples);
    uint32_t xw = buf[local * 2 + 0];
    uint32_t yw = buf[local * 2 + 1];
    Serial.print(F("  buf")); Serial.print(which);
    Serial.print(F(" frame ")); Serial.print(local);
    Serial.print(F(": x_word=0x")); Serial.print(xw, HEX);
    Serial.print(F(" y_word=0x")); Serial.println(yw, HEX);
  }
}

// Kick off the "center -> corner -> center" step test. Returns immediately;
// the two 2 s holds and the state transitions run from testxy_tick(),
// called from loop(). This lets a logic analyzer still trigger on the
// "MARK" prints (which are serialized before their position changes) while
// the console and other background work stay responsive during the holds.
void cmd_testxy(uint16_t corner) {
  if (g_testxy_state != TESTXY_IDLE) {
    Serial.println(F("testxy: already running, ignoring"));
    return;
  }
  Serial.print(F("testxy: center hold 2000 ms, then jump to 0x"));
  Serial.print(corner, HEX);
  Serial.println(F(", 2000 ms, then back to center."));
  set_position(kCenterPosition, kCenterPosition);
  Serial.println(F("MARK center"));
  g_testxy_corner     = corner;
  g_testxy_state      = TESTXY_CENTER;
  g_testxy_entered_ms = millis();
}

void testxy_tick() {
  if (g_testxy_state == TESTXY_IDLE) return;
  if (millis() - g_testxy_entered_ms < kTestxyHoldMs) return;
  switch (g_testxy_state) {
    case TESTXY_CENTER:
      Serial.println(F("MARK jump"));
      set_position(g_testxy_corner, g_testxy_corner);
      g_testxy_state      = TESTXY_CORNER;
      g_testxy_entered_ms = millis();
      break;
    case TESTXY_CORNER:
      Serial.println(F("MARK return"));
      set_position(kCenterPosition, kCenterPosition);
      Serial.println(F("testxy: done"));
      g_testxy_state = TESTXY_IDLE;
      break;
    default:
      g_testxy_state = TESTXY_IDLE;
      break;
  }
}

void monitor_tick() {
  if (!g_mon_enabled || !g_dma_ready || g_flex == nullptr) return;
  uint32_t now = millis();
  if (now - g_mon_last_check_ms < kMonPeriodMs) return;
  g_mon_last_check_ms = now;
  g_mon_ticks++;

  uint32_t es = DMA_ES;
  if (es != g_mon_last_dma_es) {
    Serial.print(F("MON["));
    Serial.print(now);
    Serial.print(F("]: DMA_ES 0x"));
    Serial.print(g_mon_last_dma_es, HEX);
    Serial.print(F(" -> 0x"));
    Serial.println(es, HEX);
    g_mon_last_dma_es = es;
  }

  uint32_t shifterr = g_flex->port().SHIFTERR;
  if (shifterr != g_mon_last_shifterr) {
    Serial.print(F("MON["));
    Serial.print(now);
    Serial.print(F("]: FLEXIO2_SHIFTERR 0x"));
    Serial.print(g_mon_last_shifterr, HEX);
    Serial.print(F(" -> 0x"));
    Serial.println(shifterr, HEX);
    g_mon_last_shifterr = shifterr;
  }

  if (g_pattern.kind == PATTERN_NONE) {
    uint32_t exp_x = encode_shifter_word(g_pattern.static_x);
    uint32_t exp_y = encode_shifter_word(g_pattern.static_y);
    uint32_t bad = 0;
    uint32_t first_bad_idx = 0, first_bad_x = 0, first_bad_y = 0;
    for (uint32_t j = 0; j < 32; j++) {
      uint32_t local = (j % 16) * (kBufSamples / 16);
      uint32_t* buf = (j < 16) ? g_frames_a : g_frames_b;
      uint32_t xw = buf[local * 2 + 0];
      uint32_t yw = buf[local * 2 + 1];
      if (xw != exp_x || yw != exp_y) {
        if (bad == 0) {
          first_bad_idx = (j < 16) ? local : (kBufSamples + local);
          first_bad_x = xw;
          first_bad_y = yw;
        }
        bad++;
      }
    }
    if (bad > 0) {
      g_mon_drift_events++;
      Serial.print(F("MON["));
      Serial.print(now);
      Serial.print(F("]: RING DRIFT during static hold. "));
      Serial.print(bad);
      Serial.println(F("/32 sampled frames wrong."));
      Serial.print(F("  expected x=0x")); Serial.print(exp_x, HEX);
      Serial.print(F(" y=0x")); Serial.println(exp_y, HEX);
      Serial.print(F("  first bad frame=")); Serial.print(first_bad_idx);
      Serial.print(F(" got x=0x")); Serial.print(first_bad_x, HEX);
      Serial.print(F(" y=0x")); Serial.println(first_bad_y, HEX);
      Serial.print(F("  drift_events so far=")); Serial.println(g_mon_drift_events);
      uint32_t citer = g_dma.TCD->CITER;
      uint32_t saddr = (uint32_t)g_dma.TCD->SADDR;
      Serial.print(F("  dma citer=")); Serial.print(citer);
      Serial.print(F(" saddr=0x")); Serial.println(saddr, HEX);
    }
  }
}

void set_monitor_enabled(bool on) {
  g_mon_enabled = on;
}

void cmd_monitor_status() {
  Serial.print(F("monitor="));
  Serial.print(g_mon_enabled ? F("on") : F("off"));
  Serial.print(F(" period_ms=")); Serial.print(kMonPeriodMs);
  Serial.print(F(" ticks=")); Serial.print(g_mon_ticks);
  Serial.print(F(" drift_events=")); Serial.println(g_mon_drift_events);
}

}  // namespace xy2
