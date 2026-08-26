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
#define PROTOCOL_VERSION 5

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

    /* Settings a node holds and a settings page edits. The node stores them in
     * non-volatile memory, so they survive a reboot and a host that was not
     * the one that set them can still find out what they are. Without this a
     * UI can only show what it last sent, which is wrong after any reset and
     * wrong the moment a second host exists.
     * The reply's SRC says which node's settings struct the payload is. */
    MSG_SETTINGS_REQUEST = 0x08, /* no payload */
    MSG_SETTINGS         = 0x09, /* <node>_settings_t */
    MSG_SETTINGS_SET     = 0x0A, /* <node>_settings_t — stored, then ACKed */

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

    /* Job upload: PC writes a job file onto the Teensy's microSD card.
     * Payload of _DATA is opaque file bytes; the file layout is defined by
     * job_file_header_t and friends below, not parsed by the transport. */
    MSG_JOB_UPLOAD_BEGIN = 0x19, /* job_upload_begin_t */
    MSG_JOB_UPLOAD_DATA  = 0x1A, /* job_upload_data_t + bytes */
    MSG_JOB_UPLOAD_END   = 0x1B, /* job_upload_end_t */

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
    MSG_LIGHT_SET       = 0x42,  /* light_set_t — chamber lighting for the camera */
    MSG_SENSOR_OVERRIDE = 0x43,  /* sensor_override_t — sent on connect and on
                                  * change only, never on the periodic report */
    MSG_PURGE_STATUS    = 0x44,  /* purge_status_t — progress through a purge,
                                  * which can run for the better part of an
                                  * hour, plus the argon it has used */

    /* 0x50-0x5F  laser and galvo (Teensy) */
    MSG_LASER_ARM       = 0x50,  /* laser_arm_t */
    MSG_LASER_PARAMS    = 0x51,  /* laser_params_t */
    /* 0x52 retired — was MSG_MARK_BATCH, which both carried vectors and
     * started emission. Vectors now reach the machine as a job file on the SD
     * card (MSG_JOB_UPLOAD_*), so no single message does both. Not reused. */
    MSG_MARK_ABORT      = 0x53,  /* no payload */
    MSG_GALVO_STATUS    = 0x54,  /* galvo_status_t */
    MSG_TIMING_OFFSET   = 0x55,  /* timing_offset_t — laser lead/lag vs position */

    /* Field correction table upload. Atomic: nothing is applied until _END
     * verifies the whole-table CRC, so a dropped chunk can never leave a
     * half-written table in place. */
    MSG_FIELD_CORRECTION_BEGIN = 0x56, /* field_corr_begin_t */
    MSG_FIELD_CORRECTION_DATA  = 0x57, /* field_corr_data_t + entries */
    MSG_FIELD_CORRECTION_END   = 0x58, /* no payload — commit and verify */

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
    AXIS_FEED       = 0x00,  /* powder feed / dispense piston (closed loop) */
    AXIS_BED        = 0x01,  /* build platform Z (closed loop) */
    AXIS_WIPE       = 0x02,  /* recoater blade (open loop — drifts, re-home) */
    AXIS_BLADE_LIFT = 0x03,  /* RESERVED — not fitted. Would tilt the blade
                              * clear so the return traverse needs no bed move. */
} axis_id_t;

/* axis_move_t.flags bits.
 * The flag names the direction of the OVERSHOOT, not of the final approach.
 * APPROACH_NEG drives past the target on the low side and comes back up, so
 * the axis always arrives travelling positive and the backlash is taken up the
 * same way every time. That is what the bed does on every recoat.
 * The overshoot is skipped when the move already ends travelling the right
 * way, since it would then take up the backlash by itself. */
#define AXIS_MOVE_RELATIVE     0x01
#define AXIS_MOVE_APPROACH_NEG 0x02
#define AXIS_MOVE_APPROACH_POS 0x04

/* Ignore the software travel limits for THIS MOVE ONLY. It is not a mode and
 * it does not stick: every unbounded move has to ask again.
 *
 * The build and supply cylinders have no top limit switch — there was no room
 * for one in this build of the machine — so the software limit is normally the
 * only thing at that end. But maintenance genuinely needs to go past it: the
 * pistons must be driven to the very top of the rail to get the build plate
 * adapters out, and re-homing first is slow and disturbs whatever else is
 * being reset.
 *
 * So this exists for troubleshooting and maintenance, and the node is expected
 * to refuse it whenever the machine is doing something, and to say loudly when
 * it honours it. Limit switches are NOT affected — nothing turns those off. */
#define AXIS_MOVE_NO_BOUNDS    0x08

/* axis_status_t.flags bits.
 *
 * HOMED means "has a usable zero", which is also true of a position restored
 * from non-volatile storage after a power cycle. POS_RESTORED says the zero
 * came back from storage rather than from a home this power cycle: believed,
 * but not verified, since nothing stops an axis being moved by hand while the
 * machine is off. A UI should show the two differently.
 *
 * Neither bit set means the axis has no zero at all, so software travel limits
 * cannot be applied to it. */
#define AXIS_FLAG_HOMED        0x01
#define AXIS_FLAG_MOVING       0x02
#define AXIS_FLAG_AT_LIMIT     0x04
#define AXIS_FLAG_ENABLED      0x08
#define AXIS_FLAG_FAULT        0x10
#define AXIS_FLAG_POS_RESTORED 0x20

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

/* --- Recoat park position ------------------------------------------------ */

/* Where the recoater ends the cycle. Overflow is the default: the build-plate
 * drop that makes room for the next layer also clears the return traverse, so
 * powder is spread only on the forward pass. Supply parking costs an extra
 * drop-and-raise to clear the blade over freshly spread powder. */
typedef enum {
    PARK_OVERFLOW = 0x00,  /* default */
    PARK_SUPPLY   = 0x01,  /* needs clearance_um on the return */
    PARK_STAGED   = 0x02,  /* RESERVED — pre-staged pile near the build area */
} park_mode_t;

/* purge_set_t.flags bits.
 * The minimum mixing time is not padding: oxygen can read low at the sensor
 * long before the chamber is homogeneous, and the pockets are what ruin a
 * part. Skipping it is a deliberate testing action, which is why it is a flag
 * a caller has to set rather than a value that can drift to zero. */
#define PURGE_FLAG_SKIP_MIN_MIX 0x01

/* --- Chamber lighting ---------------------------------------------------- */

/* Two relays. SHADOW is side-lighting, which is how surface topology and
 * therefore recoat defects become visible to the camera. */
typedef enum {
    LIGHT_OFF     = 0x00,
    LIGHT_AMBIENT = 0x01,
    LIGHT_SHADOW  = 0x02,
} light_mode_t;

/* --- Fans ---------------------------------------------------------------- */

/* Two independent fans on separate outputs. */
typedef enum {
    FAN_CHAMBER_BLOWER = 0x00,  /* argon circulation over the build area */
    FAN_RADIATOR       = 0x01,  /* build plate water cooling — NOT WIRED on the
                                 * current machine; the plate adapter is
                                 * plastic. Defined so it costs nothing later. */
} fan_id_t;

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

/* Field scale sanity band, milli-counts per millimetre. A correction file
 * outside this implies a field smaller than 33 mm or larger than 300 mm, which
 * is not this machine. A scale of exactly 1000 (1.0 counts/mm) is the known
 * placeholder written by the old tooling — reject it loudly and propose the
 * scale implied by the table rather than silently accepting or fixing it. */
#define FIELD_SCALE_MIN_MCPMM  218453UL  /* 65536/300 -> 300 mm field */
#define FIELD_SCALE_MAX_MCPMM 1986061UL  /* 65536/33  ->  33 mm field */

/* --- Purge --------------------------------------------------------------- */

/* Stage order is physical, not arbitrary — see PROTOCOL.md. */
typedef enum {
    PURGE_STAGE_IDLE     = 0x00,
    PURGE_STAGE_DISPLACE = 0x01,  /* solenoid open, blower OFF */
    PURGE_STAGE_MIX      = 0x02,  /* solenoid open, blower on */
    PURGE_STAGE_VERIFY   = 0x03,  /* solenoid shut, blower on */
} purge_stage_t;

typedef enum {
    PURGE_RESULT_NONE   = 0x00,  /* nothing has concluded */
    PURGE_RESULT_PASSED = 0x01,
    PURGE_RESULT_FAILED = 0x02,  /* O2 climbed back once the solenoid shut */
} purge_result_t;

/* fan_set_t.mode */
typedef enum {
    FAN_MODE_OFF       = 0x00,
    FAN_MODE_MANUAL    = 0x01,  /* hold duty_pm exactly */
    FAN_MODE_MAPPED    = 0x02,  /* duty follows scan speed. NOT IMPLEMENTED
                                 * anywhere: faster scanning throws more
                                 * spatter and wants more flow, so the idea is
                                 * sound, but no node that owns a fan currently
                                 * sees scan speed. It becomes implementable
                                 * once the Teensy tells the airflow node what
                                 * speed it is marking at. Refused until then,
                                 * never silently accepted. */
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
/* The Mega executes the whole sequence on one message; it owns the axes and
 * the limit switches, and the cycle must complete even if the link hiccups.
 * Order matters and is documented in PROTOCOL.md. */
typedef struct {
    int32_t  feed_um;        /* powder piston rise */
    int32_t  bed_um;         /* build platform drop (negative = down) */
    uint16_t wipe_speed_mm_s;
    uint16_t settle_ms;      /* pause after the pistons move, before the sweep.
                              * 2000 in the old firmware; the reason is no
                              * longer remembered, so measure before reducing. */
    int32_t  clearance_um;   /* PARK_SUPPLY only: extra bed drop so the return
                              * traverse clears freshly spread powder */
    uint8_t  passes;
    uint8_t  park_mode;      /* park_mode_t */
} recoat_cycle_t;

/* MSG_SENSOR_REPORT — the Mega's periodic process snapshot.
 * valid_mask bit0..bit1: oxygen[0..1], bit8..bit13: temp_c_x10[0..5]. */
typedef struct {
    uint16_t oxygen_ppm[2];
    int16_t  temp_c_x10[6];
    uint16_t valid_mask;
} sensor_report_t;

/* MSG_PURGE_SET — starts or aborts the three-stage purge. See PROTOCOL.md;
 * the stage order is physical, not arbitrary. */
typedef struct {
    uint8_t  enable;         /* 0 = abort and close the solenoid, 1 = start */
    uint8_t  flags;          /* PURGE_FLAG_* */
    uint16_t target_o2_ppm;  /* stage 2 aim; 0 = the node's default */
    uint16_t timeout_s;      /* bounds stage 2; 0 = the node's default */
    uint16_t min_mix_s;      /* shortest stage 2 before the O2 reading may end
                              * it; 0 = the node's default. A settings page
                              * owns this — it is dialled in per machine and
                              * per gas, not per job. */
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

/* ===== Job file layout ====================================================
 * These are NOT packet payloads. They are the on-card format the PC generates
 * and the Teensy streams layers from, defined here so both sides agree.
 * MSG_JOB_UPLOAD_DATA carries these bytes opaquely.
 *
 *   job_file_header_t
 *   per layer:
 *     layer_header_t
 *     per parameter group:
 *       vector_group_t
 *       laser_params_t
 *       vector_point_t[point_count]
 * ======================================================================== */

typedef struct {
    char     magic[8];           /* "MOIRENJB" */
    uint16_t format_version;
    uint16_t layer_count;
    uint32_t job_id;
    int32_t  layer_thickness_um;
    uint32_t total_bytes;        /* everything after this header */
    uint16_t file_crc;           /* CRC-16 over everything after this header */
    uint16_t reserved;
} job_file_header_t;

/* Per layer. `crc` is checked at READ time, not just at upload: a card can
 * develop bad sectors long after a correct write, so this is the check that
 * actually protects a print. A layer that fails it faults before anything is
 * melted. */
typedef struct {
    uint16_t layer_index;
    uint16_t group_count;
    uint32_t byte_count;   /* bytes of this layer following this header */
    int32_t  z_um;         /* absolute build platform position for this layer */
    uint16_t crc;          /* CRC-16 over this layer's groups and points */
    uint16_t reserved;
} layer_header_t;

/* One run of points sharing a laser_params_t, which follows immediately after
 * this record. Grouping this way costs one parameter record per contour rather
 * than a power field on every point — most vectors in a layer share settings,
 * and a curved contour holds constant power throughout. */
typedef struct {
    uint16_t point_count;
    uint16_t reserved;
} vector_group_t;

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

/* MSG_JOB_UPLOAD_BEGIN — announce a job file about to be written to the card.
 *
 * The DATA messages carry the file VERBATIM, starting with its own
 * job_file_header_t, so what lands on the card is a complete job file that can
 * still be identified after a reboot without any of this context.
 *
 * total_bytes means exactly what job_file_header_t.total_bytes means —
 * everything AFTER that header — so the two compare directly and neither
 * acquires a second definition. The bytes actually transferred are therefore
 * sizeof(job_file_header_t) + total_bytes, and file_crc covers the same span as
 * its namesake: the body, not the header. */
typedef struct {
    uint32_t job_id;
    uint32_t total_bytes;   /* body bytes, excluding job_file_header_t */
    uint16_t layer_count;
    uint16_t file_crc;      /* must match job_file_header_t.file_crc */
} job_upload_begin_t;

/* MSG_JOB_UPLOAD_DATA — followed by byte_count opaque file bytes.
 * chunk_index is 0-based and must arrive in order; a gap is detected
 * immediately rather than silently shifting the rest of the file. */
typedef struct {
    uint32_t chunk_index;
    uint16_t byte_count;
    uint16_t reserved;
} job_upload_data_t;

/* MSG_JOB_UPLOAD_END — commit. The node verifies the byte count and the file
 * CRC and only then marks the job valid on the card; a failed transfer never
 * becomes a printable job. The ACK carries the result. */
typedef struct {
    uint32_t job_id;
} job_upload_end_t;

/* MSG_FIELD_CORRECTION_BEGIN */
typedef struct {
    uint8_t  grid_size;     /* 65 */
    uint8_t  reserved;
    uint16_t point_total;   /* grid_size * grid_size, i.e. 4225 */
    uint32_t scale_mcpmm;   /* field scale, milli-counts per millimetre.
                             * 374500 = 374.5 counts/mm = a 175 mm lens.
                             * Checked against FIELD_SCALE_MIN/MAX_MCPMM. */
    uint16_t table_crc;     /* CRC-16 over all entries, in order */
    uint16_t reserved2;
} field_corr_begin_t;

/* MSG_FIELD_CORRECTION_DATA — followed by point_count field_corr_point_t. */
typedef struct {
    uint16_t chunk_index;   /* 0-based, must arrive in order */
    uint16_t point_count;
} field_corr_data_t;

/* One grid node. Ordinary signed offsets: the .cor file on disk stores
 * sign-magnitude with bit 15 as a sign flag, and that is decoded once when the
 * file is read — never inside the interpolation path. */
typedef struct {
    int16_t dx;
    int16_t dy;
} field_corr_point_t;

/* MSG_FAN_SET */
typedef struct {
    uint8_t  fan;               /* fan_id_t */
    uint8_t  mode;              /* fan_mode_t */
    uint16_t duty_pm;           /* FAN_MODE_MANUAL: 0..1000 */
    uint16_t target_flow_cm_s;  /* FAN_MODE_CLOSEDLOOP only */
} fan_set_t;

/* MSG_FAN_STATUS */
typedef struct {
    uint8_t  fan;         /* fan_id_t */
    uint8_t  mode;        /* fan_mode_t */
    uint16_t duty_pm;
    uint16_t rpm;         /* 0 when no tach is fitted */
    uint16_t flow_cm_s;   /* 0 when no flow sensor is fitted */
} fan_status_t;

/* MSG_LIGHT_SET — chamber lighting.
 *
 * Sent with no payload it is a query, and the node answers with the mode
 * currently selected. That is the same convention MSG_SENSOR_REPORT,
 * MSG_AXIS_STATUS and MSG_FAN_STATUS use.
 *
 * How long to wait after switching before a capture is worth taking is NOT
 * here: it is a property of the camera, it is dialled in once, and it belongs
 * in mega_settings_t.light_settle_ms where it can be read back. Carrying it on
 * every switch would have been a value the node had to either ignore or
 * silently adopt, and neither is honest. */
typedef struct {
    uint8_t mode;      /* light_mode_t */
    uint8_t reserved;
} light_set_t;

/* MSG_SENSOR_OVERRIDE — which channels are being substituted, and what the
 * hardware actually reads underneath.
 *
 * Overrides are compile-time on the sensing node so they cannot be set by
 * accident, but they MUST be visible: this message lets the UI show the
 * substituted value next to the real one, in red. Sent on connect and on
 * change only — never on the periodic report, which would spend bandwidth ten
 * times a second on something that almost never changes.
 *
 * Bit layout matches sensor_report_t.valid_mask: bit0..1 oxygen[0..1],
 * bit8..13 temp[0..5]. */
typedef struct {
    uint16_t override_mask;      /* set bits are substituted, not measured */
    uint16_t oxygen_true_ppm[2]; /* what the sensor actually reads */
    int16_t  temp_true_c_x10[6];
} sensor_override_t;

/* MSG_PURGE_STATUS — published while a purge runs and once when it ends.
 * A purge can take the better part of an hour, so "busy" is not a useful
 * answer; oxygen against the target over time is. */
typedef struct {
    uint8_t  stage;       /* purge_stage_t */
    uint8_t  result;      /* purge_result_t — meaningful once stage is IDLE */
    uint16_t o2_ppm;      /* worst of the oxygen channels */
    uint16_t target_ppm;
    uint16_t elapsed_s;   /* since the purge started */
    uint32_t argon_ml;    /* solenoid-open time x the configured flow rate.
                           * An estimate from a regulator setting, not a
                           * measurement — a real flow meter replaces it. */
} purge_status_t;

/* MSG_SETTINGS / MSG_SETTINGS_SET, for NODE_ARDUINO_MEGA.
 *
 * Everything here is dialled in once per machine and then rarely touched, and
 * every one of them is something the firmware cannot know for itself: they
 * depend on the gas, the regulator, the camera and the powder. The node keeps
 * them in non-volatile memory.
 *
 * A zero in the equivalent field of a command (purge_set_t, recoat_cycle_t,
 * light_set_t) means "use the stored setting", so a one-off run can still
 * override without disturbing what the settings page holds. */
typedef struct {
    uint16_t purge_target_o2_ppm;  /* stage 2 aim */
    uint16_t purge_timeout_s;      /* bounds stage 2 */
    uint16_t purge_min_mix_s;      /* shortest stage 2 the O2 reading may end */
    uint16_t light_settle_ms[3];   /* indexed by light_mode_t */
    uint16_t recoat_settle_ms;     /* after the pistons, before the sweep */
    uint16_t argon_flow_ml_min;    /* regulator dependent; calibrate per machine */
} mega_settings_t;

/* MSG_TIMING_OFFSET — systematic lead/lag between the laser power stream and
 * the galvo position stream, in galvo samples. Signed: positive emits power
 * ahead of position.
 *
 * Only the DIFFERENCE between the two path delays matters; equal delay on both
 * shifts everything uniformly and is invisible in the part. Distinct from the
 * commanded dwells in laser_params_t: this is a per-machine hardware
 * correction, measured once on the bench. */
typedef struct {
    int16_t laser_lead_samples;
    int16_t reserved;
} timing_offset_t;

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

/* How much of each bulk-transfer payload is usable. */
#define FIELD_CORR_MAX_POINTS \
    ((PACKET_MAX_PAYLOAD - (int)sizeof(field_corr_data_t)) / (int)sizeof(field_corr_point_t))
#define JOB_UPLOAD_MAX_BYTES \
    (PACKET_MAX_PAYLOAD - (int)sizeof(job_upload_data_t))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOIREN_PROTOCOL_H */
