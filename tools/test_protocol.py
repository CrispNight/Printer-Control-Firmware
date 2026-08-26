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


def test_packet_roundtrip():
    payload = p.StateReport(state=p.MachineState.PRINTING, layer_index=17,
                            layer_total=400, elapsed_s=1234).pack()
    packet = p.Packet(src=p.NodeId.TEENSY_GALVO, dst=p.NodeId.PC,
                      msg=p.MsgId.STATE, seq=7, payload=payload)
    wire = packet.pack()
    check(len(wire) == p.PACKET_HEADER_LEN + len(payload) + 2, "packet length is header+payload+crc")
    check(wire[0] == p.PACKET_SOF0 and wire[1] == p.PACKET_SOF1, "packet starts with A5 5A")

    decoded = p.PacketDecoder().feed(wire)
    check(len(decoded) == 1, "one packet decodes to one packet")
    check(decoded[0].msg == p.MsgId.STATE and decoded[0].seq == 7, "header survives")
    check(p.StateReport.unpack(decoded[0].payload).layer_index == 17, "payload survives")


def test_decoder_recovery():
    payload = p.FanSet(mode=p.FanMode.MANUAL, duty_pm=650).pack()
    wire = p.Packet(src=p.NodeId.PC, dst=p.NODE_AIRFLOW, msg=p.MsgId.FAN_SET,
                    payload=payload).pack()

    dec = p.PacketDecoder()
    got = dec.feed(b"\x00\xffnoise\xa5") + dec.feed(wire)
    check(len(got) == 1, "decoder skips leading noise")

    # Split across arbitrary chunk boundaries, as a serial port would deliver it.
    dec = p.PacketDecoder()
    got = [f for i in range(len(wire)) for f in dec.feed(wire[i:i + 1])]
    check(len(got) == 1, "decoder reassembles a byte-at-a-time stream")

    # A corrupt packet must cost exactly one packet, not the link.
    corrupt = bytearray(wire)
    corrupt[10] ^= 0xFF
    dec = p.PacketDecoder()
    got = dec.feed(bytes(corrupt) + wire)
    check(len(got) == 1 and dec.crc_errors == 1,
          "corrupt packet is dropped and the next one still decodes")

    # Back-to-back packets in one read.
    dec = p.PacketDecoder()
    check(len(dec.feed(wire * 3)) == 3, "three concatenated packets decode")


def test_bulk_transfers():
    """The atomic upload paths: field correction table and job file."""
    begin = p.FieldCorrBegin(grid_size=65, point_total=65 * 65,
                             scale_mcpmm=374500, table_crc=0xBEEF)
    check(p.FieldCorrBegin.unpack(begin.pack()) == begin, "FieldCorrBegin round-trips")

    per_packet = (p.PACKET_MAX_PAYLOAD - p.FieldCorrData.SIZE) // p.FieldCorrPoint.SIZE
    packets = -(-65 * 65 // per_packet)
    check(per_packet == 47, f"{per_packet} correction points per packet")
    check(packets == 90, f"a 65x65 table is {packets} packets")

    pt = p.FieldCorrPoint(dx=-1234, dy=5678)
    check(p.FieldCorrPoint.unpack(pt.pack()) == pt,
          "FieldCorrPoint round-trips (plain signed, not sign-magnitude)")

    job = p.JobUploadBegin(job_id=7, total_bytes=1_800_000, layer_count=400,
                           file_crc=0x1234)
    check(p.JobUploadBegin.unpack(job.pack()) == job, "JobUploadBegin round-trips")
    check(p.PACKET_MAX_PAYLOAD - p.JobUploadData.SIZE == 184,
          "184 job bytes per packet")


def test_new_messages():
    off = p.TimingOffset(laser_lead_samples=-37)
    check(p.TimingOffset.unpack(off.pack()).laser_lead_samples == -37,
          "TimingOffset carries a signed lead")

    light = p.LightSet(mode=p.LightMode.SHADOW)
    check(p.LightSet.unpack(light.pack()) == light, "LightSet round-trips")

    st = p.MegaSettings(purge_target_o2_ppm=3000, purge_timeout_s=1800,
                        purge_min_mix_s=60, light_settle_ms=(0, 1500, 1000),
                        recoat_settle_ms=2000, argon_flow_ml_min=10000)
    check(p.MegaSettings.unpack(st.pack()) == st, "MegaSettings round-trips")
    check(p.MegaSettings.unpack(st.pack()).light_settle_ms[p.LightMode.SHADOW] == 1000,
          "light settle is indexed by light_mode_t")

    ps = p.PurgeStatus(stage=p.PurgeStage.MIX, result=p.PurgeResult.NONE,
                       o2_ppm=4200, target_ppm=3000, elapsed_s=900,
                       argon_ml=150000)
    check(p.PurgeStatus.unpack(ps.pack()) == ps, "PurgeStatus round-trips")
    check(p.PurgeStatus.unpack(ps.pack()).argon_ml == 150000,
          "argon_ml is wide enough for a long purge (150 L here)")

    ov = p.SensorOverride(override_mask=0x0001, oxygen_true_ppm=(210000 & 0xFFFF, 0),
                          temp_true_c_x10=(251, 0, 0, 0, 0, 0))
    check(p.SensorOverride.unpack(ov.pack()) == ov,
          "SensorOverride carries the true reading alongside the substituted one")

    rc = p.RecoatCycle(feed_um=75, bed_um=-75, wipe_speed_mm_s=100,
                       settle_ms=2000, clearance_um=-500, passes=1,
                       park_mode=p.ParkMode.OVERFLOW)
    check(p.RecoatCycle.unpack(rc.pack()) == rc, "RecoatCycle round-trips with park mode")

    fan = p.FanSet(fan=p.FanId.RADIATOR, mode=p.FanMode.MANUAL, duty_pm=650)
    check(p.FanSet.unpack(fan.pack()).fan == p.FanId.RADIATOR,
          "FanSet addresses a specific fan")


def test_job_file_layout():
    """The on-card format both sides must agree on."""
    hdr = p.JobFileHeader(magic=b"MOIRENJB", format_version=1, layer_count=400,
                          job_id=7, layer_thickness_um=75, total_bytes=1_800_000,
                          file_crc=0x1234)
    check(p.JobFileHeader.unpack(hdr.pack()).magic == b"MOIRENJB", "job magic survives")

    lay = p.LayerHeader(layer_index=17, group_count=3, byte_count=4096,
                        z_um=-1275, crc=0xABCD)
    check(p.LayerHeader.unpack(lay.pack()) == lay, "LayerHeader round-trips")
    check(p.VectorGroup(point_count=1200).pack() != b"", "VectorGroup packs")


def test_invariants():
    check(p.NODE_AIRFLOW == p.NodeId.ARDUINO_MEGA,
          "NODE_AIRFLOW still points at the Mega (update PROTOCOL.md when it moves)")
    check(p.PACKET_MAX_LEN == p.PACKET_HEADER_LEN + p.PACKET_MAX_PAYLOAD + p.PACKET_CRC_LEN,
          "PACKET_MAX_LEN agrees with its parts")
    check(p.PROTOCOL_VERSION == 5, "PROTOCOL_VERSION is 5")
    check(p.AXIS_FLAG_POS_RESTORED == 0x20,
          "AXIS_FLAG_POS_RESTORED does not collide with the other axis flags")
    used = (p.AXIS_FLAG_HOMED | p.AXIS_FLAG_MOVING | p.AXIS_FLAG_AT_LIMIT |
            p.AXIS_FLAG_ENABLED | p.AXIS_FLAG_FAULT)
    check(p.AXIS_FLAG_POS_RESTORED & used == 0,
          "AXIS_FLAG_POS_RESTORED is a free bit")
    check(p.PURGE_FLAG_SKIP_MIN_MIX == 0x01, "PURGE_FLAG_SKIP_MIN_MIX is bit 0")
    purge = p.PurgeSet(enable=1, flags=p.PURGE_FLAG_SKIP_MIN_MIX,
                       target_o2_ppm=3000, timeout_s=1800, min_mix_s=300)
    check(len(purge.pack()) == 8, "purge_set_t packs to 8 bytes")
    check(p.PurgeSet.unpack(purge.pack()).min_mix_s == 300,
          "purge_set_t.min_mix_s survives a round trip")
    moveflags = (p.AXIS_MOVE_RELATIVE | p.AXIS_MOVE_APPROACH_NEG |
                 p.AXIS_MOVE_APPROACH_POS)
    check(p.AXIS_MOVE_NO_BOUNDS & moveflags == 0,
          "AXIS_MOVE_NO_BOUNDS is a free bit")
    check(p.PurgeSet.unpack(purge.pack()).flags == p.PURGE_FLAG_SKIP_MIN_MIX,
          "purge_set_t.flags survives a round trip")

    # 374.5 counts/mm is a 175 mm lens; both ends of the band must bracket it.
    check(p.FIELD_SCALE_MIN_MCPMM < 374500 < p.FIELD_SCALE_MAX_MCPMM,
          "the 175 mm lens scale sits inside the sanity band")
    check(round(65536 / (p.FIELD_SCALE_MIN_MCPMM / 1000)) == 300,
          "the low scale bound is a 300 mm field")
    check(round(65536 / (p.FIELD_SCALE_MAX_MCPMM / 1000)) == 33,
          "the high scale bound is a 33 mm field")
    check(1000 < p.FIELD_SCALE_MIN_MCPMM,
          "the 1.0 placeholder scale falls outside the band and is rejected")

    check(not hasattr(p, "MarkBatchHeader"),
          "MSG_MARK_BATCH is gone; vectors travel as a job file")

    oversized = p.Packet(payload=b"\x00" * (p.PACKET_MAX_PAYLOAD + 1))
    try:
        oversized.pack()
    except ValueError:
        print("  ok   oversized payload is refused")
    else:
        raise AssertionError("oversized payload was not refused")


def main():
    tests = [test_crc, test_struct_roundtrip, test_packet_roundtrip,
             test_decoder_recovery, test_bulk_transfers, test_new_messages,
             test_job_file_layout, test_invariants]
    for test in tests:
        print(f"{test.__name__}:")
        test()
    print(f"\nAll checks passed (protocol version {p.PROTOCOL_VERSION}, "
          f"source hash {p.SOURCE_HASH}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
