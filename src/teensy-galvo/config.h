#pragma once

#include <stdint.h>

// Compile-time configuration for the galvo/laser control card firmware.

// ---- Serial console -----------------------------------------------------
constexpr uint32_t kSerialBaud = 115200;

// ---- XY2-100 engine -----------------------------------------------------
// Boot bit-clock rate. v0.1 PCB has AM26C31 (design target 2 MHz); can be
// retuned at runtime with the `clkrate` command if the head or driver
// misbehaves at speed.
constexpr uint32_t kBootBitClockHz = 2000000;

// XY2-100 frame rate = one frame every 20 bit clocks. Independent of the
// bit-clock rate; sample-generation math uses this for pattern speed.
constexpr float kSampleRateHz = 100000.0f;

// Field scale for RC1001 + 254 mm f-theta lens (~175 mm field).
constexpr float kCountsPerMm = 374.5f;

// Hard safety limit: no pattern point may go more than this many counts
// from center. 30 000 counts is ~80 mm at nominal scale.
constexpr int32_t kMaxOffsetCounts = 30000;

// ---- DAC path selection -------------------------------------------------
// JP1 on the board selects which DAC drives the 0-10 V laser command line:
//   1 = MCP4921 SPI DAC + REF3025 + OPA192 gain stage (default)
//   0 = GP8211S I2C DAC
// Deferred: the actual MCP4921 / GP8211S drivers land in a later pass.
// The current firmware still ships the GP8403 driver from the protoboard;
// on the v0.1 PCB it will fail I2C ACK and go silent, which is fine.
#define DAC_USE_SPI 1
