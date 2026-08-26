/*
 * arduino-mega — Arduino Mega 2560: motion, process sensing, safety interlocks.
 *
 * Also owns airflow today (PWM to the blower controller, mapped from scan
 * speed, no flow sensor). That responsibility moves to the ESP32 node later;
 * see NODE_AIRFLOW in protocol/protocol.h.
 *
 * STATUS: skeleton. The working implementation is
 * Laser_controller_and_arduino/Arduino_Trimmed_Program/Arduino_Trimmed_Program.ino
 * in the old all-in-one repo, and is ported in here next, split by concern:
 *   TODO(port): O2 + thermistor reads, validity/averaging -> sensors.cpp/.h
 *   TODO(port): door/O2/temp interlocks, latch + relays    -> safety.cpp/.h
 *   TODO(port): feed/bed/wipe AccelStepper, homing, limits -> motion.cpp/.h
 *   TODO(port): blower + radiator fan PWM, argon solenoid  -> airflow.cpp/.h
 * Carry the hard-won pin notes across verbatim (the A2 pull-down, WIPE_STEP
 * moved to 49 off Timer 5, the fan tach on 20) — they are the reason those
 * assignments are what they are.
 *
 * Nothing here drives hardware yet: no pin is configured as an output, so a
 * flashed skeleton leaves every driver, relay and solenoid untouched.
 */

#include <Arduino.h>

#include "moiren_link.h"
#include "protocol.h"

namespace {

const uint8_t  FW_MAJOR = 0;
const uint8_t  FW_MINOR = 1;
const uint8_t  FW_PATCH = 0;
const uint32_t HEARTBEAT_INTERVAL_MS = 500;

/* USB serial to the PC today. When the Teensy control card gains inter-board
 * ports this becomes a UART to the Teensy and nothing above changes. */
MoirenLink link(Serial, NODE_ARDUINO_MEGA);

uint8_t  machine_state = STATE_BOOT;
uint16_t fault_flags   = 0;
uint32_t last_heartbeat_ms = 0;

void sendHello(uint8_t dst)
{
    sys_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.node          = NODE_ARDUINO_MEGA;
    hello.proto_version = PROTOCOL_VERSION;
    hello.fw_major      = FW_MAJOR;
    hello.fw_minor      = FW_MINOR;
    hello.fw_patch      = FW_PATCH;
    strncpy(hello.build_id, "dev", sizeof(hello.build_id) - 1);
    link.sendStruct(dst, MSG_HELLO, hello, FLAG_IS_RESPONSE);
}

void sendState(uint8_t dst)
{
    state_report_t state;
    memset(&state, 0, sizeof(state));
    state.state       = machine_state;
    state.fault_flags = fault_flags;
    link.sendStruct(dst, MSG_STATE, state, FLAG_IS_RESPONSE);
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

    case MSG_STATE_REQUEST:
        sendState(packet.src);
        break;

    case MSG_ESTOP:
        /* TODO(port): de-energise the steppers and close the argon solenoid
         * here, ahead of anything else in this handler. */
        machine_state = STATE_ESTOP;
        fault_flags |= FAULTBIT_ESTOP;
        link.sendAck(packet, ACK_OK);
        break;

    case MSG_AXIS_HOME:
    case MSG_AXIS_MOVE:
    case MSG_AXIS_STOP:
    case MSG_RECOAT_CYCLE:
    case MSG_PURGE_SET:
    case MSG_FAN_SET:
        /* TODO(port): dispatch into motion/airflow once ported. Refused until
         * then rather than silently accepted. */
        link.sendAck(packet, ACK_REFUSED);
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
        beat.node        = NODE_ARDUINO_MEGA;
        beat.state       = machine_state;
        beat.fault_flags = fault_flags;
        beat.uptime_ms   = now;
        link.sendStruct(NODE_BROADCAST, MSG_HEARTBEAT, beat);

        /* TODO(port): publish sensor_report_t and safety_status_t on the same
         * tick once sensors.cpp and safety.cpp land. */
    }

    /* TODO(port): run the stepper service loop here (AccelStepper::run()). */
}
