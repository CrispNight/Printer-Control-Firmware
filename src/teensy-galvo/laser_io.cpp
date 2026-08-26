#include "laser_io.h"

#include <Arduino.h>
#include <string.h>

#include "pins.h"

namespace laser_io {

namespace {

// ---- Tables for command outputs -----------------------------------------
// The "alt_name" is the short console token (e.g. `laser mod on`).
struct CommandInfo {
  const char* name;
  const char* alt_name;
  uint8_t     pin;
};

const CommandInfo kCommands[CMD_COUNT] = {
  { "MODULATION",  "mod",     pins::kPinLcModulation },
  { "CONTROL",     "control", pins::kPinLcControl    },
  { "ENABLE",      "enable",  pins::kPinLcEnable     },
  { "RED_LIGHT",   "red",     pins::kPinLcRedLight   },
  { "FAULT_RESET", "reset",   pins::kPinLcFaultReset },
  { "ESTOP",       "estop",   pins::kPinLcEstop      },
};

// ---- Tables for input signals -------------------------------------------
struct SignalInfo {
  const char* name;
  uint8_t     pin;
};

const SignalInfo kStatusInputs[] = {
  { "READY_N",          pins::kPinLsReadyN        },
  { "FAULT_N",          pins::kPinLsFaultN        },
  { "FAULT_RELAY_N",    pins::kPinLsFaultRelayN   },
  { "EMISSION_N",       pins::kPinLsEmissionN     },
  { "MAIN_PWR_FAULT_N", pins::kPinLsMainPwrFaultN },
};
constexpr uint8_t kStatusCount = sizeof(kStatusInputs) / sizeof(kStatusInputs[0]);

const SignalInfo kMonitorInputs[] = {
  { "ENABLE_N",     pins::kPinLmEnableN     },
  { "MODULATION_N", pins::kPinLmModulationN },
  { "INTERLOCK_N",  pins::kPinLmInterlockN  },
  { "ESTOP_N",      pins::kPinLmEstopN      },
  { "CONTROL_N",    pins::kPinLmControlN    },
};
constexpr uint8_t kMonitorsCount = sizeof(kMonitorInputs) / sizeof(kMonitorInputs[0]);

// ---- Small helpers ------------------------------------------------------
bool parse_on_off(const char* s, bool& out) {
  if (!strcmp(s, "on")  || !strcmp(s, "1")) { out = true;  return true; }
  if (!strcmp(s, "off") || !strcmp(s, "0")) { out = false; return true; }
  return false;
}

// Print name column padded to width, then "= <0/1>  (silk NN, <text>)".
void print_signal_line(const char* name, uint8_t width, uint8_t pin, bool active_low) {
  Serial.print(F("  "));
  Serial.print(name);
  for (uint8_t j = strlen(name); j < width; j++) Serial.write(' ');
  bool pin_state = digitalRead(pin) != 0;
  Serial.print(F("= ")); Serial.print(pin_state ? '1' : '0');
  Serial.print(F("  (silk ")); Serial.print(pin);
  if (active_low) {
    Serial.print(F(", "));
    Serial.print(pin_state ? F("deasserted") : F("ASSERTED"));
  }
  Serial.println(F(")"));
}

}  // namespace

// ---- Public API ---------------------------------------------------------

void init_safe() {
  for (uint8_t i = 0; i < CMD_COUNT; i++) {
    pinMode(kCommands[i].pin, OUTPUT);
    digitalWrite(kCommands[i].pin, LOW);
  }
  pinMode(pins::kPinInterlockRelayEn, OUTPUT);
  digitalWrite(pins::kPinInterlockRelayEn, LOW);
  for (uint8_t i = 0; i < kStatusCount; i++) {
    pinMode(kStatusInputs[i].pin, INPUT);
  }
  for (uint8_t i = 0; i < kMonitorsCount; i++) {
    pinMode(kMonitorInputs[i].pin, INPUT);
  }
  pinMode(pins::kPinEstopChainHealthyN, INPUT);
}

void set_command(Command c, bool on) {
  if (c >= CMD_COUNT) return;
  digitalWrite(kCommands[c].pin, on ? HIGH : LOW);
}

bool get_command(Command c) {
  if (c >= CMD_COUNT) return false;
  return digitalRead(kCommands[c].pin) != 0;
}

void set_interlock(bool on) {
  digitalWrite(pins::kPinInterlockRelayEn, on ? HIGH : LOW);
}

bool get_interlock() {
  return digitalRead(pins::kPinInterlockRelayEn) != 0;
}

void cmd_status() {
  Serial.println(F("laser commands (driven by firmware, active-high):"));
  for (uint8_t i = 0; i < CMD_COUNT; i++) {
    Serial.print(F("  "));
    Serial.print(kCommands[i].name);
    for (uint8_t j = strlen(kCommands[i].name); j < 16; j++) Serial.write(' ');
    bool state = digitalRead(kCommands[i].pin) != 0;
    Serial.print(F("= ")); Serial.print(state ? '1' : '0');
    Serial.print(F("  (silk ")); Serial.print(kCommands[i].pin);
    Serial.println(F(")"));
  }

  Serial.println(F("laser status (from controller, _N = active-low):"));
  for (uint8_t i = 0; i < kStatusCount; i++) {
    print_signal_line(kStatusInputs[i].name, 18, kStatusInputs[i].pin, true);
  }

  Serial.println(F("laser monitors (from laser side of I/O, _N = active-low):"));
  for (uint8_t i = 0; i < kMonitorsCount; i++) {
    print_signal_line(kMonitorInputs[i].name, 18, kMonitorInputs[i].pin, true);
  }

  bool chain = digitalRead(pins::kPinEstopChainHealthyN) != 0;
  Serial.print(F("ESTOP_CHAIN_HEALTHY_N = ")); Serial.print(chain ? '1' : '0');
  Serial.print(F("  (silk ")); Serial.print(pins::kPinEstopChainHealthyN);
  Serial.print(F(", chain ")); Serial.print(chain ? F("HEALTHY") : F("BROKEN"));
  Serial.println(F(")"));

  bool interlock = get_interlock();
  Serial.print(F("INTERLOCK_RELAY       = ")); Serial.print(interlock ? '1' : '0');
  Serial.print(F("  (silk ")); Serial.print(pins::kPinInterlockRelayEn);
  Serial.print(F(", SSR ")); Serial.print(interlock ? F("CLOSED") : F("open"));
  Serial.println(F(")"));
}

void cmd(const char* args) {
  // "" or "status" -> full print.
  if (args[0] == '\0' || !strcmp(args, "status")) {
    cmd_status();
    return;
  }
  // "<signal> <on|off>"
  const char* space = strchr(args, ' ');
  if (!space) {
    Serial.println(F("laser: expected 'laser status' or 'laser <sig> <on|off>'"));
    Serial.println(F("       signals: mod, control, enable, red, reset, estop"));
    return;
  }
  char sig[16];
  size_t siglen = space - args;
  if (siglen >= sizeof(sig)) siglen = sizeof(sig) - 1;
  memcpy(sig, args, siglen);
  sig[siglen] = '\0';
  const char* val = space + 1;

  for (uint8_t i = 0; i < CMD_COUNT; i++) {
    if (!strcmp(sig, kCommands[i].alt_name)) {
      bool on = false;
      if (!parse_on_off(val, on)) {
        Serial.println(F("laser: expected 'on' or 'off'"));
        return;
      }
      set_command((Command)i, on);
      Serial.print(F("laser ")); Serial.print(kCommands[i].alt_name);
      Serial.print(F(" -> "));   Serial.print(on ? F("ON  ") : F("OFF "));
      Serial.print(F("(")); Serial.print(kCommands[i].name);
      Serial.print(F(", silk ")); Serial.print(kCommands[i].pin);
      Serial.println(F(")"));
      return;
    }
  }
  Serial.print(F("laser: unknown signal '")); Serial.print(sig); Serial.println(F("'"));
  Serial.println(F("       signals: mod, control, enable, red, reset, estop"));
}

void cmd_interlock(const char* args) {
  bool on = false;
  if (!parse_on_off(args, on)) {
    Serial.println(F("interlock: expected 'on' or 'off'"));
    return;
  }
  set_interlock(on);
  Serial.print(F("interlock -> "));
  Serial.println(on ? F("CLOSED (SSR shorts DB-44 pins 1-2, laser enable path complete)")
                    : F("OPEN (laser inhibited via interlock loop)"));
}

}  // namespace laser_io
