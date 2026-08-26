"""Send a .cor field-correction table to the galvo board, and check it landed.

    python tools/upload_correction.py tools/testdata/4_30_test3.cor
    python tools/upload_correction.py FILE --scale 374.5 --verify
    python tools/upload_correction.py FILE --negative      # refusal paths only

The .cor on disk holds doubles. They are rounded to int16 here, because that is
what travels: 4225 x 2 x 2 bytes is 16.9 KB rather than the file's 68 KB. The
sign-magnitude encoding mentioned in the format notes belongs to the OLD
controller's USB protocol, not to the file and not to ours -- bit 15 there is a
sign flag, and treating it as two's complement mirrors the field.

--verify is the interesting mode. It uploads, then asks the board where a few
hundred bed coordinates actually land, and compares each answer against a
reference implementation of the same bilinear interpolation written here. That
tests the compiled firmware on the real chip rather than a model of it.

NOTE ON SCALE: the .cor files that exist all carry 1.0, a placeholder the old
tooling wrote and never filled in. Taken literally it describes a 65-metre
field, so the firmware refuses it. Pass --scale with the real figure; for this
machine 65536 counts over a 175 mm field is 374.5 counts/mm.
"""

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "protocol"))

import serial
from serial.tools import list_ports

import protocol as p

GRID = 65
POINTS = GRID * GRID
CELL_SHIFT = 10
TABLE_OFFSET = 0x210          # 22-byte label + 0x1FA header
SCALE_OFFSET = 0x16 + 2 + 43 * 8   # label, two unknown bytes, then 63 doubles

ACK_NAMES = {v: k for k, v in vars(p.AckStatus).items() if isinstance(v, int)}

fails = 0


def check(ok, label):
    global fails
    print(("  ok   " if ok else "  FAIL ") + label)
    if not ok:
        fails += 1


def ctrunc(a, b):
    """Integer divide the way C does: toward zero, not toward minus infinity."""
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def read_cor(path):
    raw = open(path, "rb").read()
    label = raw[:0x16].decode("utf-16-le", errors="replace").rstrip("\x00")
    if label != "LMC1COR_1.0":
        raise SystemExit(f"{path}: not an LMC1COR_1.0 file (label {label!r})")
    file_scale = struct.unpack_from("<d", raw, SCALE_OFFSET)[0]
    vals = struct.unpack_from("<%dd" % (POINTS * 2), raw, TABLE_OFFSET)
    dx = [max(-32768, min(32767, int(round(v)))) for v in vals[0::2]]
    dy = [max(-32768, min(32767, int(round(v)))) for v in vals[1::2]]
    return file_scale, dx, dy


def table_bytes(dx, dy):
    out = bytearray()
    for i in range(POINTS):
        out += struct.pack("<hh", dx[i], dy[i])
    return bytes(out)


def ref_apply(x_um, y_um, scale_mcpmm, dx, dy, loaded):
    """Mirror of field::apply(). Kept deliberately literal so a difference is a
    firmware bug rather than two different algorithms disagreeing."""
    cx = 32768 + ctrunc(x_um * scale_mcpmm, 1000000)
    cy = 32768 + ctrunc(y_um * scale_mcpmm, 1000000)
    cx = max(0, min(65535, cx))
    cy = max(0, min(65535, cy))
    if not loaded:
        return cx, cy

    ix = min(cx >> CELL_SHIFT, GRID - 2)
    iy = min(cy >> CELL_SHIFT, GRID - 2)
    fx = cx - (ix << CELL_SHIFT)
    fy = cy - (iy << CELL_SHIFT)

    def lerp(t):
        c00, c10 = t[iy * GRID + ix], t[iy * GRID + ix + 1]
        c01, c11 = t[(iy + 1) * GRID + ix], t[(iy + 1) * GRID + ix + 1]
        top = c00 + (((c10 - c00) * fx) >> CELL_SHIFT)
        bot = c01 + (((c11 - c01) * fx) >> CELL_SHIFT)
        return top + (((bot - top) * fy) >> CELL_SHIFT)

    return (max(0, min(65535, cx + lerp(dx))),
            max(0, min(65535, cy + lerp(dy))))


class Board:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.dec = p.PacketDecoder()
        self.text = bytearray()
        self.seq = 0

    def _pump(self, seconds, until_ack=None):
        """Read for up to `seconds`, or until the ACK for `until_ack` arrives.
        Waiting out the full timeout on every one of 90 chunks turns a
        two-second upload into a minute."""
        packets, end = [], time.time() + seconds
        while time.time() < end:
            chunk = self.ser.read(512)
            if chunk:
                self.text.extend(chunk)
                for q in self.dec.feed(chunk):
                    packets.append(q)
                    if until_ack is not None and q.msg == p.MsgId.ACK:
                        if p.SysAck.unpack(q.payload).ack_seq == until_ack:
                            return packets
            else:
                time.sleep(0.002)
        return packets

    def ask(self, msg, payload=b"", seconds=1.0):
        self.seq = (self.seq + 1) & 0xFF
        self.ser.write(p.Packet(src=p.NodeId.PC, dst=p.NodeId.TEENSY_GALVO,
                                msg=msg, flags=0, seq=self.seq,
                                payload=payload).pack())
        for q in self._pump(seconds, until_ack=self.seq):
            if q.msg == p.MsgId.ACK:
                a = p.SysAck.unpack(q.payload)
                if a.ack_seq == self.seq:
                    return a.status
        return None

    def console(self, line, seconds=0.35):
        """Send a console line and return the text that came back."""
        self.text.clear()
        self.ser.write(line.encode() + b"\r\n")
        self._pump(seconds)
        raw = bytes(self.text)
        # Strip packet bytes so heartbeats do not land in the middle of a reply.
        out, skip, hdr = bytearray(), False, bytearray()
        need = 0
        for b in raw:
            if not skip:
                if b == p.PACKET_SOF0:
                    skip, hdr, need = True, bytearray([b]), 0
                else:
                    out.append(b)
                continue
            hdr.append(b)
            if len(hdr) == 2 and b != p.PACKET_SOF1:
                skip = False
                out.extend(hdr[1:])
            elif len(hdr) == p.PACKET_HEADER_LEN:
                need = hdr[-1] + p.PACKET_CRC_LEN
            elif need and len(hdr) >= p.PACKET_HEADER_LEN + need:
                skip = False
        return "".join(chr(c) for c in out if 32 <= c < 127 or c in (10, 13))


def upload(board, dx, dy, scale_mcpmm, corrupt_crc=False, skip_chunk=None):
    body = table_bytes(dx, dy)
    crc = p.crc16_ccitt(body) ^ (0xFFFF if corrupt_crc else 0)

    hdr = p.FieldCorrBegin(grid_size=GRID, point_total=POINTS,
                           scale_mcpmm=scale_mcpmm, table_crc=crc).pack()
    status = board.ask(p.MsgId.FIELD_CORRECTION_BEGIN, hdr)
    if status != p.AckStatus.OK:
        return status, 0

    per = p.FIELD_CORR_MAX_POINTS
    sent = chunk = 0
    while sent < POINTS:
        n = min(per, POINTS - sent)
        if skip_chunk is not None and chunk == skip_chunk:
            sent += n
            chunk += 1
            continue                      # a dropped chunk, on purpose
        payload = p.FieldCorrData(chunk_index=chunk, point_count=n).pack()
        payload += body[sent * 4:(sent + n) * 4]
        status = board.ask(p.MsgId.FIELD_CORRECTION_DATA, payload)
        if status != p.AckStatus.OK:
            return status, chunk
        sent += n
        chunk += 1
    return board.ask(p.MsgId.FIELD_CORRECTION_END, b"", seconds=1.0), chunk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corfile")
    ap.add_argument("--port", default=None)
    ap.add_argument("--scale", type=float, default=374.5,
                    help="counts per mm (default 374.5 = 65536 over 175 mm)")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--negative", action="store_true",
                    help="only exercise the refusal paths")
    args = ap.parse_args()

    file_scale, dx, dy = read_cor(args.corfile)
    scale_mcpmm = int(round(args.scale * 1000))
    print(f"{args.corfile}: {POINTS} points, "
          f"dx {min(dx)}..{max(dx)}, dy {min(dy)}..{max(dy)}")
    print(f"  scale in file : {file_scale}"
          + ("   <- placeholder, never calibrated" if file_scale == 1.0 else ""))
    print(f"  scale used    : {args.scale} counts/mm "
          f"-> {65536 / args.scale:.1f} mm field\n")

    port = args.port
    if not port:
        port = next((i.device for i in list_ports.comports()
                     if (i.vid, i.pid) == (0x16C0, 0x0483)), None)
    if not port:
        print("no Teensy found; pass --port")
        return 1
    b = Board(port)
    time.sleep(0.3)

    if args.negative:
        # Establish a known-good table first, so the real question can be asked:
        # does a FAILED upload leave the working one alone? "No table loaded"
        # would be a much weaker result -- the risk is a half-written table
        # replacing a good one, not an empty board.
        print("baseline")
        st, _ = upload(b, dx, dy, scale_mcpmm)
        check(st == p.AckStatus.OK, "a good table is in place to start with")
        probe = (-53_000, 21_000)
        before = b.console(f"field map {probe[0]} {probe[1]}", seconds=0.2)
        check("->" in before, "and it maps a reference point")

        print()
        print("refusal paths")
        st, _ = upload(b, dx, dy, 1000)
        check(st == p.AckStatus.BAD_PARAM,
              f"the 1.0 placeholder scale is refused (got {ACK_NAMES.get(st)})")
        st, _ = upload(b, dx, dy, scale_mcpmm, corrupt_crc=True)
        check(st == p.AckStatus.BAD_CRC,
              f"a bad table CRC is refused at END (got {ACK_NAMES.get(st)})")
        st, at = upload(b, dx, dy, scale_mcpmm, skip_chunk=10)
        check(st == p.AckStatus.BAD_PARAM,
              f"a dropped chunk is caught immediately, not absorbed "
              f"(refused at chunk {at}, got {ACK_NAMES.get(st)})")
        after = b.console(f"field map {probe[0]} {probe[1]}", seconds=0.2)
        check(after.split("->")[-1].strip() == before.split("->")[-1].strip(),
              "the working table is untouched by three failed uploads")
        out = b.console("field")
        check("loaded" in out, "and it is still reported as loaded")
        print(f"\n{fails} failed")
        return 1 if fails else 0

    print("upload")
    t0 = time.time()
    st, chunks = upload(b, dx, dy, scale_mcpmm)
    check(st == p.AckStatus.OK,
          f"{chunks} chunks accepted and committed (got {ACK_NAMES.get(st)})")
    print(f"       {time.time() - t0:.1f} s")

    out = b.console("field")
    check("loaded" in out, "board reports the table as loaded")
    for line in out.splitlines():
        if line.strip() and not line.startswith("field") or "scale" in line:
            print("       | " + line)

    if args.verify:
        print("\nverify: does the board interpolate the way it should")
        worst, checked = 0, 0
        step = 7_000
        for x_um in range(-80_000, 80_001, step):
            for y_um in range(-80_000, 80_001, step * 3):
                reply = b.console(f"field map {x_um} {y_um}", seconds=0.12)
                if "->" not in reply:
                    continue
                got = reply.split("->")[1].split()
                gx, gy = int(got[0]), int(got[1])
                ex, ey = ref_apply(x_um, y_um, scale_mcpmm, dx, dy, True)
                worst = max(worst, abs(gx - ex), abs(gy - ey))
                checked += 1
        check(checked > 100, f"{checked} points queried")
        check(worst == 0,
              f"every point matches the reference exactly (worst delta {worst})")

    b.ser.close()
    print(f"\n{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
