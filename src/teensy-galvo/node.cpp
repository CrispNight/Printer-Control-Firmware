#include "node.h"

#include <Arduino.h>
#include <string.h>

#include "moiren_link.h"
#include "protocol.h"

#include "config.h"
#include "console.h"
#include "field_correction.h"
#include "laser_io.h"
#include "watchdog.h"
#include "xy2_engine.h"

namespace node {
namespace {

const uint8_t FW_MAJOR = 0;
const uint8_t FW_MINOR = 2;
const uint8_t FW_PATCH = 0;

const uint32_t HEARTBEAT_INTERVAL_MS = 500;
const uint32_t STATUS_INTERVAL_MS    = 1000;

MoirenLink link(Serial, NODE_TEENSY_GALVO);

uint8_t  machine_state = STATE_BOOT;
uint16_t fault_flags   = 0;
bool     estop_latched = false;
uint32_t last_heartbeat_ms;
uint32_t last_status_ms;
uint8_t  last_peer = NODE_BROADCAST;
uint32_t last_rx_byte_ms;

/* A packet arrives as one burst, so a gap this long part-way through means the
 * rest is not coming — a host was unplugged, or a cable glitched mid-frame.
 * Without this the decoder would sit waiting for bytes that never arrive and
 * swallow every keystroke typed at the console in the meantime. */
const uint32_t PACKET_GAP_TIMEOUT_MS = 250;

uint8_t laserState()
{
    if (estop_latched) return LASER_STATE_FAULT;
    /* FIRMWARE_ALIVE gates modulation in hardware. Until the arm latch is
     * clocked, no emission is physically possible whatever software believes,
     * so that is the honest thing to report. */
    if (!watchdog::is_armed()) return LASER_STATE_DISARMED;
    return laser_io::get_command(laser_io::CMD_ENABLE) ? LASER_STATE_ARMED
                                                       : LASER_STATE_DISARMED;
}

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
    state_report_t st;
    memset(&st, 0, sizeof(st));
    st.state       = machine_state;
    st.fault_flags = fault_flags;
    link.sendStruct(dst, MSG_STATE, st, FLAG_IS_RESPONSE);
}

/* x_um / y_um stay zero, and that is deliberate rather than unfinished: the
 * galvo engine is currently driven by the console's test patterns, so a
 * position here would be reporting on something this node was never told to
 * do. It becomes real with the marking path, which is also when
 * points_remaining starts meaning anything. */
void sendGalvoStatus(uint8_t dst, uint8_t flags = 0)
{
    galvo_status_t st;
    memset(&st, 0, sizeof(st));
    st.laser_state      = laserState();
    st.points_remaining = 0;
    st.fault_flags      = fault_flags;
    link.sendStruct(dst, MSG_GALVO_STATUS, st, flags);
}

/* Not a stub. Every laser command line goes low, the laser's own E-stop input
 * is asserted, the interlock relay opens, and the beam stops where it stands.
 *
 * The galvo drivers are deliberately left enabled. The beam is off by then, and
 * holding a commanded steady position is safer than tri-stating the RS-422
 * pairs and letting the head drift somewhere unknown. */
void enterEstop()
{
    laser_io::set_command(laser_io::CMD_MODULATION,  false);
    laser_io::set_command(laser_io::CMD_CONTROL,     false);
    laser_io::set_command(laser_io::CMD_ENABLE,      false);
    laser_io::set_command(laser_io::CMD_RED_LIGHT,   false);
    laser_io::set_command(laser_io::CMD_FAULT_RESET, false);
    laser_io::set_command(laser_io::CMD_ESTOP,       true);
    laser_io::set_interlock(false);

    xy2::stop_at_current();

    estop_latched = true;
    machine_state = STATE_ESTOP;
    fault_flags  |= FAULTBIT_ESTOP;
}

void onPacket(const packet_t &packet, void *)
{
    last_peer = packet.src;

    /* First, in every state, before any other work. */
    if (packet.msg == MSG_ESTOP) {
        enterEstop();
        link.sendAck(packet, ACK_OK);
        return;
    }

    if (estop_latched) {
        switch (packet.msg) {
        case MSG_PING:          link.sendEmpty(packet.src, MSG_PONG, FLAG_IS_RESPONSE); return;
        case MSG_HELLO:         sendHello(packet.src); return;
        case MSG_STATE_REQUEST: sendState(packet.src); return;
        case MSG_GALVO_STATUS:  sendGalvoStatus(packet.src, FLAG_IS_RESPONSE); return;
        default:                link.sendAck(packet, ACK_REFUSED); return;
        }
    }

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

    case MSG_GALVO_STATUS:
        sendGalvoStatus(packet.src, FLAG_IS_RESPONSE);
        break;

    case MSG_FAULT_CLEAR:
        /* FAULTBIT_ESTOP is not clearable over the link: the arm latch is
         * cleared by a hardware reset and by nothing else. */
        fault_flags &= (uint16_t)FAULTBIT_ESTOP;
        link.sendAck(packet, ACK_OK);
        break;

    /* --- Not ported yet: refused, never accepted-and-ignored --------------
     * Every one of these reaches the laser or the galvo, and on this machine
     * an ACK_OK reads as "it worked". The console can still drive the
     * hardware directly for bring-up, but that is a human at a terminal who
     * can see what happened, not a sequencer being told a lie. */
    case MSG_LASER_ARM:
    case MSG_LASER_PARAMS:
    case MSG_MARK_ABORT:
    case MSG_TIMING_OFFSET:
    case MSG_JOB_UPLOAD_BEGIN:
    case MSG_JOB_UPLOAD_DATA:
    case MSG_JOB_UPLOAD_END:
    case MSG_JOB_START:
    case MSG_JOB_ABORT:
    case MSG_JOB_PAUSE:
    case MSG_JOB_RESUME:
    case MSG_SETTINGS_REQUEST:
    case MSG_SETTINGS_SET:
        link.sendAck(packet, ACK_REFUSED);
        break;

    /* --- Field correction upload ---------------------------------------- */

    case MSG_FIELD_CORRECTION_BEGIN:
        if (packet.len != sizeof(field_corr_begin_t)) {
            link.sendAck(packet, ACK_BAD_LENGTH);
            break;
        }
        {
            field_corr_begin_t hdr;
            memcpy(&hdr, packet.payload, sizeof(hdr));
            link.sendAck(packet, field::upload_begin(hdr));
        }
        break;

    case MSG_FIELD_CORRECTION_DATA:
        if (packet.len < sizeof(field_corr_data_t)) {
            link.sendAck(packet, ACK_BAD_LENGTH);
            break;
        }
        {
            field_corr_data_t hdr;
            memcpy(&hdr, packet.payload, sizeof(hdr));
            const uint8_t body = (uint8_t)(packet.len - sizeof(field_corr_data_t));
            link.sendAck(packet,
                         field::upload_data(hdr, packet.payload + sizeof(hdr), body));
        }
        break;

    case MSG_FIELD_CORRECTION_END:
        link.sendAck(packet, field::upload_end());
        break;

    default:
        link.sendAck(packet, ACK_UNKNOWN_MSG);
        break;
    }
}

}  // namespace

void begin()
{
    link.onPacket(onPacket);
    last_heartbeat_ms = millis();
    last_status_ms    = millis();
    last_rx_byte_ms   = millis();
    machine_state     = STATE_IDLE;
    sendHello(NODE_BROADCAST);
}

void poll()
{
    /* Bounded, so a host that floods the port cannot starve the galvo engine's
     * health monitor or the console. */
    if (link.receiving() && (millis() - last_rx_byte_ms) > PACKET_GAP_TIMEOUT_MS) {
        link.resetRx();
    }

    uint8_t budget = 128;
    while (budget-- && Serial.available() > 0) {
        const uint8_t b = (uint8_t)Serial.read();
        last_rx_byte_ms = millis();

        /* The link itself says whether it is mid-packet, so this split needs
         * no framing knowledge of its own. 0xA5 cannot be typed, so a byte
         * that opens a packet is never a keystroke. */
        if (link.receiving() || b == PACKET_SOF0) {
            link.feed(b);
        } else {
            console::feed((char)b);
        }
    }
}

void tick()
{
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

    if (now - last_status_ms >= STATUS_INTERVAL_MS) {
        last_status_ms = now;
        sendGalvoStatus(NODE_BROADCAST);
    }
}

bool estopped() { return estop_latched; }

void cmd_status()
{
    Serial.print(F("node "));
    Serial.print(NODE_TEENSY_GALVO);
    Serial.print(F("  proto v"));
    Serial.print(PROTOCOL_VERSION);
    Serial.print(F("  fw "));
    Serial.print(FW_MAJOR); Serial.print('.');
    Serial.print(FW_MINOR); Serial.print('.');
    Serial.println(FW_PATCH);

    Serial.print(F("  packets rx  : ")); Serial.println(link.packetsRx());
    Serial.print(F("  crc errors  : ")); Serial.println(link.crcErrors());
    Serial.print(F("  proto errors: ")); Serial.println(link.protoErrors());
    Serial.print(F("  last peer   : ")); Serial.println(last_peer);
    Serial.print(F("  estop       : "));
    Serial.println(estop_latched ? F("LATCHED") : F("clear"));
    Serial.println(F("  climbing crc errors mean a cable or noise problem, not firmware"));
}

}  // namespace node
