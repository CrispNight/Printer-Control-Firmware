/*
 * protocol.h — Moiren SLM machine communication protocol.
 *
 * SINGLE SOURCE OF TRUTH for the wire format. Every board in this repo and
 * the PC-side application speak exactly what is defined here.
 *
 *   - Human-readable companion doc: protocol/PROTOCOL.md
 *   - PC-side Python bindings:      protocol/protocol.py  (GENERATED)
 *
 * After editing this file, regenerate the Python bindings:
 *
 *     python tools/gen_protocol.py
 *
 * CI fails if protocol.py is out of date with respect to this header, so the
 * firmware and PC sides cannot silently drift apart.
 *
 * Plain C (no C++-only constructs) so C, C++ and the code generator can all
 * consume it.
 */

#ifndef MOIREN_PROTOCOL_H
#define MOIREN_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== PROTOCOL-GEN BEGIN ===================================================
 * Everything between the GEN BEGIN/END markers is parsed by
 * tools/gen_protocol.py. Keep to the declaration style already used here:
 * `#define`d integer constants, `typedef enum { NAME = value, } name_t;` and
 * `typedef struct { fixed-width members } name_t;`. Anything outside the
 * markers is ignored by the generator and is free-form C.
 * ========================================================================= */

/* Bump on ANY change to packet layout, message ids, or payload structs.
 * Nodes refuse to talk to a peer reporting a different major version. */
#define PROTOCOL_VERSION 1

/* --- Packet framing ------------------------------------------------------ */

#define PACKET_SOF0        0xA5  /* start-of-packet byte 0 */
#define PACKET_SOF1        0x5A  /* start-of-packet byte 1 */
#define PACKET_HEADER_LEN  9     /* SOF0..LEN inclusive */
#define PACKET_CRC_LEN     2
#define PACKET_MAX_PAYLOAD 192
#define PACKET_MAX_LEN     203   /* HEADER_LEN + MAX_PAYLOAD + CRC_LEN */

/* Packet flag bits (the FLAGS header byte). */
#define FLAG_NEEDS_ACK    0x01  /* sender expects an ACK for this SEQ */
#define FLAG_IS_RESPONSE  0x02  /* this packet answers an earlier SEQ */
#define FLAG_IS_ERROR     0x04  /* payload is a fault_report_t */
#define FLAG_NO_ROUTE     0x08  /* do not forward; consume at DST only */

/* --- Nodes --------------------------------------------------------------- */

typedef enum {
    NODE_BROADCAST     = 0x00,
    NODE_PC            = 0x01,  /* optional host: UI, file upload, camera */
    NODE_TEENSY_GALVO  = 0x02,  /* galvo + laser; master in the target design */
    NODE_ARDUINO_MEGA  = 0x03,  /* steppers, O2/temp, interlocks, airflow today */
    NODE_ESP32_FAN     = 0x04,  /* EDF/ESC airflow control; not yet built */
} node_id_t;

/* Airflow is owned by the Mega today and moves to the ESP32 later. Address
 * FAN_* messages to NODE_AIRFLOW and the migration is a one-line change here
 * rather than an edit to every call site. */
#define NODE_AIRFLOW  NODE_ARDUINO_MEGA

/* --- Message ids --------------------------------------------------------- */

typedef enum {
    /* 0x00-0x0F  system / link */
    MSG_PING            = 0x01,
    MSG_PONG            = 0x02,
    MSG_HELLO           = 0x03,  /* sys_hello_t     — identity on link-up */
    MSG_ACK             = 0x04,  /* sys_ack_t */
    MSG_LOG             = 0x05,  /* sys_log_t */
    MSG_HEARTBEAT       = 0x06,  /* sys_heartbeat_t — periodic liveness */
    MSG_RESET           = 0x07,  /* no payload      — soft reset the node */

    /* 0x10-0x1F  machine state and job control */
    MSG_STATE_REQUEST   = 0x10,  /* no payload */
    MSG_STATE           = 0x11,  /* state_report_t */
    MSG_JOB_START       = 0x12,  /* job_start_t */
    MSG_JOB_ABORT       = 0x13,  /* no payload */
    MSG_JOB_PAUSE       = 0x14,  /* no payload */
    MSG_JOB_RESUME      = 0x15,  /* no payload */
    MSG_LAYER_BEGIN     = 0x16,  /* layer_begin_t */
    MSG_LAYER_END       = 0x17,  /* layer_end_t */
    MSG_JOB_COMPLETE    = 0x18,  /* no payload */

    /* 0x20-0x2F  safety */
    MSG_SAFETY_STATUS   = 0x20,  /* safety_status_t */
    MSG_ESTOP           = 0x21,  /* no payload — highest priority, always acted on */
    MSG_FAULT           = 0x22,  /* fault_report_t */
    MSG_FAULT_CLEAR     = 0x23,  /* no payload */

    /* 0x30-0x3F  motion (Mega) */
    MSG_AXIS_HOME       = 0x30,  /* axis_home_t */
    MSG_AXIS_MOVE       = 0x31,  /* axis_move_t */
    MSG_AXIS_STOP       = 0x32,  /* axis_home_t (axis field only) */
    MSG_AXIS_STATUS     = 0x33,  /* axis_status_t */
    MSG_RECOAT_CYCLE    = 0x34,  /* recoat_cycle_t — full powder recoat pass */

    /* 0x40-0x4F  process sensing (Mega) */
    MSG_SENSOR_REPORT   = 0x40,  /* sensor_report_t */
    MSG_PURGE_SET       = 0x41,  /* purge_set_t — argon solenoid / O2 target */

    /* 0x50-0x5F  laser and galvo (Teensy) */
    MSG_LASER_ARM       = 0x50,  /* laser_arm_t */
    MSG_LASER_PARAMS    = 0x51,  /* laser_params_t */
    MSG_MARK_BATCH      = 0x52,  /* mark_batch_header_t + vector_point_t[count] */
    MSG_MARK_ABORT      = 0x53,  /* no payload */
    MSG_GALVO_STATUS    = 0x54,  /* galvo_status_t */

    /* 0x60-0x6F  airflow (Mega today, ESP32 later) */
    MSG_FAN_SET         = 0x60,  /* fan_set_t */
    MSG_FAN_STATUS      = 0x61,  /* fan_status_t */
} msg_id_t;

/* --- Machine state ------------------------------------------------------- */

typedef enum {
    STATE_BOOT     = 0x00,  /* powering up, nodes not yet identified */
    STATE_IDLE     = 0x01,  /* alive, not homed, chamber not conditioned */
    STATE_HOMING   = 0x02,
    STATE_PURGING  = 0x03,  /* argon flowing, waiting for O2 below target */
    STATE_READY    = 0x04,  /* homed + purged, will accept MSG_JOB_START */
    STATE_PRINTING = 0x05,
    STATE_PAUSED   = 0x06,
    STATE_FAULT    = 0x07,  /* recoverable; needs MSG_FAULT_CLEAR */
    STATE_ESTOP    = 0x08,  /* latched; needs physical reset */
} machine_state_e;

/* --- Axes ---------------------------------------------------------------- */

typedef enum {
    AXIS_FEED = 0x00,  /* powder feed / dispense piston */
    AXIS_BED  = 0x01,  /* build platform Z */
    AXIS_WIPE = 0x02,  /* recoater blade */
} axis_id_t;

/* axis_status_t.flags bits */
#define AXIS_FLAG_HOMED    0x01
#define AXIS_FLAG_MOVING   0x02
#define AXIS_FLAG_AT_LIMIT 0x04
#define AXIS_FLAG_ENABLED  0x08
#define AXIS_FLAG_FAULT    0x10

/* --- Faults -------------------------------------------------------------- */

typedef enum {
    FAULT_NONE            = 0x00,
    FAULT_DOOR_OPEN       = 0x01,
    FAULT_OXYGEN_HIGH     = 0x02,
    FAULT_OVERTEMP        = 0x03,
    FAULT_AXIS_STALL      = 0x04,
    FAULT_LIMIT_UNEXPECT  = 0x05,  /* limit hit where none was expected */
    FAULT_LASER_INTERLOCK = 0x06,
    FAULT_LASER_FAULT     = 0x07,  /* fault line asserted by the laser source */
    FAULT_GALVO_FAULT     = 0x08,
    FAULT_FAN_STALL       = 0x09,
    FAULT_COMMS_TIMEOUT   = 0x0A,  /* peer stopped heartbeating */
    FAULT_PROTOCOL_ERROR  = 0x0B,  /* bad CRC, bad length, unknown message */
    FAULT_VERSION_MISMATCH= 0x0C,
    FAULT_SENSOR_INVALID  = 0x0D,
    FAULT_INTERNAL        = 0xFF,
} fault_code_t;

/* safety_status_t masks and fault_flags bits — one bit per fault domain, so a
 * node can report several concurrent problems in a single 16-bit field. */
#define FAULTBIT_DOOR      0x0001
#define FAULTBIT_OXYGEN    0x0002
#define FAULTBIT_TEMP      0x0004
#define FAULTBIT_MOTION    0x0008
#define FAULTBIT_LASER     0x0010
#define FAULTBIT_GALVO     0x0020
#define FAULTBIT_AIRFLOW   0x0040
#define FAULTBIT_COMMS     0x0080
#define FAULTBIT_ESTOP     0x0100

/* --- Log levels ---------------------------------------------------------- */

typedef enum {
    LOG_DEBUG = 0x00,
    LOG_INFO  = 0x01,
    LOG_WARN  = 0x02,
    LOG_ERROR = 0x03,
} log_level_t;

/* --- ACK status ---------------------------------------------------------- */

typedef enum {
    ACK_OK            = 0x00,
    ACK_BAD_CRC       = 0x01,
    ACK_BAD_LENGTH    = 0x02,
    ACK_UNKNOWN_MSG   = 0x03,
    ACK_BAD_STATE     = 0x04,  /* valid message, wrong machine state for it */
    ACK_BAD_PARAM     = 0x05,
    ACK_BUSY          = 0x06,
    ACK_REFUSED       = 0x07,  /* refused on safety grounds */
} ack_status_t;

/* --- Laser / galvo ------------------------------------------------------- */

typedef enum {
    LASER_STATE_DISARMED = 0x00,
    LASER_STATE_ARMED    = 0x01,  /* armed but not emitting */
    LASER_STATE_MARKING  = 0x02,
    LASER_STATE_FAULT    = 0x03,
} laser_state_t;

/* Written to laser_arm_t.key to arm. Guards against a corrupted or stray
 * packet enabling emission. */
#define LASER_ARM_KEY 0x4D4F4152UL  /* "MOAR" */

/* vector_point_t.flags bits */
#define POINT_FLAG_LASER_ON  0x01  /* laser on while travelling TO this point */
#define POINT_FLAG_LAST      0x02  /* final point of the layer */

/* mark_batch_header_t.flags bits */
#define BATCH_FLAG_FIRST     0x01
#define BATCH_FLAG_LAST      0x02

/* fan_set_t.mode */
typedef enum {
    FAN_MODE_OFF       = 0x00,
    FAN_MODE_MANUAL    = 0x01,  /* hold duty_pm exactly */
    FAN_MODE_MAPPED    = 0x02,  /* map from scan speed — what the Mega does today */
    FAN_MODE_CLOSEDLOOP= 0x03,  /* hold target_flow_cm_s; needs a flow sensor */
} fan_mode_t;

/* ==========================================================================
 * Payload structs.
 *
 * Wire encoding: little-endian, packed, no implicit padding. All quantities
 * are fixed-point integers (never float) so the AVR, ARM and Xtensa builds
 * agree byte for byte. Units are in the member name suffix:
 *   _um    micrometres          _um_s   micrometres/second
 *   _um_s2 micrometres/second^2  _mm_s   millimetres/second
 *   _pm    per-mille (0..1000)   _c_x10  degrees C x10
 *   _ppm   parts per million     _us/_ns microseconds / nanoseconds
 * ======================================================================== */

#pragma pack(push, 1)

/* MSG_HELLO — sent by every node on link-up and in reply to MSG_PING. */
typedef struct {
    uint8_t  node;           /* node_id_t of the sender */
    uint8_t  proto_version;  /* PROTOCOL_VERSION it was built against */
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    uint8_t  reserved;
    uint32_t capabilities;   /* reserved bitfield; 0 for now */
    char     build_id[16];   /* short git hash or build tag, NUL-padded */
} sys_hello_t;

/* MSG_HEARTBEAT — every node emits this on a fixed period. Silence past the
 * timeout is FAULT_COMMS_TIMEOUT at the receiver. */
typedef struct {
    uint8_t  node;
    uint8_t  state;        /* machine_state_e as seen by this node */
    uint16_t fault_flags;  /* FAULTBIT_* */
    uint32_t uptime_ms;
} sys_heartbeat_t;

/* MSG_ACK */
typedef struct {
    uint8_t ack_msg;  /* msg_id_t being acknowledged */
    uint8_t ack_seq;  /* SEQ of the packet being acknowledged */
    uint8_t status;   /* ack_status_t */
} sys_ack_t;

/* MSG_LOG — human-readable diagnostics, forwarded to the PC when present. */
typedef struct {
    uint8_t level;     /* log_level_t */
    char    text[48];  /* NUL-padded, not necessarily NUL-terminated */
} sys_log_t;

/* MSG_STATE */
typedef struct {
    uint8_t  state;        /* machine_state_e */
    uint8_t  substate;     /* state-specific detail; 0 when unused */
    uint16_t fault_flags;  /* FAULTBIT_* */
    uint16_t layer_index;  /* 0-based; valid in STATE_PRINTING/PAUSED */
    uint16_t layer_total;
    uint32_t elapsed_s;    /* since MSG_JOB_START */
} state_report_t;

/* MSG_JOB_START */
typedef struct {
    uint32_t job_id;             /* PC-assigned, or firmware-assigned when standalone */
    uint16_t layer_total;
    uint16_t reserved;
    int32_t  layer_thickness_um;
} job_start_t;

/* MSG_LAYER_BEGIN */
typedef struct {
    uint16_t layer_index;
    uint16_t reserved;
    int32_t  z_um;  /* absolute build platform position for this layer */
} layer_begin_t;

/* MSG_LAYER_END */
typedef struct {
    uint16_t layer_index;
    uint16_t fault_flags;
    uint32_t duration_ms;
} layer_end_t;

/* MSG_SAFETY_STATUS — the interlock picture, published by whichever node owns
 * the interlock inputs (the Mega). */
typedef struct {
    uint16_t interlock_mask;  /* FAULTBIT_* the sender monitors */
    uint16_t tripped_mask;    /* FAULTBIT_* currently tripped */
    uint8_t  door_ok;
    uint8_t  oxygen_ok;
    uint8_t  temp_ok;
    uint8_t  estop_active;
} safety_status_t;

/* MSG_FAULT */
typedef struct {
    uint8_t  code;    /* fault_code_t */
    uint8_t  node;    /* node_id_t that raised it */
    uint16_t detail;  /* code-specific: axis id, sensor index, ... */
} fault_report_t;

/* MSG_AXIS_HOME, MSG_AXIS_STOP */
typedef struct {
    uint8_t axis;   /* axis_id_t */
    uint8_t flags;  /* reserved */
} axis_home_t;

/* MSG_AXIS_MOVE */
typedef struct {
    uint8_t  axis;         /* axis_id_t */
    uint8_t  flags;        /* bit0: target is relative to current position */
    int32_t  target_um;
    uint32_t speed_um_s;
    uint32_t accel_um_s2;
} axis_move_t;

/* MSG_AXIS_STATUS */
typedef struct {
    uint8_t axis;         /* axis_id_t */
    uint8_t flags;        /* AXIS_FLAG_* */
    int32_t position_um;
    int32_t target_um;
} axis_status_t;

/* MSG_RECOAT_CYCLE — one complete powder recoat: drop bed, raise feed, sweep. */
typedef struct {
    int32_t  feed_um;        /* powder piston rise */
    int32_t  bed_um;         /* build platform drop (negative = down) */
    uint16_t wipe_speed_mm_s;
    uint8_t  passes;
    uint8_t  flags;          /* reserved */
} recoat_cycle_t;

/* MSG_SENSOR_REPORT — the Mega's periodic process snapshot.
 * valid_mask bit0..bit1: oxygen[0..1], bit8..bit13: temp_c_x10[0..5]. */
typedef struct {
    uint16_t oxygen_ppm[2];
    int16_t  temp_c_x10[6];
    uint16_t valid_mask;
} sensor_report_t;

/* MSG_PURGE_SET */
typedef struct {
    uint8_t  enable;         /* 0 = close argon solenoid, 1 = open */
    uint8_t  reserved;
    uint16_t target_o2_ppm;  /* purge until below this, then hold */
    uint16_t timeout_s;      /* 0 = no timeout */
} purge_set_t;

/* MSG_LASER_ARM — key must equal LASER_ARM_KEY or the packet is refused. */
typedef struct {
    uint8_t  arm;  /* 0 = disarm, 1 = arm */
    uint8_t  reserved;
    uint32_t key;
} laser_arm_t;

/* MSG_LASER_PARAMS — applies to every subsequent MSG_MARK_BATCH. */
typedef struct {
    uint16_t power_pm;         /* 0..1000 = 0..100% of source full scale */
    uint32_t freq_hz;          /* pulse repetition rate */
    uint16_t pulse_width_ns;   /* MOPA pulse width; 0 = source default */
    uint16_t mark_speed_mm_s;
    uint16_t jump_speed_mm_s;
    uint16_t on_delay_us;      /* galvo settle before emission */
    uint16_t off_delay_us;
    uint16_t poly_delay_us;    /* corner dwell between segments */
} laser_params_t;

/* MSG_MARK_BATCH header. Followed in the same payload by `count`
 * vector_point_t records. count is bounded by PACKET_MAX_PAYLOAD:
 *   count <= (PACKET_MAX_PAYLOAD - sizeof(mark_batch_header_t)) / sizeof(vector_point_t) */
typedef struct {
    uint16_t layer_index;
    uint16_t batch_index;  /* 0-based, monotonic within a layer */
    uint16_t count;        /* vector_point_t records that follow */
    uint8_t  flags;        /* BATCH_FLAG_* */
    uint8_t  reserved;
} mark_batch_header_t;

/* One galvo target, in bed coordinates. The Teensy applies the field
 * correction table; the PC never sends raw DAC counts. */
typedef struct {
    int32_t x_um;
    int32_t y_um;
    uint8_t flags;  /* POINT_FLAG_* */
} vector_point_t;

/* MSG_GALVO_STATUS */
typedef struct {
    uint8_t  laser_state;      /* laser_state_t */
    uint8_t  flags;            /* reserved */
    int32_t  x_um;
    int32_t  y_um;
    uint32_t points_remaining; /* still queued in the Teensy */
    uint16_t fault_flags;      /* FAULTBIT_* */
} galvo_status_t;

/* MSG_FAN_SET */
typedef struct {
    uint8_t  mode;              /* fan_mode_t */
    uint8_t  reserved;
    uint16_t duty_pm;           /* FAN_MODE_MANUAL: 0..1000 */
    uint16_t target_flow_cm_s;  /* FAN_MODE_CLOSEDLOOP only */
} fan_set_t;

/* MSG_FAN_STATUS */
typedef struct {
    uint8_t  mode;        /* fan_mode_t */
    uint8_t  flags;       /* reserved */
    uint16_t duty_pm;
    uint16_t rpm;         /* 0 when no tach is fitted */
    uint16_t flow_cm_s;   /* 0 when no flow sensor is fitted */
} fan_status_t;

#pragma pack(pop)

/* ===== PROTOCOL-GEN END =================================================== */

/* --- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) -----------------------
 * Table-free: the Mega has 8 KB of RAM and no room for a 512-byte table.
 * Mirrored byte-for-byte by crc16_ccitt() in the generated protocol.py. */
static inline uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    uint16_t i;
    uint8_t  bit;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Maximum vector_point_t records that fit in one MSG_MARK_BATCH packet. */
#define MARK_BATCH_MAX_POINTS \
    ((PACKET_MAX_PAYLOAD - (int)sizeof(mark_batch_header_t)) / (int)sizeof(vector_point_t))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOIREN_PROTOCOL_H */
