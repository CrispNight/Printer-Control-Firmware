/*
 * esp32-fan-esc — ESP32: EDF fan speed control via ESC.
 *
 * STATUS: skeleton, and the hardware does not exist yet. Airflow is handled by
 * the Mega today (PWM to the blower controller, mapped from scan speed, no
 * flow sensor). This node exists so work can start against a fixed protocol;
 * until it is built, address FAN_* messages to NODE_AIRFLOW and they reach
 * whichever board currently owns airflow.
 *
 *   TODO: ESC drive — 50 Hz / OneShot125 on LEDC, with an arming ramp
 *   TODO: startup interlock — refuse to spin unless the chamber is closed
 *   TODO: tach capture for fan_status_t.rpm
 *   TODO: FAN_MODE_CLOSEDLOOP once a flow sensor is fitted (fan_status_t
 *         already carries flow_cm_s; it reports 0 while none is present)
 *   TODO: confirm the module — platformio.ini assumes generic esp32dev
 *
 * Nothing here drives an output: no LEDC channel is attached, so a flashed
 * skeleton cannot spin a motor.
 */

#include <Arduino.h>

#include "moiren_link.h"
#include "protocol.h"

namespace {

const uint8_t  FW_MAJOR = 0;
const uint8_t  FW_MINOR = 1;
const uint8_t  FW_PATCH = 0;
const uint32_t HEARTBEAT_INTERVAL_MS = 500;

MoirenLink link(Serial, NODE_ESP32_FAN);

uint8_t  machine_state = STATE_BOOT;
uint16_t fault_flags   = 0;
uint32_t last_heartbeat_ms = 0;

/* Current commanded airflow. Reported in fan_status_t; not yet applied to an
 * output. This node owns the chamber blower; the radiator fan is defined in
 * the protocol but not wired on the current machine. */
fan_set_t commanded = { FAN_CHAMBER_BLOWER, FAN_MODE_OFF, 0, 0 };

void sendHello(uint8_t dst)
{
    sys_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.node          = NODE_ESP32_FAN;
    hello.proto_version = PROTOCOL_VERSION;
    hello.fw_major      = FW_MAJOR;
    hello.fw_minor      = FW_MINOR;
    hello.fw_patch      = FW_PATCH;
    strncpy(hello.build_id, "dev", sizeof(hello.build_id) - 1);
    link.sendStruct(dst, MSG_HELLO, hello, FLAG_IS_RESPONSE);
}

void sendFanStatus(uint8_t dst)
{
    fan_status_t status;
    memset(&status, 0, sizeof(status));
    status.fan     = commanded.fan;
    status.mode    = commanded.mode;
    status.duty_pm = commanded.duty_pm;
    /* rpm and flow_cm_s stay 0: no tach, no flow sensor. */
    link.sendStruct(dst, MSG_FAN_STATUS, status, FLAG_IS_RESPONSE);
}

void onPacket(const packet_t &packet, void *)
{
    switch (packet.msg) {
    case MSG_PING:
        link.sendEmpty(packet.src, MSG_PONG, FLAG_IS_RESPONSE);
        break;

    case MSG_HELLO:
        sendHello(packet.src);
        break;

    case MSG_ESTOP:
        /* TODO: force the ESC output to idle here, ahead of anything else. */
        commanded.mode    = FAN_MODE_OFF;
        commanded.duty_pm = 0;
        machine_state = STATE_ESTOP;
        fault_flags |= FAULTBIT_ESTOP;
        link.sendAck(packet, ACK_OK);
        break;

    case MSG_FAN_SET:
        if (packet.len < sizeof(fan_set_t)) {
            link.sendAck(packet, ACK_BAD_LENGTH);
            break;
        }
        /* Accepted and recorded, but not applied to an output — a caller can
         * be developed against this node before the hardware exists. */
        memcpy(&commanded, packet.payload, sizeof(fan_set_t));
        link.sendAck(packet, ACK_OK);
        sendFanStatus(packet.src);
        break;

    case MSG_FAN_STATUS:
        sendFanStatus(packet.src);
        break;

    default:
        link.sendAck(packet, ACK_UNKNOWN_MSG);
        break;
    }
}

}  // namespace

void setup()
{
    Serial.begin(115200);
    link.onPacket(onPacket);
    machine_state = STATE_IDLE;
    sendHello(NODE_BROADCAST);
}

void loop()
{
    link.poll();

    const uint32_t now = millis();
    if (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
        last_heartbeat_ms = now;

        sys_heartbeat_t beat;
        beat.node        = NODE_ESP32_FAN;
        beat.state       = machine_state;
        beat.fault_flags = fault_flags;
        beat.uptime_ms   = now;
        link.sendStruct(NODE_BROADCAST, MSG_HEARTBEAT, beat);
    }
}
