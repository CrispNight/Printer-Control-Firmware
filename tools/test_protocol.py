#!/usr/bin/env python3
"""Round-trip checks for the generated protocol bindings.

Run directly; no test framework needed:

    python tools/test_protocol.py

These guard the properties the firmware relies on, so a header edit that
breaks the wire format fails here rather than on the machine.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "protocol"))

import protocol as p  # noqa: E402


def check(condition, label):
    if not condition:
        raise AssertionError(label)
    print(f"  ok   {label}")


def test_crc():
    # Standard CRC-16/CCITT-FALSE check value. The C implementation in
    # protocol.h must produce the same number for the same input.
    check(p.crc16_ccitt(b"123456789") == 0x29B1, "CRC-16/CCITT-FALSE check value 0x29B1")


def test_struct_roundtrip():
    report = p.SensorReport(
        oxygen_ppm=(350, 361),
        temp_c_x10=(251, -40, 253, 254, 255, 256),
        valid_mask=0x3F03,
    )
    packed = report.pack()
    check(len(packed) == p.SensorReport.SIZE, f"SensorReport packs to {p.SensorReport.SIZE} bytes")
    check(p.SensorReport.unpack(packed) == report, "SensorReport round-trips")
    check(p.SensorReport.unpack(packed).temp_c_x10[1] == -40, "signed temperatures survive")

    move = p.AxisMove(axis=p.AxisId.BED, flags=0, target_um=-125000,
                      speed_um_s=4000, accel_um_s2=20000)
    check(p.AxisMove.unpack(move.pack()) == move, "AxisMove round-trips (negative target)")

    hello = p.SysHello(node=p.NodeId.TEENSY_GALVO, proto_version=p.PROTOCOL_VERSION,
                       fw_major=0, fw_minor=1, fw_patch=0, build_id=b"deadbee")
    check(p.SysHello.unpack(hello.pack()).build_id.rstrip(b"\0") == b"deadbee",
          "SysHello build_id is NUL-padded and recoverable")


def test_frame_roundtrip():
    payload = p.StateReport(state=p.MachineState.PRINTING, layer_index=17,
                            layer_total=400, elapsed_s=1234).pack()
    frame = p.Frame(src=p.NodeId.TEENSY_GALVO, dst=p.NodeId.PC,
                    msg=p.MsgId.STATE, seq=7, payload=payload)
    wire = frame.pack()
    check(len(wire) == p.FRAME_HEADER_LEN + len(payload) + 2, "frame length is header+payload+crc")
    check(wire[0] == p.FRAME_SOF0 and wire[1] == p.FRAME_SOF1, "frame starts with A5 5A")

    decoded = p.FrameDecoder().feed(wire)
    check(len(decoded) == 1, "one frame decodes to one frame")
    check(decoded[0].msg == p.MsgId.STATE and decoded[0].seq == 7, "header survives")
    check(p.StateReport.unpack(decoded[0].payload).layer_index == 17, "payload survives")


def test_decoder_recovery():
    payload = p.FanSet(mode=p.FanMode.MANUAL, duty_pm=650).pack()
    wire = p.Frame(src=p.NodeId.PC, dst=p.NODE_AIRFLOW, msg=p.MsgId.FAN_SET,
                   payload=payload).pack()

    dec = p.FrameDecoder()
    got = dec.feed(b"\x00\xffnoise\xa5") + dec.feed(wire)
    check(len(got) == 1, "decoder skips leading noise")

    # Split across arbitrary chunk boundaries, as a serial port would deliver it.
    dec = p.FrameDecoder()
    got = [f for i in range(len(wire)) for f in dec.feed(wire[i:i + 1])]
    check(len(got) == 1, "decoder reassembles a byte-at-a-time stream")

    # A corrupt frame must cost exactly one frame, not the link.
    corrupt = bytearray(wire)
    corrupt[10] ^= 0xFF
    dec = p.FrameDecoder()
    got = dec.feed(bytes(corrupt) + wire)
    check(len(got) == 1 and dec.crc_errors == 1,
          "corrupt frame is dropped and the next one still decodes")

    # Back-to-back frames in one read.
    dec = p.FrameDecoder()
    check(len(dec.feed(wire * 3)) == 3, "three concatenated frames decode")


def test_invariants():
    check(p.NODE_AIRFLOW == p.NodeId.ARDUINO_MEGA,
          "NODE_AIRFLOW still points at the Mega (update PROTOCOL.md when it moves)")
    max_points = (p.FRAME_MAX_PAYLOAD - p.MarkBatchHeader.SIZE) // p.VectorPoint.SIZE
    check(max_points == 20, f"MARK_BATCH holds {max_points} points per frame")
    check(p.FRAME_MAX_LEN == p.FRAME_HEADER_LEN + p.FRAME_MAX_PAYLOAD + p.FRAME_CRC_LEN,
          "FRAME_MAX_LEN agrees with its parts")

    oversized = p.Frame(payload=b"\x00" * (p.FRAME_MAX_PAYLOAD + 1))
    try:
        oversized.pack()
    except ValueError:
        print("  ok   oversized payload is refused")
    else:
        raise AssertionError("oversized payload was not refused")


def main():
    tests = [test_crc, test_struct_roundtrip, test_frame_roundtrip,
             test_decoder_recovery, test_invariants]
    for test in tests:
        print(f"{test.__name__}:")
        test()
    print(f"\nAll checks passed (protocol version {p.PROTOCOL_VERSION}, "
          f"source hash {p.SOURCE_HASH}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
