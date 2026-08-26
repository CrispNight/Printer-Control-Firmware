/*
 * teensy-galvo — Teensy 4.1, galvo + laser control.
 *
 * Intended master of the machine: it will own job state, sequence the Mega's
 * motion and the airflow node, and expose one link to the PC. Today the PC
 * still talks to each board over its own USB port, so this node behaves as a
 * peer; making it the master is a routing change, not a protocol change.
 *
 * STATUS: skeleton. The working galvo/laser implementation lives in the
 * Galvo-Control-Firmware repo and is ported in here next:
 *   TODO(port): galvo DAC output + field correction table -> galvo.cpp/.h
 *   TODO(port): laser source gating, MOPA freq/pulse width -> laser.cpp/.h
 *   TODO(port): vector queue + interpolation timing        -> mark_queue.cpp/.h
 *   TODO(port): correction file loading from SD            -> correction.cpp/.h
 * Nothing here drives hardware yet; the laser stays disarmed.
 */

#include <Arduino.h>

#include "moiren_link.h"
#include "protocol.h"

namespace {

const uint8_t  FW_MAJOR = 0;
const uint8_t  FW_MINOR = 1;
const uint8_t  FW_PATCH = 0;
const uint32_t HEARTBEAT_INTERVAL_MS = 500;

/* USB serial to the PC. Serial1 is reserved for the inter-board link on the
 * next revision of the control card. */
MoirenLink link(Serial, NODE_TEENSY_GALVO);

uint8_t  machine_state = STATE_BOOT;
uint16_t fault_flags   = 0;
uint32_t last_heartbeat_ms = 0;

void sendHello(uint8_t dst)
{
    sys_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.node          = NODE_TEENSY_GALVO;
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

void onFrame(const MoirenFrame &frame, void *)
{
    switch (frame.msg) {
    case MSG_PING:
        link.sendEmpty(frame.src, MSG_PONG, FLAG_IS_RESPONSE);
        break;

    case MSG_HELLO:
        sendHello(frame.src);
        break;

    case MSG_STATE_REQUEST:
        sendState(frame.src);
        break;

    case MSG_ESTOP:
        /* TODO(port): drop the laser gate and galvo enable in hardware here.
         * Must stay the shortest path in this file — nothing may precede it. */
        machine_state = STATE_ESTOP;
        fault_flags |= FAULTBIT_ESTOP;
        link.sendAck(frame, ACK_OK);
        break;

    case MSG_LASER_ARM:
    case MSG_LASER_PARAMS:
    case MSG_MARK_BATCH:
    case MSG_MARK_ABORT:
        /* TODO(port): wire to the galvo/laser implementation. Refused until
         * then so a half-ported build cannot fire the source. */
        link.sendAck(frame, ACK_REFUSED);
        break;

    default:
        link.sendAck(frame, ACK_UNKNOWN_MSG);
        break;
    }
}

}  // namespace

void setup()
{
    Serial.begin(115200);
    link.onFrame(onFrame);
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
        beat.node        = NODE_TEENSY_GALVO;
        beat.state       = machine_state;
        beat.fault_flags = fault_flags;
        beat.uptime_ms   = now;
        link.sendStruct(NODE_BROADCAST, MSG_HEARTBEAT, beat);
    }

    /* TODO(port): service the galvo vector queue here. */
}
