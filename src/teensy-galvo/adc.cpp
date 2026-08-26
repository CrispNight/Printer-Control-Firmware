#include "adc.h"

#include <Arduino.h>
#include <Wire.h>

namespace adc {

namespace {

// ---- ADS1115 registers --------------------------------------------------
constexpr uint8_t kAddr      = 0x48;
constexpr uint8_t kRegConv   = 0x00;   // conversion result (RO, 16-bit)
constexpr uint8_t kRegConfig = 0x01;   // config (R/W, 16-bit)

// Config word to trigger a single-shot conversion on AIN0 (single-ended):
//   bit 15    OS       = 1  (start conversion)
//   bits 14:12 MUX     = 100 (AIN0 vs GND, single-ended)
//   bits 11:9 PGA      = 001 (+/-4.096 V full-scale range)
//   bit 8     MODE     = 1  (single-shot)
//   bits 7:5  DR       = 100 (128 SPS -> ~7.8 ms per conversion)
//   bit 4     COMP_MODE = 0 (traditional; unused)
//   bit 3     COMP_POL  = 0 (active low; unused)
//   bit 2     COMP_LAT  = 0 (non-latching; unused)
//   bits 1:0  COMP_QUE  = 11 (comparator disabled, ALERT/RDY high-Z)
constexpr uint16_t kConfigTrigAin0 = 0xC383;

// LSB size at PGA +/-4.096 V, 16-bit signed = 4.096 / 32768 = 125 uV.
constexpr float kVoltsPerLsb = 4.096f / 32768.0f;

// Full-scale voltage of the laser power monitor line (BFSC pin 9): 4 V = 100 %.
constexpr float kMonitorFullScaleV = 4.0f;

// How long to wait for a conversion (128 SPS = ~7.8 ms; give it margin).
constexpr uint32_t kConvTimeoutMs = 20;

bool g_ready = false;

bool write_register(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool read_register(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start
  if (Wire.requestFrom(kAddr, (uint8_t)2) != 2) return false;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  value = ((uint16_t)hi << 8) | lo;
  return true;
}

}  // namespace

void init() {
  Wire.begin();                // idempotent if DAC already called it
  Wire.setClock(400000);
  Wire.beginTransmission(kAddr);
  uint8_t status = Wire.endTransmission();
  if (status != 0) {
    Serial.print(F("adc: ADS1115 NO ACK at 0x")); Serial.print(kAddr, HEX);
    Serial.print(F(" (I2C status=")); Serial.print(status);
    Serial.println(F("). Laser power monitor unavailable."));
    g_ready = false;
    return;
  }
  g_ready = true;
  Serial.print(F("adc: ADS1115 ready at 0x")); Serial.print(kAddr, HEX);
  Serial.println(F(", AIN0 single-shot +/-4.096 V"));
}

bool read_ain0(float& volts, uint16_t* raw_code_out) {
  if (!g_ready) return false;
  if (!write_register(kRegConfig, kConfigTrigAin0)) return false;
  // Poll OS bit (config bit 15): 0 while converting, 1 when done.
  uint32_t deadline = millis() + kConvTimeoutMs;
  uint16_t cfg = 0;
  bool done = false;
  while (millis() < deadline) {
    if (!read_register(kRegConfig, cfg)) return false;
    if (cfg & 0x8000) { done = true; break; }
    delay(1);
  }
  if (!done) return false;
  uint16_t raw = 0;
  if (!read_register(kRegConv, raw)) return false;
  int16_t signed_raw = (int16_t)raw;
  volts = (float)signed_raw * kVoltsPerLsb;
  if (raw_code_out) *raw_code_out = raw;
  return true;
}

void cmd_powermon() {
  float v = 0;
  if (!read_ain0(v)) {
    Serial.println(F("powermon: read failed"));
    return;
  }
  Serial.print(F("powermon: AIN0 = ")); Serial.print(v, 4); Serial.print(F(" V"));
  float pct = (v / kMonitorFullScaleV) * 100.0f;
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  Serial.print(F(" (~")); Serial.print(pct, 1);
  Serial.println(F("% laser power, 0-4V = 0-100%)"));
}

void cmd_powermon_raw() {
  float v = 0;
  uint16_t raw = 0;
  if (!read_ain0(v, &raw)) {
    Serial.println(F("powermon raw: read failed"));
    return;
  }
  Serial.print(F("powermon raw: code=0x")); Serial.print(raw, HEX);
  Serial.print(F(" (signed=")); Serial.print((int16_t)raw);
  Serial.print(F(") volts=")); Serial.print(v, 4);
  Serial.print(F(" pga=+/-4.096V config=0x")); Serial.println(kConfigTrigAin0, HEX);
}

}  // namespace adc
