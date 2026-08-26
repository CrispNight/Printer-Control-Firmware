#include "dac.h"

#include <Arduino.h>
#include <stdio.h>

#include "config.h"
#include "pins.h"

#if DAC_USE_SPI
  #include <SPI.h>
#else
  #include <Wire.h>
#endif

namespace dac {

namespace {

// ---- Common constants ---------------------------------------------------
constexpr float kDacFullScaleV = 10.0f;

#if DAC_USE_SPI

// ---- MCP4921 SPI DAC ----------------------------------------------------
// 16-bit write, MSB first, SPI mode 0. Command word format:
//   bit 15: A/B    (channel select -- 0 for the single channel on MCP4921)
//   bit 14: BUF    (Vref input buffer -- 1 for high-Z refs like REF3025)
//   bit 13: /GA    (output gain -- 1 = 1x internal; external OPA192 gives 4x)
//   bit 12: /SHDN  (output enable -- 1 = active)
//   bit 11..0: 12-bit code
// Top nibble is always 0b0111 = 0x7 for our configuration.
constexpr uint16_t kMcpConfigBits = 0x7000;
constexpr uint16_t kDacMaxCode    = 0x0FFF;
constexpr uint32_t kSpiClockHz    = 5000000;  // MCP4921 max 20 MHz; 5 MHz has margin

#else

// ---- GP8211S I2C DAC (from DFRobot_GP8XXX library) ---------------------
// Register 0x01 = output range mode. Write 0x11 for 0-10 V, 0x00 for 0-5 V.
// Register 0x02 = DAC value. 15-bit code left-shifted by 1 to occupy bits
//   15..1 of a 16-bit word, transmitted LSB first.
constexpr uint8_t  kGpAddr        = 0x58;
constexpr uint8_t  kGpRegRange    = 0x01;
constexpr uint8_t  kGpRegDacCh0   = 0x02;
constexpr uint8_t  kGpRange0to10V = 0x11;
constexpr uint16_t kDacMaxCode    = 0x7FFF;

#endif

// ---- Common state -------------------------------------------------------
enum DacMode : uint8_t {
  DAC_IDLE = 0,
  DAC_STEPPING,   // dacstep <n>: step 0 -> 10 V in n increments, 1 s each
  DAC_SWEEPING    // dacsweep [sec]: continuous triangle wave
};

struct DacState {
  DacMode  mode;
  uint32_t start_ms;
  uint32_t last_action_ms;
  uint32_t step_count;
  uint32_t step_current;
  uint32_t sweep_period_ms;
  float    last_volts;
};

bool     g_ready = false;
DacState g_state = { DAC_IDLE, 0, 0, 0, 0xFFFFFFFF, 20000, 0.0f };

void write_code(uint16_t code) {
  if (!g_ready) return;
  if (code > kDacMaxCode) code = kDacMaxCode;
#if DAC_USE_SPI
  uint16_t word = kMcpConfigBits | (code & 0x0FFF);
  SPI1.beginTransaction(SPISettings(kSpiClockHz, MSBFIRST, SPI_MODE0));
  digitalWriteFast(pins::kPinSpiDacCs, LOW);
  SPI1.transfer16(word);
  digitalWriteFast(pins::kPinSpiDacCs, HIGH);
  SPI1.endTransaction();
#else
  uint16_t data = (uint16_t)(code << 1);
  Wire.beginTransmission(kGpAddr);
  Wire.write(kGpRegDacCh0);
  Wire.write((uint8_t)(data & 0xFF));
  Wire.write((uint8_t)((data >> 8) & 0xFF));
  Wire.endTransmission();
#endif
}

}  // namespace

// ---- Public API ---------------------------------------------------------

void set_volts(float volts) {
  if (volts < 0.0f) volts = 0.0f;
  if (volts > kDacFullScaleV) volts = kDacFullScaleV;
  uint16_t code = (uint16_t)(volts * (float)kDacMaxCode / kDacFullScaleV + 0.5f);
  write_code(code);
  g_state.last_volts = volts;
}

void init() {
#if DAC_USE_SPI
  pinMode(pins::kPinSpiDacCs, OUTPUT);
  digitalWriteFast(pins::kPinSpiDacCs, HIGH);  // idle high, deasserted
  SPI1.begin();
  g_ready = true;
  set_volts(0.0f);
  Serial.print(F("dac: MCP4921 SPI ready on SPI1 (CS silk "));
  Serial.print(pins::kPinSpiDacCs);
  Serial.println(F("), output = 0 V"));
#else
  Wire.begin();
  Wire.setClock(400000);
  Wire.beginTransmission(kGpAddr);
  uint8_t status = Wire.endTransmission();
  if (status != 0) {
    Serial.print(F("dac: GP8211S NO ACK at 0x")); Serial.print(kGpAddr, HEX);
    Serial.print(F(" (I2C status=")); Serial.print(status);
    Serial.println(F("). Try 'dacscan' to find the actual address."));
    g_ready = false;
    return;
  }
  g_ready = true;
  Wire.beginTransmission(kGpAddr);
  Wire.write(kGpRegRange);
  Wire.write(kGpRange0to10V);
  Wire.endTransmission();
  set_volts(0.0f);
  Serial.print(F("dac: GP8211S I2C ready at 0x")); Serial.print(kGpAddr, HEX);
  Serial.println(F(", output = 0 V, range 0-10 V"));
#endif
}

void tick() {
  if (!g_ready) return;
  uint32_t now = millis();
  if (g_state.mode == DAC_STEPPING) {
    uint32_t elapsed = now - g_state.start_ms;
    uint32_t step = elapsed / 1000u;
    if (step > g_state.step_count) {
      g_state.mode = DAC_IDLE;
      Serial.println(F("dacstep: done"));
      return;
    }
    if (step != g_state.step_current) {
      g_state.step_current = step;
      float v = kDacFullScaleV * (float)step / (float)g_state.step_count;
      set_volts(v);
      Serial.print(F("dacstep step ")); Serial.print(step);
      Serial.print(F("/"));             Serial.print(g_state.step_count);
      Serial.print(F(" = "));           Serial.print(v, 3);
      Serial.println(F(" V"));
    }
  } else if (g_state.mode == DAC_SWEEPING) {
    if (now - g_state.last_action_ms < 20u) return;
    g_state.last_action_ms = now;
    uint32_t elapsed = (now - g_state.start_ms) % g_state.sweep_period_ms;
    float phase = (float)elapsed / (float)g_state.sweep_period_ms;  // 0..1
    float v = (phase < 0.5f)
              ? (kDacFullScaleV * (2.0f * phase))
              : (kDacFullScaleV * (2.0f - 2.0f * phase));
    set_volts(v);
  }
}

void cmd_dac(const char* args) {
  float volts = -1.0f;
  if (sscanf(args, "%f", &volts) == 1 && volts >= 0.0f && volts <= kDacFullScaleV) {
    g_state.mode = DAC_IDLE;
    set_volts(volts);
    Serial.print(F("dac = ")); Serial.print(volts, 3); Serial.println(F(" V"));
  } else {
    Serial.println(F("dac: expected 'dac <0.0-10.0>'"));
  }
}

void cmd_dacstep(const char* args) {
  int n = 0;
  if (sscanf(args, "%d", &n) == 1 && n >= 1 && n <= 100) {
    g_state.mode = DAC_STEPPING;
    g_state.start_ms = millis();
    g_state.step_count = (uint32_t)n;
    g_state.step_current = 0xFFFFFFFF;
    Serial.print(F("dacstep: 0->10 V in ")); Serial.print(n);
    Serial.print(F(" steps of ")); Serial.print(10.0f / (float)n, 3);
    Serial.print(F(" V, 1 s each. Total ")); Serial.print(n + 1);
    Serial.println(F(" s. Any dac command interrupts."));
  } else {
    Serial.println(F("dacstep: expected 'dacstep <1-100>'"));
  }
}

void cmd_dacsweep(const char* args) {
  int sec = 20;
  sscanf(args, "%d", &sec);
  if (sec < 2)   sec = 2;
  if (sec > 600) sec = 600;
  g_state.mode = DAC_SWEEPING;
  g_state.start_ms = millis();
  g_state.last_action_ms = 0;
  g_state.sweep_period_ms = (uint32_t)sec * 1000u;
  Serial.print(F("dacsweep: triangle 0<->10 V, period ")); Serial.print(sec);
  Serial.println(F(" s. Any dac command interrupts."));
}

void cmd_dacoff() {
  g_state.mode = DAC_IDLE;
  set_volts(0.0f);
  Serial.println(F("dac: 0 V, sequences stopped"));
}

void cmd_dacstat() {
#if DAC_USE_SPI
  Serial.print(F("dac path=MCP4921/SPI1 cs_pin=")); Serial.print(pins::kPinSpiDacCs);
  Serial.print(F(" ready=")); Serial.println(g_ready ? F("yes") : F("no"));
#else
  Serial.print(F("dac path=GP8211S/I2C addr=0x")); Serial.print(kGpAddr, HEX);
  Serial.print(F(" ready=")); Serial.println(g_ready ? F("yes") : F("no"));
#endif
  Serial.print(F("dac last_volts=")); Serial.print(g_state.last_volts, 3);
  Serial.println(F(" V"));
  const char* ms = "idle";
  if      (g_state.mode == DAC_STEPPING) ms = "stepping";
  else if (g_state.mode == DAC_SWEEPING) ms = "sweeping";
  Serial.print(F("dac mode=")); Serial.println(ms);
  if (g_state.mode == DAC_SWEEPING) {
    Serial.print(F("  sweep period ms=")); Serial.println(g_state.sweep_period_ms);
  } else if (g_state.mode == DAC_STEPPING) {
    Serial.print(F("  step "));       Serial.print(g_state.step_current);
    Serial.print(F("/"));             Serial.println(g_state.step_count);
  }
}

void cmd_dacscan() {
#if DAC_USE_SPI
  Serial.println(F("dacscan: SPI DAC selected -- I2C scan not applicable."));
  Serial.println(F("         (rebuild with DAC_USE_SPI=0 to scan for GP8211S)"));
#else
  Serial.println(F("i2c scan (0x01-0x77):"));
  int found = 0;
  for (uint8_t addr = 1; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  ACK at 0x")); Serial.println(addr, HEX);
      found++;
    }
  }
  Serial.print(F("scan: ")); Serial.print(found); Serial.println(F(" device(s)"));
#endif
}

}  // namespace dac
