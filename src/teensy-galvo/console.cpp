#include "console.h"
#include "node.h"

#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#include "adc.h"
#include "dac.h"
#include "laser_io.h"
#include "watchdog.h"
#include "xy2_engine.h"

namespace console {

namespace {

constexpr uint8_t kLineBufSize = 80;
constexpr uint8_t kHistorySize = 8;

char    g_line[kLineBufSize];
uint8_t g_line_len = 0;
uint8_t g_line_cursor = 0;      // 0..g_line_len
char    g_history[kHistorySize][kLineBufSize];
uint8_t g_history_count  = 0;   // stored entries, 0..kHistorySize
uint8_t g_history_head   = 0;   // circular slot for next stored entry
int8_t  g_history_cursor = -1;  // -1 = not browsing; else 0=most recent
uint8_t g_esc_state      = 0;   // 0=normal, 1=got ESC, 2=got ESC[, 3=got ESC[<digit>

// ---- Line-editing primitives -------------------------------------------

void clear_line_echo() {
  Serial.write('\r');
  for (uint8_t i = 0; i < g_line_len; i++) Serial.write(' ');
  Serial.write('\r');
}

void cursor_left() {
  if (g_line_cursor > 0) { g_line_cursor--; Serial.write('\b'); }
}

void cursor_right() {
  if (g_line_cursor < g_line_len) { Serial.write(g_line[g_line_cursor]); g_line_cursor++; }
}

void cursor_home() {
  while (g_line_cursor > 0) { g_line_cursor--; Serial.write('\b'); }
}

void cursor_end() {
  while (g_line_cursor < g_line_len) { Serial.write(g_line[g_line_cursor]); g_line_cursor++; }
}

void insert_char(char c) {
  if (g_line_len >= kLineBufSize - 1) return;
  for (uint8_t i = g_line_len; i > g_line_cursor; i--) g_line[i] = g_line[i - 1];
  g_line[g_line_cursor] = c;
  g_line_len++;
  for (uint8_t i = g_line_cursor; i < g_line_len; i++) Serial.write(g_line[i]);
  for (uint8_t i = g_line_len; i > g_line_cursor + 1; i--) Serial.write('\b');
  g_line_cursor++;
}

void backspace() {
  if (g_line_cursor == 0) return;
  for (uint8_t i = g_line_cursor - 1; i < g_line_len - 1; i++) g_line[i] = g_line[i + 1];
  g_line_len--;
  g_line_cursor--;
  Serial.write('\b');
  for (uint8_t i = g_line_cursor; i < g_line_len; i++) Serial.write(g_line[i]);
  Serial.write(' ');
  for (uint8_t i = g_line_len + 1; i > g_line_cursor; i--) Serial.write('\b');
}

void del_at_cursor() {
  if (g_line_cursor >= g_line_len) return;
  for (uint8_t i = g_line_cursor; i < g_line_len - 1; i++) g_line[i] = g_line[i + 1];
  g_line_len--;
  for (uint8_t i = g_line_cursor; i < g_line_len; i++) Serial.write(g_line[i]);
  Serial.write(' ');
  for (uint8_t i = g_line_len + 1; i > g_line_cursor; i--) Serial.write('\b');
}

void store_history(const char* s) {
  if (s[0] == '\0') return;
  strncpy(g_history[g_history_head], s, kLineBufSize - 1);
  g_history[g_history_head][kLineBufSize - 1] = '\0';
  g_history_head = (g_history_head + 1) % kHistorySize;
  if (g_history_count < kHistorySize) g_history_count++;
}

void recall_history(int8_t direction) {
  if (g_history_count == 0) return;
  if (g_history_cursor == -1) {
    if (direction < 0) return;
    g_history_cursor = 0;
  } else {
    int8_t nc = (int8_t)(g_history_cursor + direction);
    if (nc < 0) nc = 0;
    if (nc >= (int8_t)g_history_count) nc = (int8_t)g_history_count - 1;
    g_history_cursor = nc;
  }
  uint8_t idx = (g_history_head + kHistorySize - 1 - g_history_cursor) % kHistorySize;
  cursor_end();
  clear_line_echo();
  strncpy(g_line, g_history[idx], kLineBufSize - 1);
  g_line[kLineBufSize - 1] = '\0';
  g_line_len = (uint8_t)strlen(g_line);
  for (uint8_t i = 0; i < g_line_len; i++) Serial.write(g_line[i]);
  g_line_cursor = g_line_len;
}

// ---- Help text ----------------------------------------------------------

void print_help() {
  Serial.println(F("commands:"));
  Serial.println(F("  help                     - this message"));
  Serial.println(F("  ping                     - reply pong"));
  Serial.println(F("  uptime                   - print millis() since boot"));
  Serial.println(F("  status                   - engine + register state"));
  Serial.println(F("  wdt                      - watchdog + arm-latch status"));
  Serial.println(F("  link                     - protocol link counters + estop state"));
  Serial.println(F("  wdt starve               - stop kicking watchdog (verify FIRMWARE_ALIVE drops)"));
  Serial.println(F("  xy <X> <Y>               - static position (hex or decimal)"));
  Serial.println(F("  center                   - static center (0x8000, 0x8000)"));
  Serial.println(F("  square <mm> [speed]      - trace square, side in mm"));
  Serial.println(F("  triangle <mm> [speed]    - trace equilateral triangle"));
  Serial.println(F("  circle <mm> [speed]      - trace circle by diameter"));
  Serial.println(F("                             speed default = 200 mm/s, range 1..5000"));
  Serial.println(F("  stop                     - halt pattern, hold last position"));
  Serial.println(F("  enable                   - drive AM26C31 OE HIGH (outputs live)"));
  Serial.println(F("  disable                  - drive AM26C31 OE LOW (outputs tri-stated)"));
  Serial.println(F("  clkrate <Hz>             - retune CLK (100000..2000000)"));
  Serial.println(F("  mon [on|off]             - background health monitor toggle/query"));
  Serial.println(F("  ringstat                 - count unique (X,Y) pairs across ring"));
  Serial.println(F("  sqcheck                  - scan every frame, count on/off square perimeter"));
  Serial.println(F("  dumpring [N]             - sample N ring positions (default 16, max 512)"));
  Serial.println(F("  testxy [corner]          - auto-run center->corner->center step test"));
  Serial.println(F("  dac <v>                  - set DAC output (0.0..10.0 V)"));
  Serial.println(F("  dacstep <n>              - step 0->10 V in n increments, 1 s each"));
  Serial.println(F("  dacsweep [sec]           - triangle sweep 0<->10 V, default 20 s period"));
  Serial.println(F("  dacoff                   - DAC to 0 V, stop sequences"));
  Serial.println(F("  dacstat                  - DAC path + last commanded voltage + mode"));
  Serial.println(F("  dacscan                  - I2C bus scan (GP8211S variant only)"));
  Serial.println(F("  powermon                 - read AIN0 laser power monitor (0-4V = 0-100%)"));
  Serial.println(F("  powermon raw             - powermon + raw ADC code + config word"));
  Serial.println(F("  laser                    - print all laser I/O state"));
  Serial.println(F("  laser <sig> <on|off>     - sig: mod control enable red reset estop"));
  Serial.println(F("  interlock <on|off>       - INTERLOCK_RELAY_EN SSR (DB-44 pins 1-2)"));
}

// ---- Command dispatcher -------------------------------------------------

void handle_line(const char* s) {
  if (!strcmp(s, "help")) {
    print_help();
  } else if (!strcmp(s, "ping")) {
    Serial.println(F("pong"));
  } else if (!strcmp(s, "uptime")) {
    Serial.print(F("uptime_ms="));
    Serial.println(millis());
  } else if (!strcmp(s, "status")) {
    xy2::cmd_status();
  } else if (!strcmp(s, "link")) {
    node::cmd_status();
  } else if (!strcmp(s, "wdt")) {
    watchdog::cmd_status();
  } else if (!strcmp(s, "wdt starve")) {
    watchdog::cmd_starve();
  } else if (!strcmp(s, "enable")) {
    xy2::set_drv_oe(true);
    Serial.println(F("!!! AM26C31 OE = HIGH. Outputs live."));
  } else if (!strcmp(s, "disable")) {
    xy2::set_drv_oe(false);
    Serial.println(F("AM26C31 OE = LOW. Outputs tri-stated."));
  } else if (!strcmp(s, "center")) {
    xy2::set_position(0x8000, 0x8000);
    Serial.println(F("pos = 0x8000, 0x8000 (center)"));
  } else if (!strncmp(s, "xy ", 3)) {
    unsigned int xu = 0, yu = 0;
    if (sscanf(s + 3, "%i %i", &xu, &yu) == 2 && xu <= 0xFFFF && yu <= 0xFFFF) {
      xy2::set_position((uint16_t)xu, (uint16_t)yu);
      Serial.print(F("pos = 0x"));  Serial.print(xu, HEX);
      Serial.print(F(", 0x"));       Serial.println(yu, HEX);
    } else {
      Serial.println(F("xy: expected two 16-bit values"));
    }
  } else if (!strncmp(s, "square ", 7)) {
    float mm = 0, speed = xy2::kDefaultSpeedMmS;
    int nargs = sscanf(s + 7, "%f %f", &mm, &speed);
    uint32_t used = 0;
    if (nargs >= 1 && mm > 0 && speed >= xy2::kMinSpeedMmS && speed <= xy2::kMaxSpeedMmS) {
      if (xy2::start_square(mm, speed, &used)) {
        Serial.print(F("square: side="));  Serial.print(mm, 2);
        Serial.print(F(" mm, speed="));     Serial.print(speed, 0);
        Serial.print(F(" mm/s, samples=")); Serial.println(used);
      } else {
        Serial.println(F("square: exceeds field safety limit (~160 mm max side)"));
      }
    } else {
      Serial.println(F("square: expected side_mm [speed_mm_s]"));
    }
  } else if (!strncmp(s, "triangle ", 9)) {
    float mm = 0, speed = xy2::kDefaultSpeedMmS;
    int nargs = sscanf(s + 9, "%f %f", &mm, &speed);
    uint32_t used = 0;
    if (nargs >= 1 && mm > 0 && speed >= xy2::kMinSpeedMmS && speed <= xy2::kMaxSpeedMmS) {
      if (xy2::start_triangle(mm, speed, &used)) {
        Serial.print(F("triangle: side="));  Serial.print(mm, 2);
        Serial.print(F(" mm, speed="));       Serial.print(speed, 0);
        Serial.print(F(" mm/s, samples="));   Serial.println(used);
      } else {
        Serial.println(F("triangle: exceeds field safety limit (~138 mm max side)"));
      }
    } else {
      Serial.println(F("triangle: expected side_mm [speed_mm_s]"));
    }
  } else if (!strncmp(s, "circle ", 7)) {
    float mm = 0, speed = xy2::kDefaultSpeedMmS;
    int nargs = sscanf(s + 7, "%f %f", &mm, &speed);
    uint32_t used = 0;
    if (nargs >= 1 && mm > 0 && speed >= xy2::kMinSpeedMmS && speed <= xy2::kMaxSpeedMmS) {
      if (xy2::start_circle(mm, speed, &used)) {
        Serial.print(F("circle: dia="));    Serial.print(mm, 2);
        Serial.print(F(" mm, speed="));      Serial.print(speed, 0);
        Serial.print(F(" mm/s, samples=")); Serial.println(used);
      } else {
        Serial.println(F("circle: exceeds field safety limit (~160 mm max diameter)"));
      }
    } else {
      Serial.println(F("circle: expected diameter_mm [speed_mm_s]"));
    }
  } else if (!strcmp(s, "stop")) {
    xy2::stop_at_current();
    Serial.println(F("pattern stopped, position held"));
  } else if (!strncmp(s, "clkrate ", 8)) {
    unsigned int hz = 0;
    if (sscanf(s + 8, "%u", &hz) == 1) {
      if (xy2::set_bit_clock_hz(hz)) {
        Serial.print(F("clkrate = ")); Serial.print(hz); Serial.println(F(" Hz"));
      } else {
        Serial.println(F("clkrate: expected 100000..2000000 Hz"));
      }
    } else {
      Serial.println(F("clkrate: expected 100000..2000000 Hz"));
    }
  } else if (!strncmp(s, "testxy", 6)) {
    unsigned int corner = 0x9000;
    if (strlen(s) > 7) { sscanf(s + 7, "%i", &corner); }
    if (corner > 0xFFFF) corner = 0x9000;
    xy2::cmd_testxy((uint16_t)corner);
  } else if (!strncmp(s, "mon", 3)) {
    if (!strcmp(s, "mon on"))       xy2::set_monitor_enabled(true);
    else if (!strcmp(s, "mon off")) xy2::set_monitor_enabled(false);
    xy2::cmd_monitor_status();
  } else if (!strcmp(s, "patcheck")) {
    xy2::cmd_patcheck();
  } else if (!strcmp(s, "sqcheck")) {
    xy2::cmd_sqcheck();
  } else if (!strcmp(s, "ringstat")) {
    xy2::cmd_ringstat();
  } else if (!strncmp(s, "dumpring", 8)) {
    uint32_t n = 16;
    if (strlen(s) > 9) { sscanf(s + 9, "%u", &n); }
    xy2::cmd_dumpring(n);
  } else if (!strcmp(s, "dacscan")) {
    dac::cmd_dacscan();
  } else if (!strncmp(s, "dac ", 4)) {
    dac::cmd_dac(s + 4);
  } else if (!strncmp(s, "dacstep", 7)) {
    dac::cmd_dacstep(s + 7);
  } else if (!strncmp(s, "dacsweep", 8)) {
    dac::cmd_dacsweep(s + 8);
  } else if (!strcmp(s, "dacoff")) {
    dac::cmd_dacoff();
  } else if (!strcmp(s, "dacstat")) {
    dac::cmd_dacstat();
  } else if (!strcmp(s, "powermon")) {
    adc::cmd_powermon();
  } else if (!strcmp(s, "powermon raw")) {
    adc::cmd_powermon_raw();
  } else if (!strncmp(s, "laser ", 6)) {
    laser_io::cmd(s + 6);
  } else if (!strcmp(s, "laser")) {
    laser_io::cmd_status();
  } else if (!strncmp(s, "interlock ", 10)) {
    laser_io::cmd_interlock(s + 10);
  } else if (!strcmp(s, "interlock")) {
    Serial.println(F("interlock: expected 'interlock on' or 'interlock off'"));
  } else if (s[0] == '\0') {
    // empty line -- ignore
  } else {
    Serial.print(F("unknown: "));
    Serial.println(s);
  }
}

}  // namespace

void print_banner() {
  Serial.println();
  Serial.println(F("Moiren galvo/laser control card v0.1 - Teensy 4.1"));
  Serial.println(F("XY2-100 on silk 10/11/12/13. AM26C31 OE (silk 31) boots LOW."));
  Serial.println(F("Type 'help' for commands."));
}

void feed(char c) {
  // ANSI escape sequences: ESC [ A/B/C/D (arrows), H (home), F (end), 3~ (delete).
  if (c == 0x1B) { g_esc_state = 1; return; }
  if (g_esc_state == 1) {
    if (c == '[') { g_esc_state = 2; return; }
    g_esc_state = 0;
    // fall through: ESC followed by non-'[' means process this char normally.
  }
  if (g_esc_state == 2) {
    if (c == 'A') { g_esc_state = 0; recall_history(+1); return; }
    if (c == 'B') { g_esc_state = 0; recall_history(-1); return; }
    if (c == 'C') { g_esc_state = 0; cursor_right(); g_history_cursor = -1; return; }
    if (c == 'D') { g_esc_state = 0; cursor_left();  g_history_cursor = -1; return; }
    if (c == 'H') { g_esc_state = 0; cursor_home();  g_history_cursor = -1; return; }
    if (c == 'F') { g_esc_state = 0; cursor_end();   g_history_cursor = -1; return; }
    if (c == '3') { g_esc_state = 3; return; }  // Delete: ESC[3~
    if (c == '1') { g_esc_state = 3; return; }  // Home (some terms): ESC[1~
    if (c == '4') { g_esc_state = 3; return; }  // End (some terms):  ESC[4~
    g_esc_state = 0;
    return;
  }
  if (g_esc_state == 3) {
    if (c == '~') { del_at_cursor(); g_history_cursor = -1; }
    g_esc_state = 0;
    return;
  }

  // Backspace (BS 0x08) or DEL (0x7F). Different terminals map differently.
  if (c == 0x08 || c == 0x7F) {
    backspace();
    g_history_cursor = -1;
    return;
  }

  if (c == '\r' || c == '\n') {
    Serial.write('\r');
    Serial.write('\n');
    g_line[g_line_len] = '\0';
    store_history(g_line);
    g_history_cursor = -1;
    handle_line(g_line);
    g_line_len = 0;
    g_line_cursor = 0;
    return;
  }

  if (c >= 0x20 && c < 0x7F) {
  insert_char(c);
  g_history_cursor = -1;
  }
}

}  // namespace console
