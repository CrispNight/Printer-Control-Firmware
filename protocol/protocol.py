"""Moiren SLM machine communication protocol — Python bindings.

GENERATED FILE — DO NOT EDIT.
Generated from protocol/protocol.h by tools/gen_protocol.py.
Edit the header, then run `python tools/gen_protocol.py`.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import ClassVar, Tuple

SOURCE_HASH = "c0eb69a57259c168"  # sha256 of the generated region of protocol.h


# --- Constants -----------------------------------------------------------

PROTOCOL_VERSION = 1
PACKET_SOF0 = 0xA5
PACKET_SOF1 = 0x5A
PACKET_HEADER_LEN = 9
PACKET_CRC_LEN = 2
PACKET_MAX_PAYLOAD = 0xC0
PACKET_MAX_LEN = 0xCB
FLAG_NEEDS_ACK = 1
FLAG_IS_RESPONSE = 2
FLAG_IS_ERROR = 4
FLAG_NO_ROUTE = 8
NODE_AIRFLOW = 3
AXIS_FLAG_HOMED = 1
AXIS_FLAG_MOVING = 2
AXIS_FLAG_AT_LIMIT = 4
AXIS_FLAG_ENABLED = 8
AXIS_FLAG_FAULT = 0x10
FAULTBIT_DOOR = 1
FAULTBIT_OXYGEN = 2
FAULTBIT_TEMP = 4
FAULTBIT_MOTION = 8
FAULTBIT_LASER = 0x10
FAULTBIT_GALVO = 0x20
FAULTBIT_AIRFLOW = 0x40
FAULTBIT_COMMS = 0x80
FAULTBIT_ESTOP = 0x100
LASER_ARM_KEY = 0x4D4F4152
POINT_FLAG_LASER_ON = 1
POINT_FLAG_LAST = 2
BATCH_FLAG_FIRST = 1
BATCH_FLAG_LAST = 2


# --- Enums ---------------------------------------------------------------


class NodeId(IntEnum):
    """C `node_id_t`."""

    BROADCAST = 0x00
    PC = 0x01
    TEENSY_GALVO = 0x02
    ARDUINO_MEGA = 0x03
    ESP32_FAN = 0x04


class MsgId(IntEnum):
    """C `msg_id_t`."""

    PING = 0x01
    PONG = 0x02
    HELLO = 0x03
    ACK = 0x04
    LOG = 0x05
    HEARTBEAT = 0x06
    RESET = 0x07
    STATE_REQUEST = 0x10
    STATE = 0x11
    JOB_START = 0x12
    JOB_ABORT = 0x13
    JOB_PAUSE = 0x14
    JOB_RESUME = 0x15
    LAYER_BEGIN = 0x16
    LAYER_END = 0x17
    JOB_COMPLETE = 0x18
    SAFETY_STATUS = 0x20
    ESTOP = 0x21
    FAULT = 0x22
    FAULT_CLEAR = 0x23
    AXIS_HOME = 0x30
    AXIS_MOVE = 0x31
    AXIS_STOP = 0x32
    AXIS_STATUS = 0x33
    RECOAT_CYCLE = 0x34
    SENSOR_REPORT = 0x40
    PURGE_SET = 0x41
    LASER_ARM = 0x50
    LASER_PARAMS = 0x51
    MARK_BATCH = 0x52
    MARK_ABORT = 0x53
    GALVO_STATUS = 0x54
    FAN_SET = 0x60
    FAN_STATUS = 0x61


class MachineState(IntEnum):
    """C `machine_state_e`."""

    BOOT = 0x00
    IDLE = 0x01
    HOMING = 0x02
    PURGING = 0x03
    READY = 0x04
    PRINTING = 0x05
    PAUSED = 0x06
    FAULT = 0x07
    ESTOP = 0x08


class AxisId(IntEnum):
    """C `axis_id_t`."""

    FEED = 0x00
    BED = 0x01
    WIPE = 0x02


class FaultCode(IntEnum):
    """C `fault_code_t`."""

    NONE = 0x00
    DOOR_OPEN = 0x01
    OXYGEN_HIGH = 0x02
    OVERTEMP = 0x03
    AXIS_STALL = 0x04
    LIMIT_UNEXPECT = 0x05
    LASER_INTERLOCK = 0x06
    LASER_FAULT = 0x07
    GALVO_FAULT = 0x08
    FAN_STALL = 0x09
    COMMS_TIMEOUT = 0x0A
    PROTOCOL_ERROR = 0x0B
    VERSION_MISMATCH = 0x0C
    SENSOR_INVALID = 0x0D
    INTERNAL = 0xFF


class LogLevel(IntEnum):
    """C `log_level_t`."""

    DEBUG = 0x00
    INFO = 0x01
    WARN = 0x02
    ERROR = 0x03


class AckStatus(IntEnum):
    """C `ack_status_t`."""

    OK = 0x00
    BAD_CRC = 0x01
    BAD_LENGTH = 0x02
    UNKNOWN_MSG = 0x03
    BAD_STATE = 0x04
    BAD_PARAM = 0x05
    BUSY = 0x06
    REFUSED = 0x07


class LaserState(IntEnum):
    """C `laser_state_t`."""

    DISARMED = 0x00
    ARMED = 0x01
    MARKING = 0x02
    FAULT = 0x03


class FanMode(IntEnum):
    """C `fan_mode_t`."""

    OFF = 0x00
    MANUAL = 0x01
    MAPPED = 0x02
    CLOSEDLOOP = 0x03


# --- Payload structs -----------------------------------------------------
# Wire encoding is little-endian and packed, matching #pragma pack(1) in the
# header. SIZE is the exact on-wire byte count of the payload.


_S_SYSHELLO = struct.Struct("<BBBBBBI16s")


@dataclass
class SysHello:
    """C `sys_hello_t` — 26 bytes on the wire."""

    node: int = 0
    proto_version: int = 0
    fw_major: int = 0
    fw_minor: int = 0
    fw_patch: int = 0
    reserved: int = 0
    capabilities: int = 0
    build_id: bytes = b""

    FORMAT: ClassVar[str] = "<BBBBBBI16s"
    SIZE: ClassVar[int] = 26

    def pack(self) -> bytes:
        return _S_SYSHELLO.pack(
            self.node,
            self.proto_version,
            self.fw_major,
            self.fw_minor,
            self.fw_patch,
            self.reserved,
            self.capabilities,
            self.build_id.ljust(16, b'\x00')[:16],
        )

    @classmethod
    def unpack(cls, data: bytes) -> "SysHello":
        v = _S_SYSHELLO.unpack(data[: cls.SIZE])
        return cls(
            node=v[0],
            proto_version=v[1],
            fw_major=v[2],
            fw_minor=v[3],
            fw_patch=v[4],
            reserved=v[5],
            capabilities=v[6],
            build_id=v[7],
        )


_S_SYSHEARTBEAT = struct.Struct("<BBHI")


@dataclass
class SysHeartbeat:
    """C `sys_heartbeat_t` — 8 bytes on the wire."""

    node: int = 0
    state: int = 0
    fault_flags: int = 0
    uptime_ms: int = 0

    FORMAT: ClassVar[str] = "<BBHI"
    SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        return _S_SYSHEARTBEAT.pack(
            self.node,
            self.state,
            self.fault_flags,
            self.uptime_ms,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "SysHeartbeat":
        v = _S_SYSHEARTBEAT.unpack(data[: cls.SIZE])
        return cls(
            node=v[0],
            state=v[1],
            fault_flags=v[2],
            uptime_ms=v[3],
        )


_S_SYSACK = struct.Struct("<BBB")


@dataclass
class SysAck:
    """C `sys_ack_t` — 3 bytes on the wire."""

    ack_msg: int = 0
    ack_seq: int = 0
    status: int = 0

    FORMAT: ClassVar[str] = "<BBB"
    SIZE: ClassVar[int] = 3

    def pack(self) -> bytes:
        return _S_SYSACK.pack(
            self.ack_msg,
            self.ack_seq,
            self.status,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "SysAck":
        v = _S_SYSACK.unpack(data[: cls.SIZE])
        return cls(
            ack_msg=v[0],
            ack_seq=v[1],
            status=v[2],
        )


_S_SYSLOG = struct.Struct("<B48s")


@dataclass
class SysLog:
    """C `sys_log_t` — 49 bytes on the wire."""

    level: int = 0
    text: bytes = b""

    FORMAT: ClassVar[str] = "<B48s"
    SIZE: ClassVar[int] = 49

    def pack(self) -> bytes:
        return _S_SYSLOG.pack(
            self.level,
            self.text.ljust(48, b'\x00')[:48],
        )

    @classmethod
    def unpack(cls, data: bytes) -> "SysLog":
        v = _S_SYSLOG.unpack(data[: cls.SIZE])
        return cls(
            level=v[0],
            text=v[1],
        )


_S_STATEREPORT = struct.Struct("<BBHHHI")


@dataclass
class StateReport:
    """C `state_report_t` — 12 bytes on the wire."""

    state: int = 0
    substate: int = 0
    fault_flags: int = 0
    layer_index: int = 0
    layer_total: int = 0
    elapsed_s: int = 0

    FORMAT: ClassVar[str] = "<BBHHHI"
    SIZE: ClassVar[int] = 12

    def pack(self) -> bytes:
        return _S_STATEREPORT.pack(
            self.state,
            self.substate,
            self.fault_flags,
            self.layer_index,
            self.layer_total,
            self.elapsed_s,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "StateReport":
        v = _S_STATEREPORT.unpack(data[: cls.SIZE])
        return cls(
            state=v[0],
            substate=v[1],
            fault_flags=v[2],
            layer_index=v[3],
            layer_total=v[4],
            elapsed_s=v[5],
        )


_S_JOBSTART = struct.Struct("<IHHi")


@dataclass
class JobStart:
    """C `job_start_t` — 12 bytes on the wire."""

    job_id: int = 0
    layer_total: int = 0
    reserved: int = 0
    layer_thickness_um: int = 0

    FORMAT: ClassVar[str] = "<IHHi"
    SIZE: ClassVar[int] = 12

    def pack(self) -> bytes:
        return _S_JOBSTART.pack(
            self.job_id,
            self.layer_total,
            self.reserved,
            self.layer_thickness_um,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "JobStart":
        v = _S_JOBSTART.unpack(data[: cls.SIZE])
        return cls(
            job_id=v[0],
            layer_total=v[1],
            reserved=v[2],
            layer_thickness_um=v[3],
        )


_S_LAYERBEGIN = struct.Struct("<HHi")


@dataclass
class LayerBegin:
    """C `layer_begin_t` — 8 bytes on the wire."""

    layer_index: int = 0
    reserved: int = 0
    z_um: int = 0

    FORMAT: ClassVar[str] = "<HHi"
    SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        return _S_LAYERBEGIN.pack(
            self.layer_index,
            self.reserved,
            self.z_um,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "LayerBegin":
        v = _S_LAYERBEGIN.unpack(data[: cls.SIZE])
        return cls(
            layer_index=v[0],
            reserved=v[1],
            z_um=v[2],
        )


_S_LAYEREND = struct.Struct("<HHI")


@dataclass
class LayerEnd:
    """C `layer_end_t` — 8 bytes on the wire."""

    layer_index: int = 0
    fault_flags: int = 0
    duration_ms: int = 0

    FORMAT: ClassVar[str] = "<HHI"
    SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        return _S_LAYEREND.pack(
            self.layer_index,
            self.fault_flags,
            self.duration_ms,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "LayerEnd":
        v = _S_LAYEREND.unpack(data[: cls.SIZE])
        return cls(
            layer_index=v[0],
            fault_flags=v[1],
            duration_ms=v[2],
        )


_S_SAFETYSTATUS = struct.Struct("<HHBBBB")


@dataclass
class SafetyStatus:
    """C `safety_status_t` — 8 bytes on the wire."""

    interlock_mask: int = 0
    tripped_mask: int = 0
    door_ok: int = 0
    oxygen_ok: int = 0
    temp_ok: int = 0
    estop_active: int = 0

    FORMAT: ClassVar[str] = "<HHBBBB"
    SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        return _S_SAFETYSTATUS.pack(
            self.interlock_mask,
            self.tripped_mask,
            self.door_ok,
            self.oxygen_ok,
            self.temp_ok,
            self.estop_active,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "SafetyStatus":
        v = _S_SAFETYSTATUS.unpack(data[: cls.SIZE])
        return cls(
            interlock_mask=v[0],
            tripped_mask=v[1],
            door_ok=v[2],
            oxygen_ok=v[3],
            temp_ok=v[4],
            estop_active=v[5],
        )


_S_FAULTREPORT = struct.Struct("<BBH")


@dataclass
class FaultReport:
    """C `fault_report_t` — 4 bytes on the wire."""

    code: int = 0
    node: int = 0
    detail: int = 0

    FORMAT: ClassVar[str] = "<BBH"
    SIZE: ClassVar[int] = 4

    def pack(self) -> bytes:
        return _S_FAULTREPORT.pack(
            self.code,
            self.node,
            self.detail,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "FaultReport":
        v = _S_FAULTREPORT.unpack(data[: cls.SIZE])
        return cls(
            code=v[0],
            node=v[1],
            detail=v[2],
        )


_S_AXISHOME = struct.Struct("<BB")


@dataclass
class AxisHome:
    """C `axis_home_t` — 2 bytes on the wire."""

    axis: int = 0
    flags: int = 0

    FORMAT: ClassVar[str] = "<BB"
    SIZE: ClassVar[int] = 2

    def pack(self) -> bytes:
        return _S_AXISHOME.pack(
            self.axis,
            self.flags,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "AxisHome":
        v = _S_AXISHOME.unpack(data[: cls.SIZE])
        return cls(
            axis=v[0],
            flags=v[1],
        )


_S_AXISMOVE = struct.Struct("<BBiII")


@dataclass
class AxisMove:
    """C `axis_move_t` — 14 bytes on the wire."""

    axis: int = 0
    flags: int = 0
    target_um: int = 0
    speed_um_s: int = 0
    accel_um_s2: int = 0

    FORMAT: ClassVar[str] = "<BBiII"
    SIZE: ClassVar[int] = 14

    def pack(self) -> bytes:
        return _S_AXISMOVE.pack(
            self.axis,
            self.flags,
            self.target_um,
            self.speed_um_s,
            self.accel_um_s2,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "AxisMove":
        v = _S_AXISMOVE.unpack(data[: cls.SIZE])
        return cls(
            axis=v[0],
            flags=v[1],
            target_um=v[2],
            speed_um_s=v[3],
            accel_um_s2=v[4],
        )


_S_AXISSTATUS = struct.Struct("<BBii")


@dataclass
class AxisStatus:
    """C `axis_status_t` — 10 bytes on the wire."""

    axis: int = 0
    flags: int = 0
    position_um: int = 0
    target_um: int = 0

    FORMAT: ClassVar[str] = "<BBii"
    SIZE: ClassVar[int] = 10

    def pack(self) -> bytes:
        return _S_AXISSTATUS.pack(
            self.axis,
            self.flags,
            self.position_um,
            self.target_um,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "AxisStatus":
        v = _S_AXISSTATUS.unpack(data[: cls.SIZE])
        return cls(
            axis=v[0],
            flags=v[1],
            position_um=v[2],
            target_um=v[3],
        )


_S_RECOATCYCLE = struct.Struct("<iiHBB")


@dataclass
class RecoatCycle:
    """C `recoat_cycle_t` — 12 bytes on the wire."""

    feed_um: int = 0
    bed_um: int = 0
    wipe_speed_mm_s: int = 0
    passes: int = 0
    flags: int = 0

    FORMAT: ClassVar[str] = "<iiHBB"
    SIZE: ClassVar[int] = 12

    def pack(self) -> bytes:
        return _S_RECOATCYCLE.pack(
            self.feed_um,
            self.bed_um,
            self.wipe_speed_mm_s,
            self.passes,
            self.flags,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "RecoatCycle":
        v = _S_RECOATCYCLE.unpack(data[: cls.SIZE])
        return cls(
            feed_um=v[0],
            bed_um=v[1],
            wipe_speed_mm_s=v[2],
            passes=v[3],
            flags=v[4],
        )


_S_SENSORREPORT = struct.Struct("<2H6hH")


@dataclass
class SensorReport:
    """C `sensor_report_t` — 18 bytes on the wire."""

    oxygen_ppm: Tuple[int, ...] = field(default_factory=tuple)
    temp_c_x10: Tuple[int, ...] = field(default_factory=tuple)
    valid_mask: int = 0

    FORMAT: ClassVar[str] = "<2H6hH"
    SIZE: ClassVar[int] = 18

    def pack(self) -> bytes:
        return _S_SENSORREPORT.pack(
            *tuple(self.oxygen_ppm)[:2],
            *tuple(self.temp_c_x10)[:6],
            self.valid_mask,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "SensorReport":
        v = _S_SENSORREPORT.unpack(data[: cls.SIZE])
        return cls(
            oxygen_ppm=tuple(v[0:2]),
            temp_c_x10=tuple(v[2:8]),
            valid_mask=v[8],
        )


_S_PURGESET = struct.Struct("<BBHH")


@dataclass
class PurgeSet:
    """C `purge_set_t` — 6 bytes on the wire."""

    enable: int = 0
    reserved: int = 0
    target_o2_ppm: int = 0
    timeout_s: int = 0

    FORMAT: ClassVar[str] = "<BBHH"
    SIZE: ClassVar[int] = 6

    def pack(self) -> bytes:
        return _S_PURGESET.pack(
            self.enable,
            self.reserved,
            self.target_o2_ppm,
            self.timeout_s,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "PurgeSet":
        v = _S_PURGESET.unpack(data[: cls.SIZE])
        return cls(
            enable=v[0],
            reserved=v[1],
            target_o2_ppm=v[2],
            timeout_s=v[3],
        )


_S_LASERARM = struct.Struct("<BBI")


@dataclass
class LaserArm:
    """C `laser_arm_t` — 6 bytes on the wire."""

    arm: int = 0
    reserved: int = 0
    key: int = 0

    FORMAT: ClassVar[str] = "<BBI"
    SIZE: ClassVar[int] = 6

    def pack(self) -> bytes:
        return _S_LASERARM.pack(
            self.arm,
            self.reserved,
            self.key,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "LaserArm":
        v = _S_LASERARM.unpack(data[: cls.SIZE])
        return cls(
            arm=v[0],
            reserved=v[1],
            key=v[2],
        )


_S_LASERPARAMS = struct.Struct("<HIHHHHHH")


@dataclass
class LaserParams:
    """C `laser_params_t` — 18 bytes on the wire."""

    power_pm: int = 0
    freq_hz: int = 0
    pulse_width_ns: int = 0
    mark_speed_mm_s: int = 0
    jump_speed_mm_s: int = 0
    on_delay_us: int = 0
    off_delay_us: int = 0
    poly_delay_us: int = 0

    FORMAT: ClassVar[str] = "<HIHHHHHH"
    SIZE: ClassVar[int] = 18

    def pack(self) -> bytes:
        return _S_LASERPARAMS.pack(
            self.power_pm,
            self.freq_hz,
            self.pulse_width_ns,
            self.mark_speed_mm_s,
            self.jump_speed_mm_s,
            self.on_delay_us,
            self.off_delay_us,
            self.poly_delay_us,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "LaserParams":
        v = _S_LASERPARAMS.unpack(data[: cls.SIZE])
        return cls(
            power_pm=v[0],
            freq_hz=v[1],
            pulse_width_ns=v[2],
            mark_speed_mm_s=v[3],
            jump_speed_mm_s=v[4],
            on_delay_us=v[5],
            off_delay_us=v[6],
            poly_delay_us=v[7],
        )


_S_MARKBATCHHEADER = struct.Struct("<HHHBB")


@dataclass
class MarkBatchHeader:
    """C `mark_batch_header_t` — 8 bytes on the wire."""

    layer_index: int = 0
    batch_index: int = 0
    count: int = 0
    flags: int = 0
    reserved: int = 0

    FORMAT: ClassVar[str] = "<HHHBB"
    SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        return _S_MARKBATCHHEADER.pack(
            self.layer_index,
            self.batch_index,
            self.count,
            self.flags,
            self.reserved,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "MarkBatchHeader":
        v = _S_MARKBATCHHEADER.unpack(data[: cls.SIZE])
        return cls(
            layer_index=v[0],
            batch_index=v[1],
            count=v[2],
            flags=v[3],
            reserved=v[4],
        )


_S_VECTORPOINT = struct.Struct("<iiB")


@dataclass
class VectorPoint:
    """C `vector_point_t` — 9 bytes on the wire."""

    x_um: int = 0
    y_um: int = 0
    flags: int = 0

    FORMAT: ClassVar[str] = "<iiB"
    SIZE: ClassVar[int] = 9

    def pack(self) -> bytes:
        return _S_VECTORPOINT.pack(
            self.x_um,
            self.y_um,
            self.flags,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "VectorPoint":
        v = _S_VECTORPOINT.unpack(data[: cls.SIZE])
        return cls(
            x_um=v[0],
            y_um=v[1],
            flags=v[2],
        )


_S_GALVOSTATUS = struct.Struct("<BBiiIH")


@dataclass
class GalvoStatus:
    """C `galvo_status_t` — 16 bytes on the wire."""

    laser_state: int = 0
    flags: int = 0
    x_um: int = 0
    y_um: int = 0
    points_remaining: int = 0
    fault_flags: int = 0

    FORMAT: ClassVar[str] = "<BBiiIH"
    SIZE: ClassVar[int] = 16

    def pack(self) -> bytes:
        return _S_GALVOSTATUS.pack(
            self.laser_state,
            self.flags,
            self.x_um,
            self.y_um,
            self.points_remaining,
            self.fault_flags,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "GalvoStatus":
        v = _S_GALVOSTATUS.unpack(data[: cls.SIZE])
        return cls(
            laser_state=v[0],
            flags=v[1],
            x_um=v[2],
            y_um=v[3],
            points_remaining=v[4],
            fault_flags=v[5],
        )


_S_FANSET = struct.Struct("<BBHH")


@dataclass
class FanSet:
    """C `fan_set_t` — 6 bytes on the wire."""

    mode: int = 0
    reserved: int = 0
    duty_pm: int = 0
    target_flow_cm_s: int = 0

    FORMAT: ClassVar[str] = "<BBHH"
    SIZE: ClassVar[int] = 6

    def pack(self) -> bytes:
        return _S_FANSET.pack(
            self.mode,
            self.reserved,
            self.duty_pm,
            self.target_flow_cm_s,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "FanSet":
        v = _S_FANSET.unpack(data[: cls.SIZE])
        return cls(
            mode=v[0],
            reserved=v[1],
            duty_pm=v[2],
            target_flow_cm_s=v[3],
        )


_S_FANSTATUS = struct.Struct("<BBHHH")


@dataclass
class FanStatus:
    """C `fan_status_t` — 8 bytes on the wire."""

    mode: int = 0
    flags: int = 0
    duty_pm: int = 0
    rpm: int = 0
    flow_cm_s: int = 0

    FORMAT: ClassVar[str] = "<BBHHH"
    SIZE: ClassVar[int] = 8

    def pack(self) -> bytes:
        return _S_FANSTATUS.pack(
            self.mode,
            self.flags,
            self.duty_pm,
            self.rpm,
            self.flow_cm_s,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "FanStatus":
        v = _S_FANSTATUS.unpack(data[: cls.SIZE])
        return cls(
            mode=v[0],
            flags=v[1],
            duty_pm=v[2],
            rpm=v[3],
            flow_cm_s=v[4],
        )



# --- Packet framing -------------------------------------------------------------
# Layout (little-endian):
#   SOF0 SOF1 VER SRC DST MSG FLAGS SEQ LEN  payload[LEN]  CRC16
#   0    1    2   3   4   5   6     7   8    9..           last two bytes
# CRC-16/CCITT-FALSE over VER..payload inclusive, i.e. bytes [2, 9+LEN).

_PACKET_HEADER = struct.Struct("<BBBBBBBBB")


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE. Byte-for-byte mirror of crc16_ccitt() in protocol.h."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass
class Packet:
    """One decoded protocol packet."""

    src: int = 0
    dst: int = 0
    msg: int = 0
    flags: int = 0
    seq: int = 0
    payload: bytes = b""
    version: int = PROTOCOL_VERSION

    def pack(self) -> bytes:
        if len(self.payload) > PACKET_MAX_PAYLOAD:
            raise ValueError(
                f"payload of {len(self.payload)} bytes exceeds "
                f"PACKET_MAX_PAYLOAD ({PACKET_MAX_PAYLOAD})"
            )
        header = _PACKET_HEADER.pack(
            PACKET_SOF0,
            PACKET_SOF1,
            self.version,
            self.src,
            self.dst,
            self.msg,
            self.flags,
            self.seq,
            len(self.payload),
        )
        body = header[2:] + self.payload
        return header + self.payload + struct.pack("<H", crc16_ccitt(body))


_NEED_MORE = object()  # decoder needs more bytes before it can decide


class PacketDecoder:
    """Incremental byte-stream decoder. Feed it whatever the port hands you.

    Resynchronises on its own: a corrupted packet costs at most one packet, and
    the decoder picks the next valid start-of-packet out of the stream.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self.crc_errors = 0

    def feed(self, data: bytes) -> list["Packet"]:
        self._buf.extend(data)
        packets: list[Packet] = []
        while True:
            result = self._try_decode()
            if result is _NEED_MORE:
                return packets
            if result is not None:
                packets.append(result)
            # result None: a byte was discarded to resync — keep scanning, so a
            # good packet sitting behind a corrupt one is still delivered.

    def _try_decode(self):
        """Packet on success, None after discarding a byte, _NEED_MORE if starved."""
        buf = self._buf
        # Discard anything before a start-of-packet pair.
        while len(buf) >= 2 and not (buf[0] == PACKET_SOF0 and buf[1] == PACKET_SOF1):
            del buf[0]
        if len(buf) < PACKET_HEADER_LEN:
            return _NEED_MORE
        length = buf[8]
        if length > PACKET_MAX_PAYLOAD:
            del buf[0]  # bogus length: this was not a real packet start
            return None
        total = PACKET_HEADER_LEN + length + 2
        if len(buf) < total:
            return _NEED_MORE
        payload = bytes(buf[PACKET_HEADER_LEN : PACKET_HEADER_LEN + length])
        (received,) = struct.unpack("<H", bytes(buf[total - 2 : total]))
        if crc16_ccitt(bytes(buf[2 : PACKET_HEADER_LEN + length])) != received:
            self.crc_errors += 1
            del buf[0]  # resync past this false start
            return None
        packet = Packet(
            version=buf[2],
            src=buf[3],
            dst=buf[4],
            msg=buf[5],
            flags=buf[6],
            seq=buf[7],
            payload=payload,
        )
        del buf[:total]
        return packet
