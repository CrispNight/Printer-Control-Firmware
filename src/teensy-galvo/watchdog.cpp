#include "watchdog.h"

#include <Arduino.h>
#include <IntervalTimer.h>

#include "pins.h"

namespace watchdog {

namespace {

// 50 ms kick period. TPS3820 timeout is 112 ms min / 200 ms typ / 300 ms max
// so this gives >2x margin against the worst-case min timeout.
constexpr uint32_t kKickPeriodUs = 50000;

// Pulse width for the WDI HIGH-to-LOW cycle. TPS3820 requires >=1 µs.
constexpr uint32_t kKickPulseUs = 2;

// Same for the arm-latch CLK rising edge -> LOW cycle. LVC family clock
// input needs only a few ns; 2 µs is comfortably over.
constexpr uint32_t kArmPulseUs = 2;

IntervalTimer     g_timer;
volatile uint32_t g_kick_count = 0;
bool              g_armed      = false;
bool              g_starved    = false;  // set by cmd_starve() -- hardware FIRMWARE_ALIVE is now LOW

// ISR: pulse silk 28 HIGH then LOW. The LOW transition is the falling edge
// that clears the TPS3820 watchdog counter.
void kick_isr() {
  digitalWriteFast(pins::kPinWdtKick, HIGH);
  delayMicroseconds(kKickPulseUs);
  digitalWriteFast(pins::kPinWdtKick, LOW);
  g_kick_count++;
}

}  // namespace

void init() {
  // WDI idles LOW between kicks. Each kick goes LOW -> HIGH -> LOW so the
  // trailing falling edge clears the timer.
  pinMode(pins::kPinWdtKick, OUTPUT);
  digitalWriteFast(pins::kPinWdtKick, LOW);

  // Arm latch CLK must start LOW so the rising edge in arm() is unambiguous
  // and no stray rising edge occurs before self-checks pass. TPS3820 /RESET
  // holds the D-FF cleared during POR, but the pin state still matters for
  // glitch immunity once /RESET releases (~25 ms after VDD stable).
  pinMode(pins::kPinArmLatch, OUTPUT);
  digitalWriteFast(pins::kPinArmLatch, LOW);

  // Start the PIT kicker. First interrupt fires kKickPeriodUs from now,
  // well within TPS3820's 112 ms worst-case window from /RESET release.
  g_timer.begin(kick_isr, kKickPeriodUs);
}

void arm() {
  // Rising edge on CLK latches D=1 into Q.
  digitalWriteFast(pins::kPinArmLatch, HIGH);
  delayMicroseconds(kArmPulseUs);
  digitalWriteFast(pins::kPinArmLatch, LOW);
  g_armed = true;
}

bool is_armed() {
  return g_armed;
}

void cmd_status() {
  Serial.print(F("watchdog kick_period_us=")); Serial.println(kKickPeriodUs);
  Serial.print(F("watchdog kick_count="));     Serial.println(g_kick_count);
  if (g_starved) {
    Serial.println(F("watchdog KICKER STOPPED via 'wdt starve'."));
    Serial.println(F("  FIRMWARE_ALIVE is LOW in hardware regardless of the 'armed' bit below."));
    Serial.println(F("  Press Teensy PROGRAM/RESET or power-cycle to recover."));
  }
  Serial.print(F("watchdog armed (software)="));
  if (g_armed) {
    Serial.println(F("yes (arm() ran this boot)"));
  } else {
    Serial.println(F("no (self-check failed at boot)"));
  }
}

void cmd_starve() {
  Serial.println(F("wdt starve: stopping PIT kicker."));
  Serial.println(F("            TPS3820 will trip in <=300 ms -> FIRMWARE_ALIVE drops LOW."));
  Serial.println(F("            Teensy keeps running (TPS3820 /RESET is not wired to it)."));
  Serial.println(F("            Recovery: Teensy PROGRAM/RESET button or power-cycle."));
  Serial.flush();  // make sure prints reach the host before we lose the kick
  g_timer.end();
  g_starved = true;
  // No return / no re-arm -- FIRMWARE_ALIVE will stay low until a reset.
}

}  // namespace watchdog
