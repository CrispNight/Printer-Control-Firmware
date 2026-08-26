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

bool     purge_enabled;
bool     valve_open;
uint16_t purge_target_ppm;
uint16_t purge_timeout_s;
uint32_t purge_start_ms;
bool     purge_reached;
bool     purge_timed_out;

uint8_t dutyToPwm(uint16_t duty_pm)
{
    if (duty_pm > 1000) duty_pm = 1000;
    return (uint8_t)(((uint32_t)duty_pm * 255UL + 500UL) / 1000UL);
}

void applyFan(fan_t &f)
{
    analogWrite(f.pin, (f.mode == FAN_MODE_MANUAL) ? dutyToPwm(f.duty_pm) : 0);
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

    purge_enabled = false;
    valve_open = false;
    purge_target_ppm = 0;
    purge_timeout_s = 0;
    purge_reached = false;
    purge_timed_out = false;
}

void service()
{
    if (!purge_enabled) return;

    /* target_o2_ppm of 0 means "just hold the valve open"; there is nothing to
     * close the loop against. */
    if (purge_target_ppm == 0) return;

    const uint16_t o2 = sensors::oxygenWorstPpm();

    if (!purge_reached) {
        if (o2 <= purge_target_ppm) {
            purge_reached = true;
            setValve(false);
            return;
        }
        if (purge_timeout_s != 0 &&
            (millis() - purge_start_ms) >= (uint32_t)purge_timeout_s * 1000UL) {
            purge_timed_out = true;
            purge_enabled = false;
            setValve(false);
        }
        return;
    }

    /* Reached the target once; top up if it drifts back, with hysteresis so
     * the solenoid does not chatter around the setpoint. */
    if (!valve_open && o2 > (uint16_t)(purge_target_ppm + PURGE_HYSTERESIS_PPM))
        setValve(true);
    else if (valve_open && o2 <= purge_target_ppm)
        setValve(false);
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
        purge_enabled = false;
        purge_reached = false;
        setValve(false);
        return ACK_OK;
    }

    purge_enabled    = true;
    purge_target_ppm = req.target_o2_ppm;
    purge_timeout_s  = req.timeout_s;
    purge_start_ms   = millis();
    purge_reached    = false;
    purge_timed_out  = false;
    setValve(true);
    return ACK_OK;
}

bool purging()
{
    return purge_enabled && !purge_reached;
}

bool consumePurgeTimeout()
{
    const bool t = purge_timed_out;
    purge_timed_out = false;
    return t;
}

void allOff()
{
    for (uint8_t i = 0; i < 2; i++) {
        fans[i].mode = FAN_MODE_OFF;
        fans[i].duty_pm = 0;
        analogWrite(fans[i].pin, 0);
    }
    purge_enabled = false;
    purge_reached = false;
    setValve(false);
}

}  // namespace airflow
