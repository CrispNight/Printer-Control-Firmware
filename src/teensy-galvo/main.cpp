#include <Arduino.h>

#include "adc.h"
#include "config.h"
#include "console.h"
#include "dac.h"
#include "field_correction.h"
#include "laser_io.h"
#include "node.h"
#include "pins.h"
#include "watchdog.h"
#include "xy2_engine.h"

// NOTE: pin 13 (LED_BUILTIN on Teensy 4.1) is routed to FlexIO2 as
// GALVO_Y_CHANNEL on the v0.1 PCB. Do NOT call pinMode(LED_BUILTIN, ...)
// or digitalWrite(LED_BUILTIN, ...) anywhere -- doing so overrides the
// FlexIO alt and destroys the Y bit stream. No heartbeat LED on v0.1;
// v0.2 will add a dedicated status LED on an unused GPIO.

void setup() {
  // ---- Safe pin defaults (before anything else can drive them) ----------
  // RS-422 driver enable: outputs tri-stated.
  pinMode(pins::kPinDrvOe, OUTPUT);
  digitalWrite(pins::kPinDrvOe, LOW);

  // All laser command outputs LOW, interlock SSR LOW, laser inputs configured.
  // Must precede anything that could open a laser emission path (e.g. arm()).
  laser_io::init_safe();

  // ---- Start hardware watchdog kicker BEFORE any slow init --------------
  // TPS3820 min timeout is 112 ms. The Serial wait below can take up to
  // 1.5 s. Starting the PIT kicker here keeps WDI happy through the whole
  // bring-up, including DAC probe / FlexIO setup / etc.
  watchdog::init();

  Serial.begin(kSerialBaud);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) {}
  console::print_banner();

  // The protocol and the text console share this one USB port. node::poll()
  // owns the Serial reads and routes each byte; see node.h for why that split
  // is unambiguous rather than a guess.
  node::begin();

  bool xy2_ok = xy2::start_clk_sync();
  if (!xy2_ok) {
    Serial.println(F("ERR: XY2 CLK+SYNC startup failed"));
  } else {
    Serial.print(F("XY2 engine running. Static center streaming. AM26C31 OE (silk "));
    Serial.print(pins::kPinDrvOe);
    Serial.println(F(") = LOW."));
  }
  xy2::print_buffer_addrs();

  field::begin();
  dac::init();
  adc::init();

  // ---- Arm safety chain if critical self-checks passed ------------------
  // Pass 1: only XY2 engine health gates arming (DAC failure is expected
  // on v0.1 until the GP8403->MCP4921 swap in Pass 3). Later passes will
  // add more subsystems to this check.
  if (xy2_ok) {
    watchdog::arm();
    Serial.println(F("safety chain: ARMED (FIRMWARE_ALIVE high, modulation gate open)"));
  } else {
    Serial.println(F("safety chain: NOT ARMED (self-check failed; modulation gate blocked)"));
  }
}

void loop() {
  node::poll();
  node::tick();
  xy2::testxy_tick();
  xy2::monitor_tick();
  dac::tick();
}
