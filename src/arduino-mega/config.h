/*
 * config.h — machine constants for the Mega.
 *
 * Values carried across from the old firmware. Anything the PC is allowed to
 * change at runtime travels in a protocol message instead and is NOT here;
 * this file is the machine's own geometry, calibration and safety limits.
 *
 * Protocol-facing quantities are integers in micrometres to match protocol.h.
 * Steps-per-mm stay float because AccelStepper is float internally anyway;
 * they are used to set up moves, never on the wire.
 */

#ifndef MEGA_CONFIG_H
#define MEGA_CONFIG_H

#include <stdint.h>

/* --- Compile-time testing aids ------------------------------------------ */

/* Set true to inject fixed test values instead of real sensor readings.
 * Oxygen will appear as TEST_OXYGEN_PPM, all temps as TEST_TEMP_C_X10.
 * Digital safety outputs and the sensor report will all reflect these values.
 *
 * Overrides are deliberately compile-time so they cannot be set by accident,
 * and every one of them is published in MSG_SENSOR_OVERRIDE alongside the true
 * reading so the UI can show both. */
static const bool     SENSOR_TEST_MODE = false;
static const uint16_t TEST_OXYGEN_PPM  = 4000;   /* 0.40 % */
static const int16_t  TEST_TEMP_C_X10  = 250;    /* 25.0 C */

/* Set true to override the O2 reading only (e.g. damaged/unreliable sensor).
 * Temperatures and all other sensors still read live values. */
static const bool     OXYGEN_OVERRIDE     = true;
static const uint16_t OXYGEN_OVERRIDE_PPM = 3500;  /* 0.35 % — value reported while overridden */

/* Set true to bypass software travel limits on a homed axis, for
 * troubleshooting. Hardware limit switches stay active either way.
 *
 * NOTE: software bounds only apply to an axis that has been homed — an
 * un-homed axis has no trustworthy zero, so only the switches protect it. That
 * is why this is false here while the old firmware shipped with the equivalent
 * BED_OVERRIDE/FEED_OVERRIDE/WIPE_OVERRIDE all true: with the pistons never
 * homed, enforcing bounds against a meaningless zero would have blocked every
 * descent. */
static const bool BOUNDS_OVERRIDE = false;

/* The old ZERO command homed only the wiper: the pistons are closed loop and
 * were never homed. Each axis is homed by its own MSG_AXIS_HOME now, so there
 * is no home-everything list here. A "prepare the machine" message that homes
 * what needs homing is a protocol item for later. */

/* Set this true only if you physically wired ENA to the driver. */
static const bool USE_ENABLE_PIN = false;

/* --- ADC ----------------------------------------------------------------- */

static const uint16_t ADC_MAX = 1023;

/* --- Oxygen -------------------------------------------------------------- */

/* Sensor is 1 %O2 per volt, so full scale is 5 % = 50000 ppm — which is why
 * ppm fits a uint16 at all. */
static const uint32_t OXYGEN_PPM_FULL_SCALE = 50000UL;
static const uint16_t OXYGEN_THRESHOLD_PPM  = 20000;  /* 2 % — chain trips above this */

/* --- Temperature --------------------------------------------------------- */

/* Quadratic ADC->degC fits, carried verbatim. Index 2 is S4 (laser, A4);
 * every other channel uses the S2 (bed, A3) equation. */
static const float TEMP_FIT_S4[3] = { 0.0005594681f, -0.5929980825f, 179.6102190468f };
static const float TEMP_FIT_S2[3] = { 0.0000276225f,  0.1189925567f, -56.9439157464f };

/* Per-sensor trim offsets (°C) — adjust to correct systematic error on individual sensors.
 * Order: T1(A2/unused), T2(A3/S2/bed), T3(A4/S4/laser), T4(A5/unused), T5(A6/unused), T6(A7/unused) */
static const float TEMP_TRIM_C[6] = {0.0f, 12.0f, -2.5f, 0.0f, 0.0f, 0.0f};

/* Readings outside this band are implausible on a floating/unused channel and
 * are rejected — this is what stopped heater-switching noise from reading as a
 * 95 C spike. A rejected channel holds its last good value and drops out of
 * sensor_report_t.valid_mask. */
static const int16_t TEMP_MIN_PLAUSIBLE_C_X10 = -400;
static const int16_t TEMP_MAX_PLAUSIBLE_C_X10 = 1200;

/* Which thermistor channels actually have a sensor on them. Only these can be
 * "broken": the unused channels float or sit on a pull-down and read
 * implausibly by design, so faulting on them would fire constantly.
 * Bit per channel, matching sensor_report_t.valid_mask bits 8..13.
 * Fitted today: 1 = A3 (S2, bed), 2 = A4 (S4, laser). */
static const uint8_t TEMP_FITTED_MASK   = 0x06;
static const uint8_t OXYGEN_FITTED_MASK = 0x03;

/* A fitted channel has to read implausibly for this long before it counts as
 * broken, so one noisy conversion does not raise a fault. */
static const uint16_t SENSOR_INVALID_FAULT_MS = 3000;

static const int16_t TEMP_LIMIT_C_X10[6]   = {350, 350, 350, 350, 350, 350};
static const int16_t TEMP_WARN_OFFSET_C_X10 = -50;  /* warn this far below the limit */

/* --- Motion geometry ----------------------------------------------------- */

static const float STEPS_PER_REV      = 200.0f;   // driver microstep setting
static const float WIPE_STEPS_PER_REV = 1600.0f;  // driver microstep setting
static const float FEED_MM_PER_REV    = 5.0f;     // lead screw or belt travel per rev
static const float BED_MM_PER_REV     = 5.0f;
static const float WIPE_MM_PER_REV    = 2.0f * 25.0f;  //GT2 belt with 2.0mm pitch * 25 teeth pulley

static const float FEED_STEPS_PER_MM = STEPS_PER_REV / FEED_MM_PER_REV;
static const float BED_STEPS_PER_MM  = STEPS_PER_REV / BED_MM_PER_REV;
static const float WIPE_STEPS_PER_MM = WIPE_STEPS_PER_REV / WIPE_MM_PER_REV;

/* Default speeds and accelerations, in protocol units. A move may override
 * them; a value of 0 in axis_move_t means "use the default". */
static const uint32_t FEED_SPEED_UM_S = 2000UL;    /* 2 mm/s */
static const uint32_t FEED_ACCEL_UM_S2 = 10000UL;
static const uint32_t BED_SPEED_UM_S  = 2000UL;
static const uint32_t BED_ACCEL_UM_S2 = 10000UL;
static const uint32_t WIPE_SPEED_UM_S = 100000UL;  /* Wipe needs to be fast to minimize recoating time */
static const uint32_t WIPE_ACCEL_UM_S2 = 400000UL;

/* --- Homing -------------------------------------------------------------- */

static const uint32_t HOME_ACCEL_UM_S2 = 100000UL;

static const uint32_t FEED_HOME_SPEED_UM_S = 2000UL;
static const uint32_t BED_HOME_SPEED_UM_S  = 1000UL;
static const uint32_t WIPE_HOME_SPEED_UM_S = 30000UL;

static const int32_t FEED_HOME_BACKOFF_UM = 4000;
static const int32_t BED_HOME_BACKOFF_UM  = 4000;
static const int32_t WIPE_HOME_BACKOFF_UM = 5000;

/* The switch is approached HOME_SAMPLES times and the results averaged; the
 * first pass runs at the homing speed and later passes at half of it. */
static const uint8_t HOME_SAMPLES = 3;

/* Stutter homing: back off periodically on the way in, so a long approach on a
 * piston does not stall against accumulated powder. */
static const int32_t STUTTER_INTERVAL_UM = 20000;
static const int32_t STUTTER_BACKOFF_UM  = 5000;

/* Where an axis parks immediately after homing, clear of the switch. */
static const int32_t HOME_PARK_UM = 5000;

/* --- Travel limits ------------------------------------------------------- */

static const int32_t FEED_MAX_UM = 225000;
static const int32_t BED_MAX_UM  = 215000;
/* The wiper's far limit is measured during homing rather than assumed; this is
 * the value used until then. */
static const int32_t WIPE_MAX_DEFAULT_UM = 300000;

/* Real mechanical backlash compensation, not superstition — an axis asked to
 * approach from one side overshoots by this and comes back. */
static const int32_t BACKLASH_UM = 1000;

/* --- Recoat -------------------------------------------------------------- */

/* Where the sweep starts and ends, in wiper coordinates. 0 is the supply-end
 * limit switch; RECOAT_DISTANCE_UM is the overflow end. */
static const int32_t RECOAT_SUPPLY_PARK_UM = 10000;   /* HOME_OFFSET in the old firmware */
static const int32_t RECOAT_DISTANCE_UM    = 285000;  /* normally 285.0 mm */

/* Pause after the pistons move, before the wiper sweeps. The reason for 2 s is
 * no longer remembered and it costs ~33 minutes over 1000 layers, so it is a
 * parameter on recoat_cycle_t now. Measure before reducing it. */
static const uint16_t RECOAT_SETTLE_DEFAULT_MS = 2000;

/* Pause between the two wiper sweeps of a supply-park cycle. */
static const uint16_t RECOAT_SWEEP_PAUSE_MS = 500;

/* The wiper is the only open-loop axis, so it is the only one that drifts.
 * Re-home it every this many recoat cycles. */
static const uint8_t WIPE_REHOME_INTERVAL = 5;

/* --- Chamber lighting ---------------------------------------------------- */

/* How long to wait after switching lights before a capture is worth taking.
 * The webcam has a physical lens; autofocus, white balance and exposure all
 * need time to adapt. These are the values the PC side had dialled in.
 * Indexed by light_mode_t. */
static const uint16_t LIGHT_SETTLE_DEFAULT_MS[3] = {0, 1500, 1000};

/* --- Purge --------------------------------------------------------------- */

/* The argon purge is three stages, and the order is physical rather than
 * arbitrary. Argon is heavier than air, so stage 1 lets it displace the air by
 * pressure with the blower OFF - stirring at that point would only mix the two
 * back together. Stage 2 then turns the blower on to homogenise what is left.
 * Stage 3 shuts the solenoid and holds, to prove the chamber actually seals
 * rather than merely reaching the number while gas is still flowing in.
 *
 * Carried across from the PC-side purge_system(); these were its constants.
 * The whole thing takes up to ~40 minutes, which is exactly why it must not
 * depend on a host staying connected. */
static const uint16_t PURGE_STAGE1_S        = 480;   /* displace: 5+ min, blower off */
/* Shortest stage 2 before the O2 reading is allowed to end it. Oxygen reads
 * low at the sensor long before the chamber is homogeneous, and the leftover
 * pockets are what ruin a part - so this is a real floor, not padding.
 *
 * This is the DEFAULT only. It wants dialling in per machine and per gas
 * rather than being decided here, so purge_set_t.min_mix_s overrides it and a
 * settings page owns the value. PURGE_FLAG_SKIP_MIN_MIX removes it entirely
 * for testing. */
static const uint16_t PURGE_STAGE2_MIN_S    = 60;
static const uint16_t PURGE_STAGE2_TIMEOUT_S = 1800; /* then move on regardless */
static const uint16_t PURGE_STAGE3_S        = 30;    /* verification hold */

static const uint16_t PURGE_TARGET_DEFAULT_PPM = 3000;  /* 0.30 % — stage 2 aim */

/* Stage 3 fails if O2 climbs back to target + this while the solenoid is shut.
 * The PC used a fixed 0.5 % against a 0.3 % target; expressing it as a margin
 * keeps the pair consistent when the target is changed in the message. */
static const uint16_t PURGE_HOLD_MARGIN_PPM = 2000;

/* Blower duty during and after a purge, per-mille. The PC sent raw PWM: 50 and
 * 94 of 255. */
/* BLOWER_PRINT_DUTY_PM is MEASURED, not guessed: the chamber airflow was swept
 * with an anemometer and PWM 94 was the setting that gave the right velocity
 * over the build area. Every print to date has run at it. Do not "tidy" it to
 * a round number. A venturi flow meter on its own ESP32 will eventually close
 * the loop on this (FAN_MODE_CLOSEDLOOP); until then it is open loop at a
 * value that was verified once. */
static const uint16_t BLOWER_PURGE_DUTY_PM = 196;  /* PWM 50/255 */
static const uint16_t BLOWER_PRINT_DUTY_PM = 369;  /* PWM 94/255 — anemometer-verified */

/* Argon flow with the solenoid open, for the consumption estimate. This is a
 * property of the regulator, not the machine, so it is a stored setting the
 * user can calibrate — this is only the starting value. The old PC controller
 * used 10 L/min with the same caveat. */
static const uint16_t ARGON_FLOW_ML_MIN_DEFAULT = 10000;  /* 10 L/min */

/* --- Position persistence ------------------------------------------------ */

/* Only the closed-loop pistons are persisted. The recoater is open loop, moves
 * several times per layer and has a switch at each end, so it is homed every
 * power cycle instead — which the recoat cycle already requires. Keeping it
 * out of the store also keeps the write rate to roughly one per layer. */
static const uint8_t PERSIST_AXIS_COUNT = 2;   /* AXIS_FEED, AXIS_BED */

/* Settings live above the position ring, in two alternating copies so a power
 * cut during a write cannot destroy the only good one. They change so rarely
 * that endurance is a non-issue. */
static const uint16_t PERSIST_SETTINGS_BASE = 2048;

/* Ring of slots for wear levelling. 128 slots x 14 bytes is 1.75 KB of the
 * Mega's 4 KB EEPROM, and means any one slot is rewritten only once every 128
 * records — at roughly one record per layer that is over 12 million layers
 * against the 100k-cycle endurance. persist.cpp checks the fit at compile
 * time. */
static const uint16_t PERSIST_SLOT_COUNT = 128;

/* Every axis has to be still this long before a record is written. */
static const uint16_t PERSIST_SETTLE_MS = 2000;

/* And records are spaced at least this far apart. A print moves the pistons
 * once a layer with far more than this between, so every layer still gets
 * saved. Jogging during setup does not: a burst of jog commands coalesces into
 * one record, which is all it is worth - a setup position is about to be
 * redone anyway. The cost of the spacing is that an unexpected power cut can
 * lose up to this much movement, which during a print is at most one layer. */
static const uint16_t PERSIST_MIN_INTERVAL_MS = 10000;

/* Changes smaller than this are not worth an EEPROM cycle; this is really just
 * filtering the step/micrometre rounding. */
static const int32_t PERSIST_MIN_DELTA_UM = 5;

/* --- Service pacing ------------------------------------------------------ */

/* One ADC channel is sampled per tick, round-robin, so a sensor sweep never
 * stalls the stepper service for more than a single conversion pair. Eight
 * channels at this interval is a full refresh every 40 ms. */
static const uint16_t SENSOR_SAMPLE_INTERVAL_MS = 5;

static const uint16_t HEARTBEAT_INTERVAL_MS   = 500;
static const uint16_t SENSOR_REPORT_INTERVAL_MS = 1000;
static const uint16_t SAFETY_REPORT_INTERVAL_MS = 1000;
static const uint16_t AXIS_REPORT_INTERVAL_MS   = 100;  /* while an axis is moving */
/* A purge runs for tens of minutes and oxygen moves slowly, so progress every
 * few seconds is plenty; stage changes are published immediately regardless. */
static const uint16_t PURGE_REPORT_INTERVAL_MS  = 5000;

#endif /* MEGA_CONFIG_H */
