#include "motion.h"

#include <Arduino.h>
#include <AccelStepper.h>
#include <math.h>

#include "config.h"
#include "persist.h"
#include "pins.h"

namespace motion {
namespace {

/* Phases of one axis. Everything the old firmware did inside a blocking while
 * loop is a phase here; service() advances one phase per call. */
enum phase_t {
    PH_IDLE = 0,
    PH_MOVE,               /* running to final_target_ */
    PH_OVERSHOOT,          /* past the target, so the final approach has a
                            * known direction and takes up the backlash */
    PH_HOME_SEEK,          /* driving toward the switch */
    PH_HOME_STUTTER_STOP,  /* decelerating for a stutter back-off */
    PH_HOME_STUTTER_BACK,  /* backing off mid-approach */
    PH_HOME_BACKOFF,       /* backing off after touching the switch */
    PH_HOME_AVERAGE,       /* moving to the averaged switch position */
    PH_HOME_PARK,          /* moving clear of the switch */
    PH_TRACK_SEEK,         /* wiper only: measuring travel against the far switch */
    PH_TRACK_RETURN,
};

/* A seek is "move until the switch", so the target only has to be further than
 * the machine is long. */
const long SEEK_STEPS = 2000000L;

struct axis_t {
    AccelStepper *stepper;
    uint8_t       lim_min_pin;
    uint8_t       lim_max_pin;   /* PIN_NONE when the axis has no far switch */
    float         steps_per_mm;
    uint32_t      def_speed_um_s;
    uint32_t      def_accel_um_s2;
    uint32_t      home_speed_um_s;
    int32_t       home_backoff_um;
    bool          home_stutter;

    int32_t  max_um;            /* measured for the wiper, fixed for the pistons */

    /* Where the axis has been TOLD to be, in exact micrometres. A relative
     * move accumulates here and the step target is derived from it, so the
     * quantising error stays bounded at one step instead of compounding.
     * Going through the reported position instead would lose the remainder
     * every layer: a 30 um layer on the bed is 1.2 steps, and dropping the
     * 0.2 costs 5 um a layer — 5 mm over a thousand-layer print. */
    int32_t  commanded_um;
    long     stutter_steps;     /* precomputed at home(); see PH_HOME_SEEK */
    uint8_t  phase;
    uint8_t  flags;             /* AXIS_FLAG_* */
    bool     limit_fault;
    long     final_target;      /* steps */
    uint8_t  home_sample;
    long     home_pos[3];       /* HOME_SAMPLES */
    long     stutter_mark;
};

AccelStepper feeder(AccelStepper::DRIVER, PIN_FEED_STEP, PIN_FEED_DIR);
AccelStepper bed(AccelStepper::DRIVER, PIN_BED_STEP, PIN_BED_DIR);
AccelStepper wiper(AccelStepper::DRIVER, PIN_WIPE_STEP, PIN_WIPE_DIR);

axis_t axes[AXIS_COUNT] = {
    /* AXIS_FEED */
    {&feeder, PIN_FEED_LIM, PIN_NONE, FEED_STEPS_PER_MM,
     FEED_SPEED_UM_S, FEED_ACCEL_UM_S2, FEED_HOME_SPEED_UM_S,
     FEED_HOME_BACKOFF_UM, true,
     FEED_MAX_UM, 0, 0, PH_IDLE, 0, false, 0, 0, {0, 0, 0}, 0},
    /* AXIS_BED */
    {&bed, PIN_BED_LIM, PIN_NONE, BED_STEPS_PER_MM,
     BED_SPEED_UM_S, BED_ACCEL_UM_S2, BED_HOME_SPEED_UM_S,
     BED_HOME_BACKOFF_UM, true,
     BED_MAX_UM, 0, 0, PH_IDLE, 0, false, 0, 0, {0, 0, 0}, 0},
    /* AXIS_WIPE — the only open-loop axis, and the only one with a far switch */
    {&wiper, PIN_WIPE_LIM, PIN_WIPE_LIM2, WIPE_STEPS_PER_MM,
     WIPE_SPEED_UM_S, WIPE_ACCEL_UM_S2, WIPE_HOME_SPEED_UM_S,
     WIPE_HOME_BACKOFF_UM, false,
     WIPE_MAX_DEFAULT_UM, 0, 0, PH_IDLE, 0, false, 0, 0, {0, 0, 0}, 0},
};

long umToSteps(int32_t um, float steps_per_mm)
{
    return lround((double)um * (double)steps_per_mm * 0.001);
}

int32_t stepsToUm(long steps, float steps_per_mm)
{
    if (steps_per_mm <= 0.0f) return (int32_t)steps;
    return (int32_t)lround((double)steps * 1000.0 / (double)steps_per_mm);
}

float umPerSecToSteps(uint32_t um_s, float steps_per_mm)
{
    return (float)um_s * steps_per_mm * 0.001f;
}

void applySpeed(axis_t &a, uint32_t speed_um_s, uint32_t accel_um_s2)
{
    a.stepper->setMaxSpeed(umPerSecToSteps(speed_um_s, a.steps_per_mm));
    a.stepper->setAcceleration(umPerSecToSteps(accel_um_s2, a.steps_per_mm));
}

/* Stop dead where we are. AccelStepper::setCurrentPosition() also zeroes the
 * target and the running speed, which is exactly the "freeze" the old
 * firmware used at a limit switch. */
void freeze(axis_t &a)
{
    a.stepper->setCurrentPosition(a.stepper->currentPosition());
    /* Wherever the axis actually stopped is now the truth; a stale commanded
     * position would otherwise make the next relative move start from a place
     * the axis never reached. */
    a.commanded_um = stepsToUm(a.stepper->currentPosition(), a.steps_per_mm);
}

bool limitTripped(const axis_t &a, long distance_to_go)
{
    if (distance_to_go < 0 && a.lim_min_pin != PIN_NONE)
        return digitalRead(a.lim_min_pin) == HIGH;
    if (distance_to_go > 0 && a.lim_max_pin != PIN_NONE)
        return digitalRead(a.lim_max_pin) == HIGH;
    return false;
}

bool isHoming(const axis_t &a)
{
    return a.phase >= PH_HOME_SEEK && a.phase <= PH_TRACK_RETURN;
}

/* Software bounds apply only once an axis is homed. Before that its zero means
 * nothing, and clamping against a meaningless zero would block every move —
 * which is precisely why the old firmware shipped with its bounds overrides
 * turned on. Un-homed axes are protected by the switches alone. */
/* Software bounds need a zero to measure from, and AXIS_FLAG_HOMED means "has
 * a usable zero" whether it came from homing this power cycle or from the
 * position store. This is the ONLY protection at the top of the piston travel
 * — those cylinders have a limit switch at the bottom only, and driven far
 * enough up they push the weight out. An axis with no zero at all still gets
 * the switches, and nothing else. */
int32_t clampTarget(const axis_t &a, int32_t target_um)
{
    if (BOUNDS_OVERRIDE) return target_um;
    if (!(a.flags & AXIS_FLAG_HOMED)) return target_um;
    if (target_um < 0) return 0;
    if (target_um > a.max_um) return a.max_um;
    return target_um;
}

uint8_t axisIndexOf(const axis_t &a)
{
    return (uint8_t)(&a - &axes[0]);
}

void beginSeek(axis_t &a)
{
    a.stutter_mark = a.stepper->currentPosition();
    a.stepper->move(-SEEK_STEPS);
    a.phase = PH_HOME_SEEK;
}

void serviceAxis(axis_t &a)
{
    switch (a.phase) {
    case PH_IDLE:
        a.flags &= (uint8_t)~AXIS_FLAG_MOVING;
        return;

    case PH_OVERSHOOT:
    case PH_MOVE: {
        const long togo = a.stepper->distanceToGo();
        if (limitTripped(a, togo)) {
            freeze(a);
            a.flags |= (uint8_t)(AXIS_FLAG_AT_LIMIT | AXIS_FLAG_FAULT);
            /* Nothing outside homing expects to reach a switch. Do NOT
             * re-reference position here: a glitching switch would otherwise
             * silently redefine zero and every later clamp would be wrong. */
            a.limit_fault = true;
            a.phase = PH_IDLE;
            return;
        }
        if (togo == 0) {
            if (a.phase == PH_OVERSHOOT) {
                a.stepper->moveTo(a.final_target);
                a.phase = PH_MOVE;
                return;
            }
            a.phase = PH_IDLE;
            return;
        }
        a.stepper->run();
        return;
    }

    case PH_HOME_SEEK:
        if (digitalRead(a.lim_min_pin) == HIGH) {
            freeze(a);
            a.home_pos[a.home_sample] = a.stepper->currentPosition();
            a.stepper->move(umToSteps(a.home_backoff_um, a.steps_per_mm));
            a.phase = PH_HOME_BACKOFF;
            return;
        }
        if (a.home_stutter) {
            const long travelled = labs(a.stepper->currentPosition() - a.stutter_mark);
            if (travelled >= a.stutter_steps) {
                a.stepper->stop();
                a.phase = PH_HOME_STUTTER_STOP;
                return;
            }
        }
        a.stepper->run();
        return;

    case PH_HOME_STUTTER_STOP:
        if (a.stepper->isRunning()) {
            a.stepper->run();
            return;
        }
        a.stepper->move(umToSteps(STUTTER_BACKOFF_UM, a.steps_per_mm));
        a.phase = PH_HOME_STUTTER_BACK;
        return;

    case PH_HOME_STUTTER_BACK:
        if (a.stepper->distanceToGo() != 0) {
            a.stepper->run();
            return;
        }
        beginSeek(a);
        return;

    case PH_HOME_BACKOFF:
        if (a.stepper->distanceToGo() != 0) {
            a.stepper->run();
            return;
        }
        a.home_sample++;
        if (a.home_sample < HOME_SAMPLES) {
            /* Later passes creep in at half speed, so the averaged switch
             * position is not dominated by the first fast approach. */
            applySpeed(a, a.home_speed_um_s / 2, HOME_ACCEL_UM_S2);
            beginSeek(a);
            return;
        }
        {
            long sum = 0;
            for (uint8_t i = 0; i < HOME_SAMPLES; i++) sum += a.home_pos[i];
            a.stepper->moveTo(sum / (long)HOME_SAMPLES);
        }
        a.phase = PH_HOME_AVERAGE;
        return;

    case PH_HOME_AVERAGE:
        if (a.stepper->distanceToGo() != 0) {
            a.stepper->run();
            return;
        }
        a.stepper->setCurrentPosition(0);
        applySpeed(a, a.def_speed_um_s, a.def_accel_um_s2);
        a.commanded_um = HOME_PARK_UM;
        a.stepper->moveTo(umToSteps(HOME_PARK_UM, a.steps_per_mm));
        a.phase = PH_HOME_PARK;
        return;

    case PH_HOME_PARK:
        if (a.stepper->distanceToGo() != 0) {
            a.stepper->run();
            return;
        }
        a.flags |= AXIS_FLAG_HOMED;
        /* Verified this power cycle, so it is no longer merely believed. */
        a.flags &= (uint8_t)~(AXIS_FLAG_AT_LIMIT | AXIS_FLAG_POS_RESTORED);
        persist::note(axisIndexOf(a), a.commanded_um, true);
        if (a.lim_max_pin != PIN_NONE) {
            /* Measure the real travel rather than trusting a constant — the
             * recoater's usable length is a belt-and-frame property. */
            a.stepper->move(SEEK_STEPS);
            a.phase = PH_TRACK_SEEK;
            return;
        }
        a.phase = PH_IDLE;
        return;

    case PH_TRACK_SEEK:
        if (digitalRead(a.lim_max_pin) == HIGH) {
            freeze(a);
            a.max_um = stepsToUm(a.stepper->currentPosition(), a.steps_per_mm);
            a.commanded_um = HOME_PARK_UM;
            a.stepper->moveTo(umToSteps(HOME_PARK_UM, a.steps_per_mm));
            a.phase = PH_TRACK_RETURN;
            return;
        }
        a.stepper->run();
        return;

    case PH_TRACK_RETURN:
        if (a.stepper->distanceToGo() != 0) {
            a.stepper->run();
            return;
        }
        a.phase = PH_IDLE;
        return;

    default:
        a.phase = PH_IDLE;
        return;
    }
}

}  // namespace

void begin()
{
    if (USE_ENABLE_PIN) {
        pinMode(PIN_FEED_ENA, OUTPUT);
        digitalWrite(PIN_FEED_ENA, HIGH);
        pinMode(PIN_BED_ENA, OUTPUT);
        digitalWrite(PIN_BED_ENA, HIGH);
        pinMode(PIN_WIPE_ENA, OUTPUT);
        digitalWrite(PIN_WIPE_ENA, HIGH);

        feeder.setEnablePin(PIN_FEED_ENA);
        bed.setEnablePin(PIN_BED_ENA);
        wiper.setEnablePin(PIN_WIPE_ENA);

        feeder.enableOutputs();
        bed.enableOutputs();
        wiper.enableOutputs();
    }

    wiper.setPinsInverted(true, false, false);

    pinMode(PIN_FEED_LIM, INPUT);
    pinMode(PIN_BED_LIM, INPUT);
    pinMode(PIN_WIPE_LIM, INPUT);
    pinMode(PIN_WIPE_LIM2, INPUT);

    for (uint8_t i = 0; i < AXIS_COUNT; i++) {
        applySpeed(axes[i], axes[i].def_speed_um_s, axes[i].def_accel_um_s2);
        axes[i].stutter_steps = umToSteps(STUTTER_INTERVAL_UM, axes[i].steps_per_mm);

        /* A position that survived the power cycle gives this axis a usable
         * zero without re-homing, which is the whole point — but it is
         * believed rather than verified, so it is flagged as restored. */
        if (persist::restored(i)) {
            const int32_t pos = persist::restoredPosition_um(i);
            axes[i].commanded_um = pos;
            axes[i].stepper->setCurrentPosition(umToSteps(pos, axes[i].steps_per_mm));
            axes[i].flags |= (uint8_t)(AXIS_FLAG_HOMED | AXIS_FLAG_POS_RESTORED);
        } else {
            axes[i].stepper->setCurrentPosition(0);
            axes[i].commanded_um = 0;
        }
        if (USE_ENABLE_PIN) axes[i].flags |= AXIS_FLAG_ENABLED;
    }
}

void service()
{
    for (uint8_t i = 0; i < AXIS_COUNT; i++) {
        serviceAxis(axes[i]);
        if (axes[i].phase != PH_IDLE) {
            axes[i].flags |= AXIS_FLAG_MOVING;
        } else if (axes[i].flags & AXIS_FLAG_HOMED) {
            /* Cheap when nothing changed; persist.cpp decides when a record is
             * actually worth burning. */
            persist::note(i, axes[i].commanded_um, true);
        }
    }
}

bool fitted(uint8_t axis) { return axis < AXIS_COUNT; }

bool home(uint8_t axis)
{
    if (!fitted(axis)) return false;
    axis_t &a = axes[axis];
    if (a.phase != PH_IDLE) return false;

    a.flags &= (uint8_t)~(AXIS_FLAG_HOMED | AXIS_FLAG_AT_LIMIT | AXIS_FLAG_FAULT |
                          AXIS_FLAG_POS_RESTORED);
    persist::forget(axis);   /* mid-home there is no trustworthy zero to keep */
    a.home_sample = 0;
    applySpeed(a, a.home_speed_um_s, HOME_ACCEL_UM_S2);
    beginSeek(a);
    return true;
}

bool moveTo(uint8_t axis, int32_t target_um, uint32_t speed_um_s,
            uint32_t accel_um_s2, uint8_t flags)
{
    if (!fitted(axis)) return false;
    axis_t &a = axes[axis];
    if (a.phase != PH_IDLE) return false;

    if (flags & AXIS_MOVE_RELATIVE)
        target_um += a.commanded_um;
    target_um = clampTarget(a, target_um);
    a.commanded_um = target_um;

    applySpeed(a, speed_um_s ? speed_um_s : a.def_speed_um_s,
                  accel_um_s2 ? accel_um_s2 : a.def_accel_um_s2);

    a.final_target = umToSteps(target_um, a.steps_per_mm);
    a.flags &= (uint8_t)~(AXIS_FLAG_AT_LIMIT | AXIS_FLAG_FAULT);

    /* Anti-backlash. The flag names the direction of the overshoot, so
     * APPROACH_NEG overshoots low and comes back up, leaving the final
     * approach upward. It is only worth doing when the move would otherwise
     * arrive from the wrong side — a move that already ends travelling the
     * right way takes up the backlash by itself. */
    const long here = a.stepper->currentPosition();
    long overshoot = a.final_target;
    if ((flags & AXIS_MOVE_APPROACH_NEG) && a.final_target < here)
        overshoot = a.final_target - umToSteps(BACKLASH_UM, a.steps_per_mm);
    else if ((flags & AXIS_MOVE_APPROACH_POS) && a.final_target > here)
        overshoot = a.final_target + umToSteps(BACKLASH_UM, a.steps_per_mm);

    if (overshoot != a.final_target) {
        a.stepper->moveTo(overshoot);
        a.phase = PH_OVERSHOOT;
    } else {
        a.stepper->moveTo(a.final_target);
        a.phase = PH_MOVE;
    }
    a.flags |= AXIS_FLAG_MOVING;
    return true;
}

void stop(uint8_t axis)
{
    if (!fitted(axis)) return;
    axis_t &a = axes[axis];
    if (a.phase == PH_IDLE) return;

    a.stepper->stop();                       /* decelerate to a halt */
    a.final_target = a.stepper->targetPosition();
    a.commanded_um = stepsToUm(a.final_target, a.steps_per_mm);
    a.phase = PH_MOVE;
}

void stopAll()
{
    for (uint8_t i = 0; i < AXIS_COUNT; i++) stop(i);
}

void estop()
{
    for (uint8_t i = 0; i < AXIS_COUNT; i++) {
        freeze(axes[i]);
        axes[i].phase = PH_IDLE;
        axes[i].flags &= (uint8_t)~AXIS_FLAG_MOVING;
        /* The drivers' enable lines are unplugged on this machine
         * (USE_ENABLE_PIN false), so there is nothing to de-energise — the
         * steppers simply stop being stepped and hold. */
        if (USE_ENABLE_PIN) {
            axes[i].stepper->disableOutputs();
            axes[i].flags &= (uint8_t)~AXIS_FLAG_ENABLED;
        }
    }
}

bool busy(uint8_t axis)
{
    return fitted(axis) && axes[axis].phase != PH_IDLE;
}

bool anyBusy()
{
    for (uint8_t i = 0; i < AXIS_COUNT; i++)
        if (axes[i].phase != PH_IDLE) return true;
    return false;
}

bool homing(uint8_t axis)
{
    return fitted(axis) && isHoming(axes[axis]);
}

int32_t position_um(uint8_t axis)
{
    if (!fitted(axis)) return 0;
    return stepsToUm(axes[axis].stepper->currentPosition(), axes[axis].steps_per_mm);
}

int32_t target_um(uint8_t axis)
{
    if (!fitted(axis)) return 0;
    return stepsToUm(axes[axis].stepper->targetPosition(), axes[axis].steps_per_mm);
}

uint8_t statusFlags(uint8_t axis)
{
    return fitted(axis) ? axes[axis].flags : 0;
}

int32_t maxTravel_um(uint8_t axis)
{
    return fitted(axis) ? axes[axis].max_um : 0;
}

uint8_t consumeLimitFault()
{
    for (uint8_t i = 0; i < AXIS_COUNT; i++) {
        if (axes[i].limit_fault) {
            axes[i].limit_fault = false;
            return i;
        }
    }
    return AXIS_NONE;
}

}  // namespace motion
