/*
 * arduino-mega — Arduino Mega 2560: motion, process sensing, safety interlocks.
 *
 * Also owns airflow today (blower PWM and the argon solenoid). That
 * responsibility moves to the ESP32 node later; see NODE_AIRFLOW in
 * protocol/protocol.h.
 *
 * Ported from Laser_controller_and_arduino/Arduino_Trimmed_Program in the old
 * all-in-one repo, restructured rather than translated. Three things about the
 * old firmware were structural and had to change on the way in:
 *
 *   1. Every move ran inside `while (distanceToGo()) { run(); }`, so during a
 *      move the board read no sensors, checked no interlocks and could not
 *      receive an emergency stop. motion.cpp is a state machine now, and this
 *      loop() never blocks.
 *   2. The sensor report ended by reading and discarding everything pending on
 *      the serial port, silently dropping any command that arrived while it
 *      was talking. Every received byte goes to the packet decoder here.
 *   3. The PC held the build and supply positions and sent absolute targets,
 *      so the machine structurally could not run without a host. The Mega owns
 *      its positions and publishes them as axis_status_t.
 *
 * Motion and recoat commands are acknowledged when the work FINISHES, not when
 * it is accepted — that is what the old `DONE|TASK=...` line meant, and it is
 * the answer a caller actually needs. Everything else acks immediately.
 */

#include <Arduino.h>
#include <string.h>

#include "moiren_link.h"
#include "protocol.h"

#include "airflow.h"
#include "config.h"
#include "lighting.h"
#include "motion.h"
#include "persist.h"
#include "pins.h"
#include "recoat.h"
#include "safety.h"
#include "sensors.h"

namespace {

const uint8_t FW_MAJOR = 0;
const uint8_t FW_MINOR = 2;
const uint8_t FW_PATCH = 0;

/* USB serial to the PC today. When the Teensy control card gains inter-board
 * ports this becomes a UART to the Teensy and nothing above changes. */
MoirenLink link(Serial, NODE_ARDUINO_MEGA);

uint8_t  machine_state = STATE_BOOT;

/* Two pictures, deliberately. `fault_flags` is what is wrong right now, and it
 * includes live interlock trips — an open door is reported, but it clears
 * itself when the door shuts. `latched_flags` is the subset that needs
 * MSG_FAULT_CLEAR to go away, and only that subset puts the board in
 * STATE_FAULT. Treating an open door as a latched fault would mean a clear
 * after every powder load. */
uint16_t fault_flags   = 0;
uint16_t latched_flags = 0;

uint32_t last_heartbeat_ms;
uint32_t last_sensor_report_ms;
uint32_t last_safety_report_ms;
uint32_t last_axis_report_ms;
uint16_t last_warn_mask;
uint16_t last_tripped_mask;
uint16_t last_link_errors;

/* Report link errors every this many, rather than every one. */
const uint16_t LINK_ERROR_REPORT_STEP = 16;

/* A command whose ACK is owed once the work it started completes. */
struct pending_t {
    bool    active;
    uint8_t dst;
    uint8_t msg;
    uint8_t seq;
};

pending_t axis_pending[motion::AXIS_COUNT];
pending_t recoat_pending;
bool      axis_was_busy[motion::AXIS_COUNT];

void sendAckLater(pending_t &p, const packet_t &packet)
{
    p.active = true;
    p.dst    = packet.src;
    p.msg    = packet.msg;
    p.seq    = packet.seq;
}

void settleAck(pending_t &p, uint8_t status)
{
    if (!p.active) return;
    p.active = false;

    sys_ack_t ack;
    ack.ack_msg = p.msg;
    ack.ack_seq = p.seq;
    ack.status  = status;
    link.sendStruct(p.dst, MSG_ACK, ack, FLAG_IS_RESPONSE);
}

void sendFault(uint8_t code, uint16_t detail, uint16_t bit)
{
    fault_flags   |= bit;
    latched_flags |= bit;

    fault_report_t f;
    f.code   = code;
    f.node   = NODE_ARDUINO_MEGA;
    f.detail = detail;
    link.sendStruct(NODE_BROADCAST, MSG_FAULT, f, FLAG_IS_ERROR);
}

/* A fault worth naming that does not need clearing: it goes away when its
 * cause does. */
void sendLiveFault(uint8_t code, uint16_t detail)
{
    fault_report_t f;
    f.code   = code;
    f.node   = NODE_ARDUINO_MEGA;
    f.detail = detail;
    link.sendStruct(NODE_BROADCAST, MSG_FAULT, f, FLAG_IS_ERROR);
}

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
    /* Which of the three purge stages is running — the whole sequence can take
     * ~40 minutes, so "purging" on its own is not much of an answer. */
    if (machine_state == STATE_PURGING) state.substate = airflow::purgeStage();
    link.sendStruct(dst, MSG_STATE, state, FLAG_IS_RESPONSE);
}

void sendAxisStatus(uint8_t axis, uint8_t dst)
{
    axis_status_t st;
    st.axis        = axis;
    st.flags       = motion::statusFlags(axis);
    st.position_um = motion::position_um(axis);
    st.target_um   = motion::target_um(axis);
    link.sendStruct(dst, MSG_AXIS_STATUS, st);
}

void sendOverride(uint8_t dst)
{
    if (!sensors::overrideActive()) return;
    sensor_override_t ov;
    sensors::fillOverride(ov);
    link.sendStruct(dst, MSG_SENSOR_OVERRIDE, ov);
}

/* Everything off, everything frozen. Latched: only a physical reset clears it. */
void enterEstop()
{
    motion::estop();
    airflow::allOff();
    recoat::abort();
    recoat::consumeResult();          /* the abort is not a cycle failure to report */
    lighting::off();

    machine_state = STATE_ESTOP;
    fault_flags   |= FAULTBIT_ESTOP;
    latched_flags |= FAULTBIT_ESTOP;
    safety::setEstop(true);

    for (uint8_t i = 0; i < motion::AXIS_COUNT; i++)
        settleAck(axis_pending[i], ACK_REFUSED);
    settleAck(recoat_pending, ACK_REFUSED);
}

/* The Mega's own view of the machine. It does not own printing — that is the
 * Teensy's sequencer — so STATE_PRINTING never appears here. */
void updateState()
{
    if (machine_state == STATE_ESTOP) return;

    if (latched_flags & ~(uint16_t)FAULTBIT_ESTOP) {
        machine_state = STATE_FAULT;
        return;
    }
    if (motion::homing(AXIS_FEED) || motion::homing(AXIS_BED) || motion::homing(AXIS_WIPE)) {
        machine_state = STATE_HOMING;
        return;
    }
    if (airflow::purging()) {
        machine_state = STATE_PURGING;
        return;
    }
    /* STATE_READY is "homed and purged, will accept a job" — so the chamber
     * has to actually be inert, not merely not-faulted. With the O2 override
     * on this is trivially satisfied, which is reported as an override rather
     * than hidden. */
    const bool inert = sensors::oxygenWorstPpm() <= OXYGEN_THRESHOLD_PPM;
    machine_state = (safety::allClear() && inert &&
                     (motion::statusFlags(AXIS_WIPE) & AXIS_FLAG_HOMED))
                        ? STATE_READY
                        : STATE_IDLE;
}

bool payloadIs(const packet_t &packet, uint8_t len)
{
    return packet.len == len;
}

void handleAxisMove(const packet_t &packet)
{
    if (!payloadIs(packet, sizeof(axis_move_t))) {
        link.sendAck(packet, ACK_BAD_LENGTH);
        return;
    }
    axis_move_t req;
    memcpy(&req, packet.payload, sizeof(req));

    if (!motion::fitted(req.axis)) {
        /* AXIS_BLADE_LIFT is reserved in the protocol but not fitted. Refused,
         * never accepted as a silent no-op. */
        link.sendAck(packet, ACK_BAD_PARAM);
        return;
    }
    if (recoat::active()) {
        link.sendAck(packet, ACK_BUSY);
        return;
    }

    if (req.flags & AXIS_MOVE_NO_BOUNDS) {
        /* This is the maintenance move that runs a piston to the top of its
         * rail, where there is no switch to catch it. It is only allowed when
         * the machine is otherwise still, and it never passes silently — if it
         * ever shows up in a log during a print, something is wrong upstream.
         *
         * The Mega cannot tell whether the Teensy is mid-mark; refusing a move
         * during marking is the Teensy's job. This is the part the Mega can
         * see for itself. */
        if (motion::anyBusy() || machine_state == STATE_PRINTING) {
            link.sendAck(packet, ACK_BAD_STATE);
            return;
        }
        link.sendLog(NODE_BROADCAST, LOG_WARN, "move with travel limits off");
    }

    if (!motion::moveTo(req.axis, req.target_um, req.speed_um_s,
                        req.accel_um_s2, req.flags)) {
        link.sendAck(packet, ACK_BUSY);
        return;
    }
    sendAckLater(axis_pending[req.axis], packet);
}

void handleAxisHome(const packet_t &packet)
{
    if (!payloadIs(packet, sizeof(axis_home_t))) {
        link.sendAck(packet, ACK_BAD_LENGTH);
        return;
    }
    axis_home_t req;
    memcpy(&req, packet.payload, sizeof(req));

    if (!motion::fitted(req.axis)) {
        link.sendAck(packet, ACK_BAD_PARAM);
        return;
    }
    if (recoat::active() || !motion::home(req.axis)) {
        link.sendAck(packet, ACK_BUSY);
        return;
    }
    sendAckLater(axis_pending[req.axis], packet);
}

void handleAxisStop(const packet_t &packet)
{
    if (!payloadIs(packet, sizeof(axis_home_t))) {
        link.sendAck(packet, ACK_BAD_LENGTH);
        return;
    }
    axis_home_t req;
    memcpy(&req, packet.payload, sizeof(req));

    if (!motion::fitted(req.axis)) {
        link.sendAck(packet, ACK_BAD_PARAM);
        return;
    }
    recoat::abort();
    recoat::consumeResult();
    settleAck(recoat_pending, ACK_REFUSED);
    motion::stop(req.axis);
    link.sendAck(packet, ACK_OK);
}

void handleRecoat(const packet_t &packet)
{
    if (!payloadIs(packet, sizeof(recoat_cycle_t))) {
        link.sendAck(packet, ACK_BAD_LENGTH);
        return;
    }
    recoat_cycle_t req;
    memcpy(&req, packet.payload, sizeof(req));

    const uint8_t status = recoat::start(req);
    if (status != ACK_OK) {
        link.sendAck(packet, status);
        return;
    }
    sendAckLater(recoat_pending, packet);
}

void onPacket(const packet_t &packet, void *)
{
    /* MSG_ESTOP first, in every state, before any other work. */
    if (packet.msg == MSG_ESTOP) {
        enterEstop();
        link.sendAck(packet, ACK_OK);
        return;
    }

    /* Once latched, the only things still worth answering are the ones that
     * report what happened. Nothing may start moving again. */
    if (machine_state == STATE_ESTOP) {
        switch (packet.msg) {
        case MSG_PING:          link.sendEmpty(packet.src, MSG_PONG, FLAG_IS_RESPONSE); return;
        case MSG_HELLO:         sendHello(packet.src); return;
        case MSG_STATE_REQUEST: sendState(packet.src); return;
        case MSG_SAFETY_STATUS: break;
        default:                link.sendAck(packet, ACK_REFUSED); return;
        }
    }

    switch (packet.msg) {
    case MSG_PING:
        link.sendEmpty(packet.src, MSG_PONG, FLAG_IS_RESPONSE);
        break;

    case MSG_HELLO:
        sendHello(packet.src);
        sendOverride(packet.src);
        break;

    case MSG_STATE_REQUEST:
        sendState(packet.src);
        break;

    case MSG_SAFETY_STATUS: {
        safety_status_t st;
        safety::fill(st);
        link.sendStruct(packet.src, MSG_SAFETY_STATUS, st, FLAG_IS_RESPONSE);
        break;
    }

    case MSG_RESET:
        /* Refused, with a reason. A watchdog reset is the only clean way to
         * do this on an AVR, and the stock Mega2560 bootloader does not
         * reliably clear WDRF on entry — the board can come up in a reset
         * loop that needs a manual reflash to escape. That is not a risk
         * worth taking on a machine that may be mid-print. Revisit if the
         * bootloader is confirmed watchdog-safe. */
        link.sendAck(packet, ACK_REFUSED);
        break;

    case MSG_FAULT_CLEAR:
        /* FAULTBIT_ESTOP is not clearable over the link — it needs a physical
         * reset — and a fault whose cause is still present comes straight
         * back on the next service tick. */
        latched_flags &= (uint16_t)FAULTBIT_ESTOP;
        fault_flags = (uint16_t)(latched_flags | safety::trippedMask());
        link.sendAck(packet, ACK_OK);
        break;

    case MSG_AXIS_HOME:
        handleAxisHome(packet);
        break;

    case MSG_AXIS_MOVE:
        handleAxisMove(packet);
        break;

    case MSG_AXIS_STOP:
        handleAxisStop(packet);
        break;

    case MSG_AXIS_STATUS: {
        for (uint8_t i = 0; i < motion::AXIS_COUNT; i++) sendAxisStatus(i, packet.src);
        break;
    }

    case MSG_RECOAT_CYCLE:
        handleRecoat(packet);
        break;

    case MSG_SENSOR_REPORT: {
        sensor_report_t rep;
        sensors::fill(rep);
        link.sendStruct(packet.src, MSG_SENSOR_REPORT, rep, FLAG_IS_RESPONSE);
        break;
    }

    case MSG_SENSOR_OVERRIDE:
        sendOverride(packet.src);
        break;

    case MSG_PURGE_SET:
        if (!payloadIs(packet, sizeof(purge_set_t))) {
            link.sendAck(packet, ACK_BAD_LENGTH);
            break;
        }
        {
            purge_set_t req;
            memcpy(&req, packet.payload, sizeof(req));
            link.sendAck(packet, airflow::setPurge(req));
        }
        break;

    case MSG_LIGHT_SET:
        if (!payloadIs(packet, sizeof(light_set_t))) {
            link.sendAck(packet, ACK_BAD_LENGTH);
            break;
        }
        {
            light_set_t req;
            memcpy(&req, packet.payload, sizeof(req));
            link.sendAck(packet, lighting::set(req));
        }
        break;

    case MSG_FAN_SET:
        if (!payloadIs(packet, sizeof(fan_set_t))) {
            link.sendAck(packet, ACK_BAD_LENGTH);
            break;
        }
        {
            fan_set_t req;
            memcpy(&req, packet.payload, sizeof(req));
            const uint8_t status = airflow::setFan(req);
            link.sendAck(packet, status);
            if (status == ACK_OK) {
                fan_status_t st;
                if (airflow::fillStatus(req.fan, st))
                    link.sendStruct(packet.src, MSG_FAN_STATUS, st);
            }
        }
        break;

    case MSG_FAN_STATUS: {
        fan_status_t st;
        for (uint8_t f = FAN_CHAMBER_BLOWER; f <= FAN_RADIATOR; f++)
            if (airflow::fillStatus(f, st))
                link.sendStruct(packet.src, MSG_FAN_STATUS, st, FLAG_IS_RESPONSE);
        break;
    }

    default:
        link.sendAck(packet, ACK_UNKNOWN_MSG);
        break;
    }
}

/* Turn anything the subsystems latched into MSG_FAULT, and settle the ACKs of
 * whatever it interrupted. */
void serviceFaults()
{
    const uint8_t axis = motion::consumeLimitFault();
    if (axis != motion::AXIS_NONE) {
        sendFault(FAULT_LIMIT_UNEXPECT, axis, FAULTBIT_MOTION);
        settleAck(axis_pending[axis], ACK_REFUSED);
        if (recoat::active()) {
            recoat::abort();
            recoat::consumeResult();
            settleAck(recoat_pending, ACK_REFUSED);
        }
    }

    /* A purge that did not hold is reported but does not latch: whether to
     * print into a chamber that failed verification is the job sequencer's
     * call, not the board's. STATE_READY already refuses to appear while the
     * chamber is above the threshold, which is the gate that matters. */
    const airflow::purge_result_t purge = airflow::consumePurgeResult();
    if (purge == airflow::PURGE_RESULT_FAILED) {
        sendLiveFault(FAULT_OXYGEN_HIGH, sensors::oxygenWorstPpm());
        link.sendLog(NODE_BROADCAST, LOG_WARN, "purge did not hold");
    } else if (purge == airflow::PURGE_RESULT_PASSED) {
        link.sendLog(NODE_BROADCAST, LOG_INFO, "purge verified");
    }

    const uint8_t dead = sensors::consumeInvalidSensor();
    if (dead != sensors::SENSOR_NONE)
        sendFault(FAULT_SENSOR_INVALID, dead, FAULTBIT_TEMP);

    /* The interlock picture is already published as safety_status_t, but a
     * fresh trip also gets a fault code so a listener does not have to diff
     * two status messages to notice one. Live, not latched — it clears itself
     * when the door shuts. */
    const uint16_t tripped = safety::trippedMask();
    const uint16_t newly = (uint16_t)(tripped & ~last_tripped_mask);
    last_tripped_mask = tripped;
    if (newly & FAULTBIT_DOOR)   sendLiveFault(FAULT_DOOR_OPEN, 0);
    if (newly & FAULTBIT_TEMP)   sendLiveFault(FAULT_OVERTEMP, 0);
    if (newly & FAULTBIT_OXYGEN) sendLiveFault(FAULT_OXYGEN_HIGH, 0);

    /* Link errors are diagnostic, not a machine fault: a noisy cable should
     * not need a MSG_FAULT_CLEAR to get out of. Reported in batches so a bad
     * connection cannot flood the link it is already struggling with. */
    const uint16_t errs = (uint16_t)(link.crcErrors() + link.protoErrors());
    if ((uint16_t)(errs - last_link_errors) >= LINK_ERROR_REPORT_STEP) {
        last_link_errors = errs;
        sendLiveFault(FAULT_PROTOCOL_ERROR, errs);
    }

    const uint16_t warn = sensors::warnMask();
    if (warn != last_warn_mask) {
        last_warn_mask = warn;
        if (warn) link.sendLog(NODE_BROADCAST, LOG_WARN, "temp near limit");
    }
}

/* Settle deferred ACKs as the work behind them finishes. */
void serviceCompletions()
{
    for (uint8_t i = 0; i < motion::AXIS_COUNT; i++) {
        const bool busy = motion::busy(i);
        if (axis_was_busy[i] && !busy) {
            settleAck(axis_pending[i], ACK_OK);
            sendAxisStatus(i, NODE_BROADCAST);
        }
        axis_was_busy[i] = busy;
    }

    const recoat::result_t r = recoat::consumeResult();
    if (r == recoat::RESULT_OK)          settleAck(recoat_pending, ACK_OK);
    else if (r == recoat::RESULT_FAILED) settleAck(recoat_pending, ACK_REFUSED);
}

void serviceReports()
{
    const uint32_t now = millis();

    if (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
        last_heartbeat_ms = now;

        sys_heartbeat_t beat;
        beat.node        = NODE_ARDUINO_MEGA;
        beat.state       = machine_state;
        beat.fault_flags = fault_flags;
        beat.uptime_ms   = now;
        link.sendStruct(NODE_BROADCAST, MSG_HEARTBEAT, beat);
    }

    if (now - last_sensor_report_ms >= SENSOR_REPORT_INTERVAL_MS) {
        last_sensor_report_ms = now;

        sensor_report_t rep;
        sensors::fill(rep);
        link.sendStruct(NODE_BROADCAST, MSG_SENSOR_REPORT, rep);
    }

    /* Published on change as well as on the tick, so an interlock opening is
     * seen immediately rather than up to a second later. */
    if (safety::consumeChanged() || now - last_safety_report_ms >= SAFETY_REPORT_INTERVAL_MS) {
        last_safety_report_ms = now;

        safety_status_t st;
        safety::fill(st);
        link.sendStruct(NODE_BROADCAST, MSG_SAFETY_STATUS, st);

        fault_flags = (uint16_t)(latched_flags | safety::trippedMask());
    }

    if (motion::anyBusy() && now - last_axis_report_ms >= AXIS_REPORT_INTERVAL_MS) {
        last_axis_report_ms = now;
        for (uint8_t i = 0; i < motion::AXIS_COUNT; i++)
            if (motion::busy(i)) sendAxisStatus(i, NODE_BROADCAST);
    }

    if (sensors::consumeOverrideChanged()) sendOverride(NODE_BROADCAST);
}

}  // namespace

void setup()
{
    Serial.begin(115200);

    /* Outputs are configured by the modules that own them, and every one of
     * them comes up in its safe state: drivers not stepping, solenoid closed,
     * fans at zero, lights off. */
    persist::begin();   /* before motion::begin(), which restores from it */
    motion::begin();
    sensors::begin();
    safety::begin();
    airflow::begin();
    lighting::begin();
    recoat::begin();

    link.onPacket(onPacket);

    const uint32_t now = millis();
    last_heartbeat_ms = last_sensor_report_ms = last_safety_report_ms = last_axis_report_ms = now;
    last_warn_mask = 0;

    last_tripped_mask = safety::trippedMask();
    last_link_errors = 0;

    machine_state = STATE_IDLE;
    sendHello(NODE_BROADCAST);
    sendOverride(NODE_BROADCAST);
    sensors::consumeOverrideChanged();   /* just sent it; do not repeat on tick 1 */
}

void loop()
{
    /* Every byte on the port reaches the decoder. The old firmware ended its
     * sensor report by draining and discarding whatever had arrived, which is
     * where the host-side timeouts and "drain stale completions" hacks came
     * from. */
    link.poll();

    /* Step pulses come from here, so it runs every iteration and nothing below
     * is allowed to block. */
    motion::service();

    recoat::service();
    persist::service();
    sensors::service();
    safety::service();
    airflow::service();

    serviceFaults();
    serviceCompletions();
    updateState();
    serviceReports();
}
