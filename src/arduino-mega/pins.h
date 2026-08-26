/*
 * pins.h — Arduino Mega 2560 pin map.
 *
 * Carried across from the old all-in-one firmware
 * (Laser_controller_and_arduino/Arduino_Trimmed_Program). Several of the
 * comments below record hardware faults that were expensive to find. They are
 * kept verbatim, next to the pin they explain, rather than tidied into a table
 * that loses the reason.
 */

#ifndef MEGA_PINS_H
#define MEGA_PINS_H

#include <stdint.h>

/* --- Counts -------------------------------------------------------------- */

static const uint8_t NUM_OXYGEN_SENSORS = 2;
static const uint8_t NUM_TEMP_SENSORS   = 6;
static const uint8_t NUM_SAFETY_PINS    = 11;
static const uint8_t NUM_LIGHT_RELAYS   = 2;

/* --- Process sensing ----------------------------------------------------- */

static const uint8_t PIN_OXYGEN_ANA[NUM_OXYGEN_SENSORS] = {A0, A1};
static const uint8_t PIN_OXYGEN_DIG[NUM_OXYGEN_SENSORS] = {28, 29};

static const uint8_t PIN_TEMP_ANA[NUM_TEMP_SENSORS] = {A2, A3, A4, A5, A6, A7};
// T1=A2(unused, 10kΩ pull-down to GND fitted — floating pin caused ~95°C spikes when O2 heater switched off),
// T2=A3(S2/bed), T3=A4(S4/laser), T4=A5(unused), T5=A6(unused), T6=A7(unused)
static const uint8_t PIN_TEMP_DIG[NUM_TEMP_SENSORS] = {22, 23, 24, 25, 26, 27};

/* The Mega asserts one digital line per sensor into an independent hardware
 * interlock chain, then reads that chain's verdict back on PIN_SAFETY. It is
 * a contributor to the chain, not its master — firmware cannot defeat it. */
static const uint8_t PIN_SAFETY[NUM_SAFETY_PINS] = {33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43};
// 33-34 Door, 35-36 Oxygen, 37-42 Temperature, 43 Green check light

/* --- Motion -------------------------------------------------------------- */

// Motor Control Pins - Set PUL- and DIR- to ground and unplug enable pins
static const uint8_t PIN_FEED_STEP = 9;
static const uint8_t PIN_FEED_DIR  = 8;

static const uint8_t PIN_BED_STEP = 11;
static const uint8_t PIN_BED_DIR  = 10;

static const uint8_t PIN_WIPE_STEP = 49;   // moved from 45 — pins 44/45/46 share Timer 5; 45 was disrupting fan PWM on 46
static const uint8_t PIN_WIPE_DIR  = 12;

static const uint8_t PIN_FEED_ENA = 5;
static const uint8_t PIN_BED_ENA  = 6;
static const uint8_t PIN_WIPE_ENA = 7;

// 3 Motor Limit Switch Pins
static const uint8_t PIN_FEED_LIM = 2; //2
static const uint8_t PIN_BED_LIM  = 3;
static const uint8_t PIN_WIPE_LIM = 4; //4
static const uint8_t PIN_WIPE_LIM2 = 32;

/* No second switch on the pistons; PIN_NONE means "this axis has no limit
 * switch at that end" and the software bound is the only stop. */
static const uint8_t PIN_NONE = 0xFF;

/* --- Airflow ------------------------------------------------------------- */

// Blower Motor Pins
static const uint8_t PIN_BLOWER_PWM = 44; // PWM output to blower motor controller
static const uint8_t PIN_PURGE      = 48; // digital output — argon solenoid valve
static const uint8_t PIN_FAN_PWM    = 46; // PWM output to radiator fan controller

// TODO: Tach input — pin 20 wired to radiator fan tach signal (open-collector, 10kΩ pull-up to 5V).
// INT3 (digitalPinToInterrupt(20)), 2 pulses/rev. Not yet implemented — add attachInterrupt() + pulse counter when RPM feedback is needed.
static const uint8_t PIN_FAN_TACH = 20;

/* --- Chamber lighting ---------------------------------------------------- */

// Light Relay Pins
static const uint8_t PIN_LIGHT_RELAY[NUM_LIGHT_RELAYS] = {30, 31};

#endif /* MEGA_PINS_H */
