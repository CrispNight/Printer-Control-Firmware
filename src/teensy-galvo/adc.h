#pragma once

#include <stdint.h>

// ADS1115 16-bit ADC on shared I2C bus. Reads AIN0 = the laser controller's
// 0-4 V power monitor output (BFSC DB-44 pin 9). Single-shot conversion mode
// -- the read function triggers, waits for completion (polls OS bit), and
// returns the sample. No continuous-conversion machinery yet.
//
// Default I2C address is 0x48 (ADDR pin tied to GND). If the chip doesn't
// ACK at init, try 'dacscan' (with the I2C DAC variant) or check the schematic
// for a different ADDR wiring (0x49 VDD, 0x4A SDA, 0x4B SCL).

namespace adc {

// Probe I2C, configure for AIN0 single-shot / +/-4.096 V range.
// Prints init result. ADC failure does not gate the safety chain --
// laser can still operate without power monitoring, just without feedback.
void init();

// Trigger a single conversion, wait for completion, read the result.
// volts is AIN0 relative to GND (0-4 V normal range).
// If raw_code_out is non-null, writes the raw 16-bit code there.
// Returns true on success, false on I2C error or ~20 ms timeout.
bool read_ain0(float& volts, uint16_t* raw_code_out = nullptr);

// Console commands.
void cmd_powermon();       // "powermon"      -- volts + laser power %
void cmd_powermon_raw();   // "powermon raw"  -- code + volts + config word

}  // namespace adc
