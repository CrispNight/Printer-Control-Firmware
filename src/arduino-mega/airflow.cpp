#include "airflow.h"

#include <Arduino.h>

#include "config.h"
#include "pins.h"
#include "sensors.h"

namespace airflow {
namespace {

struct fan_t {
    uint8_t  pin;
    uint8_t  mode;
    uint16_t duty_pm;
};

fan_t fans[2] = {
    {PIN_BLOWER_PWM, FAN_MODE_OFF, 0},  /* FAN_CHAMBER_BLOWER */
    {PIN_FAN_PWM,    FAN_MODE_OFF, 0},  /* FAN_RADIATOR */
};

/* Stages of the argon purge. See config.h for why the order is what it is. */
enum purge_stage_t {
    PURGE_IDLE = 0,
    PURGE_DISPLACE,  /* solenoid open, blower OFF — argon displaces air */
    PURGE_MIX,       /* solenoid open, blower on  — homogenise */
    PURGE_VERIFY,    /* solenoid shut, blower on  — prove it holds */
};

uint8_t  purge_stage;
bool     valve_open;
uint16_t purge_target_ppm;
uint16_t purge_timeout_s;
uint16_t purge_min_mix_s;
uint32_t purge_start_ms;      /* of the whole purge, for argon accounting */
uint32_t stage_start_ms;
uint16_t purge_open_s;
purge_result_t purge_result;

uint8_t dutyToPwm(uint16_t duty_pm)
{
    if (duty_pm > 1000) duty_pm = 1000;
    return (uint8_t)(((uint32_t)duty_pm * 255UL + 500UL) / 1000UL);
}

void applyFan(fan_t &f)
{
    analogWrite(f.pin, (f.mode == FAN_MODE_MANUAL) ? dutyToPwm(f.duty_pm) : 0);
}

void enterStage(uint8_t stage);
void setValve(bool open);

/* The purge drives the blower directly, without disturbing the mode a caller
 * set — so when the purge finishes, a later FAN_MODE_MANUAL still means what
 * it said. */
void setBlowerDuty(uint16_t duty_pm)
{
    fans[FAN_CHAMBER_BLOWER].duty_pm = duty_pm;
    fans[FAN_CHAMBER_BLOWER].mode = duty_pm ? FAN_MODE_MANUAL : FAN_MODE_OFF;
    analogWrite(fans[FAN_CHAMBER_BLOWER].pin, dutyToPwm(duty_pm));
}

void enterStage(uint8_t stage)
{
    purge_stage = stage;
    stage_start_ms = millis();

    switch (stage) {
    case PURGE_DISPLACE:
        setValve(true);
        setBlowerDuty(0);          /* stirring now would only remix the air */
        break;
    case PURGE_MIX:
        setValve(true);
        setBlowerDuty(BLOWER_PURGE_DUTY_PM);
        break;
    case PURGE_VERIFY:
        purge_open_s = (uint16_t)((millis() - purge_start_ms) / 1000UL);
        setValve(false);
        /* The blower keeps running through the hold, as the old sequence did
         * — its docstring said otherwise but its code never turned it off. */
        break;
    default:
        setValve(false);
        setBlowerDuty(BLOWER_PRINT_DUTY_PM);
        purge_stage = PURGE_IDLE;
        break;
    }
}

bool stageElapsed(uint16_t seconds)
{
    return (millis() - stage_start_ms) >= (uint32_t)seconds * 1000UL;
}

void setValve(bool open)
{
    valve_open = open;
    digitalWrite(PIN_PURGE, open ? HIGH : LOW);
}

}  // namespace

void begin()
{
    for (uint8_t i = 0; i < 2; i++) {
        pinMode(fans[i].pin, OUTPUT);
        analogWrite(fans[i].pin, 0);
        fans[i].mode = FAN_MODE_OFF;
        fans[i].duty_pm = 0;
    }

    pinMode(PIN_PURGE, OUTPUT);
    digitalWrite(PIN_PURGE, LOW);

    // TODO: Tach input — PIN_FAN_TACH is wired but not yet read; fan_status_t
    // reports rpm 0 until attachInterrupt() + a pulse counter land here.

    purge_stage = PURGE_IDLE;
    valve_open = false;
    purge_target_ppm = 0;
    purge_timeout_s = 0;
    purge_open_s = 0;
    purge_result = PURGE_RESULT_NONE;
}

void service()
{
    if (purge_stage == PURGE_IDLE) return;

    const uint16_t o2 = sensors::oxygenWorstPpm();

    switch (purge_stage) {
    case PURGE_DISPLACE:
        if (stageElapsed(PURGE_STAGE1_S)) enterStage(PURGE_MIX);
        return;

    case PURGE_MIX:
        /* The minimum mix time has to pass before the reading is allowed to
         * end the stage — O2 can read low near the sensor long before the
         * chamber is actually homogeneous. */
        if (stageElapsed(purge_min_mix_s) && o2 < purge_target_ppm) {
            enterStage(PURGE_VERIFY);
            return;
        }
        /* Timing out does not abort. The old sequence went to verification
         * anyway and reported what it found, which is the honest thing to do:
         * the hold is what actually decides. */
        if (stageElapsed(purge_timeout_s)) enterStage(PURGE_VERIFY);
        return;

    case PURGE_VERIFY:
        if (o2 >= (uint16_t)(purge_target_ppm + PURGE_HOLD_MARGIN_PPM)) {
            purge_result = PURGE_RESULT_FAILED;
            enterStage(PURGE_IDLE);
            return;
        }
        if (stageElapsed(PURGE_STAGE3_S)) {
            purge_result = PURGE_RESULT_PASSED;
            enterStage(PURGE_IDLE);
        }
        return;

    default:
        enterStage(PURGE_IDLE);
        return;
    }
}

uint8_t setFan(const fan_set_t &req)
{
    if (req.fan > FAN_RADIATOR) return ACK_BAD_PARAM;
    if (req.duty_pm > 1000) return ACK_BAD_PARAM;

    switch (req.mode) {
    case FAN_MODE_OFF:
    case FAN_MODE_MANUAL:
        break;
    case FAN_MODE_MAPPED:
        /* The speed-to-fan map lived here when the Mega drove the marking. It
         * no longer sees scan speed at all. */
        return ACK_REFUSED;
    case FAN_MODE_CLOSEDLOOP:
        return ACK_REFUSED;  /* no flow sensor fitted */
    default:
        return ACK_BAD_PARAM;
    }

    if (req.fan == FAN_CHAMBER_BLOWER && purging()) {
        /* The purge owns the blower while it runs; the stages turn it off and
         * on for a physical reason. */
        return ACK_BUSY;
    }

    fan_t &f = fans[req.fan];
    f.mode = req.mode;
    f.duty_pm = (req.mode == FAN_MODE_MANUAL) ? req.duty_pm : 0;
    applyFan(f);
    return ACK_OK;
}

bool fillStatus(uint8_t fan, fan_status_t &out)
{
    if (fan > FAN_RADIATOR) return false;
    out.fan       = fan;
    out.mode      = fans[fan].mode;
    out.duty_pm   = fans[fan].duty_pm;
    out.rpm       = 0;  /* no tach counter yet */
    out.flow_cm_s = 0;  /* no flow sensor fitted */
    return true;
}

uint8_t setPurge(const purge_set_t &req)
{
    if (!req.enable) {
        /* Abort: shut the valve and stop the blower, as the old abort path
         * did. No result is reported — nothing was concluded. */
        purge_stage = PURGE_IDLE;
        purge_result = PURGE_RESULT_NONE;
        setValve(false);
        setBlowerDuty(0);
        return ACK_OK;
    }
    if (purge_stage != PURGE_IDLE) return ACK_BUSY;

    purge_target_ppm = req.target_o2_ppm ? req.target_o2_ppm : PURGE_TARGET_DEFAULT_PPM;
    purge_timeout_s  = req.timeout_s ? req.timeout_s : PURGE_STAGE2_TIMEOUT_S;
    /* Skipping the mix minimum is a deliberate testing action, never a
     * default: the pockets it exists to clear are what ruin a part. */
    purge_min_mix_s  = (req.flags & PURGE_FLAG_SKIP_MIN_MIX) ? 0 : PURGE_STAGE2_MIN_S;
    purge_result     = PURGE_RESULT_NONE;
    purge_open_s     = 0;
    purge_start_ms   = millis();
    enterStage(PURGE_DISPLACE);
    return ACK_OK;
}

bool purging() { return purge_stage != PURGE_IDLE; }

uint8_t purgeStage() { return purge_stage; }

purge_result_t consumePurgeResult()
{
    const purge_result_t r = purge_result;
    purge_result = PURGE_RESULT_NONE;
    return r;
}

uint16_t lastPurgeOpenSeconds() { return purge_open_s; }

void allOff()
{
    for (uint8_t i = 0; i < 2; i++) {
        fans[i].mode = FAN_MODE_OFF;
        fans[i].duty_pm = 0;
        analogWrite(fans[i].pin, 0);
    }
    purge_stage = PURGE_IDLE;
    purge_result = PURGE_RESULT_NONE;
    setValve(false);
}

}  // namespace airflow
