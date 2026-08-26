#pragma once

#include <stdint.h>

// Pin map for the Moiren galvo/laser control card v0.1.
// Values are Teensy 4.1 silk-pin numbers (what pinMode/digitalWrite use).
// FlexIO2 indices are the peripheral-side pin numbers used inside the
// FlexIO port register set (TIMCTL/SHIFTCTL PINSEL fields).
//
// Cross-reference: NOTES.md §2 for the full board-side pin table.

namespace pins {

// ---- XY2-100 galvo protocol pins (FlexIO2, ALT4 mux) --------------------
// Silk 10 = B0_00 = FlexIO2:0
// Silk 11 = B0_02 = FlexIO2:2
// Silk 12 = B0_01 = FlexIO2:1
// Silk 13 = B0_03 = FlexIO2:3  -- also LED_BUILTIN. Do NOT digitalWrite this pin.
constexpr uint8_t kPinClk    = 10;
constexpr uint8_t kPinSync   = 12;
constexpr uint8_t kPinX      = 11;
constexpr uint8_t kPinY      = 13;

constexpr uint8_t kFlexClk   = 0;
constexpr uint8_t kFlexSync  = 1;
constexpr uint8_t kFlexX     = 2;
constexpr uint8_t kFlexY     = 3;

constexpr uint8_t kTimerClk  = 0;
constexpr uint8_t kTimerSync = 1;
constexpr uint8_t kShifterX  = 0;
constexpr uint8_t kShifterY  = 2;

// ---- RS-422 driver enable (AM26C31 output enable) -----------------------
// Silk 31. Boots LOW = outputs tri-stated. Board has 10 kΩ pull for safe default.
constexpr uint8_t kPinDrvOe  = 31;

// ---- Laser command outputs (all active-high, isolated via U11 + U6/U17) --
// Boot-safe: all driven LOW as the very first thing in setup().
// MODULATION uniquely passes through U24 which ANDs it with FIRMWARE_ALIVE,
// so even if firmware asserts it, the laser sees nothing until arm() ran.
constexpr uint8_t kPinLcModulation  = 15;
constexpr uint8_t kPinLcControl     = 16;
constexpr uint8_t kPinLcEnable      = 17;
constexpr uint8_t kPinLcRedLight    = 20;
constexpr uint8_t kPinLcFaultReset  = 21;
constexpr uint8_t kPinLcEstop       = 41;

// ---- Laser status inputs (from laser controller, all active-LOW via opto) --
// Idles HIGH (opto not conducting = 3.3 V through pullup), pulled LOW when
// the laser asserts the underlying line.
constexpr uint8_t kPinLsMainPwrFaultN = 2;
constexpr uint8_t kPinLsEmissionN     = 3;
constexpr uint8_t kPinLsFaultN        = 4;
constexpr uint8_t kPinLsReadyN        = 5;
constexpr uint8_t kPinLsFaultRelayN   = 33;

// ---- Laser monitor read-backs (from laser side of I/O, active-LOW via opto) --
// Independent tap on each of the 5 command lines at the DB-44 side, so we
// can confirm what the laser actually sees (loop-through the isolator +
// TBD62783/U17 + laser wiring). Idles HIGH; pulled LOW when line is asserted
// on the laser side.
constexpr uint8_t kPinLmEnableN     = 36;
constexpr uint8_t kPinLmModulationN = 37;
constexpr uint8_t kPinLmInterlockN  = 38;
constexpr uint8_t kPinLmEstopN      = 39;
constexpr uint8_t kPinLmControlN    = 40;

// ---- E-stop chain input + interlock SSR ---------------------------------
// ESTOP_CHAIN_HEALTHY_N: HIGH = chain healthy (all machine-side E-stops
// released, cabinet interlocks made). LOW = chain broken somewhere.
// INTERLOCK_RELAY_EN: HIGH = SSR conducts, shorts DB-44 pins 1-2 (laser
// interlock closed = laser can emit). LOW = SSR open = laser inhibited.
constexpr uint8_t kPinEstopChainHealthyN = 35;
constexpr uint8_t kPinInterlockRelayEn   = 34;

// ---- 0-4 V laser power monitor ADC (ADS1115, shared I2C bus) -----------
// Silk 14 = ADS1115 ALERT/RDY output (active-low by default).
// Not currently used by firmware -- single-shot reads poll the config
// register's OS bit instead. Reserved for future continuous-conversion
// mode where ALERT signals conversion-ready.
// AIN0 = Laser_Power-monitoring_analog_0-4V_output (BFSC pin 9).
// AIN1 tied to GND through R+C (reserved). AIN2, AIN3 tied to GND directly.
constexpr uint8_t kPinAdcAlert = 14;

// ---- 0-10 V laser command DAC (JP1 selects which chip drives the pin) --
// Only one of these is active per build (config.h DAC_USE_SPI):
//
//   SPI path (MCP4921): SPI1 = MOSI1 silk 26, SCK1 silk 27, CS silk 32.
//     REF3025 (2.5 V) + OPA192 4x external gain -> 0-10 V full scale.
//
//   I2C path (GP8211S): Wire = SDA silk 18, SCL silk 19 (shared with ADS1115),
//     default I2C address 0x58 (verify with 'dacscan' if it fails to ACK).
//
// Both chips are physically present on v0.1; JP1 chooses which drives the
// laser input. Compile-time flag must match the hardware jumper.
constexpr uint8_t kPinSpiDacCs = 32;

// ---- Watchdog + arm latch -----------------------------------------------
// Silk 28 -> TPS3820 WDI. Falling edge kicks the watchdog timer. PIT-driven,
// ~50 ms period. Pin idles LOW between kicks.
constexpr uint8_t kPinWdtKick   = 28;

// Silk 29 -> SN74LVC1G74 CLK (arm latch). Rising edge latches D=1 into Q,
// asserting FIRMWARE_ALIVE. D-FF /CLR is wired to TPS3820 /RESET, so Q
// clears on any reset event (watchdog trip, POR, brownout). Firmware never
// deasserts -- only hardware reset does.
constexpr uint8_t kPinArmLatch  = 29;

}  // namespace pins
