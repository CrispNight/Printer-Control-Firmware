"""Push a binary job file onto the galvo board's microSD card.

    python tools/make_job.py "test export.lpbf" -o job.moi --layers 1-40
    python tools/upload_job.py job.moi
    python tools/upload_job.py job.moi --negative     # refusal paths only

The upload is atomic on the board: bytes land in a temporary file, and only a
matching byte count and whole-file CRC turn it into the job. A transfer that
fails part-way leaves whatever was there before untouched and never becomes
printable.

MSG_JOB_UPLOAD_END takes noticeably longer than the other messages because the
board reads the whole file back off the card to check it. That is the point --
reading rather than trusting what was just written is what catches a card that
accepted the bytes and stored something else.
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

ACK_NAMES = {v: k for k, v in vars(p.AckStatus).items() if isinstance(v, int)}

fails = 0


def check(ok, label):
    global fails
    print(("  ok   " if ok else "  FAIL ") + label)
    if not ok:
        fails += 1


class Board:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.01)
        self.dec = p.PacketDecoder()
        self.text = bytearray()
        self.seq = 0

    def _pump(self, seconds, until_ack=None):
        packets, end = [], time.time() + seconds
        while time.time() < end:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                self.text.extend(chunk)
                for q in self.dec.feed(chunk):
                    packets.append(q)
                    if until_ack is not None and q.msg == p.MsgId.ACK:
                        if p.SysAck.unpack(q.payload).ack_seq == until_ack:
                            return packets
            else:
                time.sleep(0.001)
        return packets

    def ask(self, msg, payload=b"", seconds=2.0):
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

    def console(self, line, seconds=0.4):
        self.text.clear()
        self.ser.write(line.encode() + b"\r\n")
        self._pump(seconds)
        raw, out, skip, hdr, need = bytes(self.text), bytearray(), False, bytearray(), 0
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


def send_job(board, path, corrupt_crc=False, skip_chunk=None, progress=True):
    raw = open(path, "rb").read()
    head = p.JobFileHeader.unpack(raw[:p.JobFileHeader.SIZE])
    body = raw[p.JobFileHeader.SIZE:]
    crc = head.file_crc ^ (0xFFFF if corrupt_crc else 0)

    # total_bytes is the BODY, matching job_file_header_t exactly.
    begin = p.JobUploadBegin(job_id=head.job_id, total_bytes=len(body),
                             layer_count=head.layer_count, file_crc=crc).pack()
    st = board.ask(p.MsgId.JOB_UPLOAD_BEGIN, begin)
    if st != p.AckStatus.OK:
        return st, 0, 0.0

    per = p.JOB_UPLOAD_MAX_BYTES
    # The file goes on the wire verbatim, its own header included, so what
    # lands on the card is a complete job file rather than a body that only
    # makes sense alongside the BEGIN message that described it.
    wire = raw
    sent = chunk = 0
    t0 = time.time()
    while sent < len(wire):
        n = min(per, len(wire) - sent)
        if skip_chunk is not None and chunk == skip_chunk:
            sent += n
            chunk += 1
            continue
        payload = p.JobUploadData(chunk_index=chunk, byte_count=n).pack()
        payload += wire[sent:sent + n]
        st = board.ask(p.MsgId.JOB_UPLOAD_DATA, payload)
        if st != p.AckStatus.OK:
            return st, chunk, time.time() - t0
        sent += n
        chunk += 1
        if progress and chunk % 2000 == 0:
            rate = sent / max(0.001, time.time() - t0) / 1024
            eta = (len(wire) - sent) / max(1, sent / max(0.001, time.time() - t0))
            print(f"       {sent/1e6:5.1f} / {len(wire)/1e6:.1f} MB   "
                  f"{rate:5.0f} KB/s   {eta:4.0f} s left")

    elapsed = time.time() - t0
    # END reads the whole file back off the card, so it needs real time.
    wait = 10.0 + len(wire) / 200_000.0
    end = p.JobUploadEnd(job_id=head.job_id).pack()
    return board.ask(p.MsgId.JOB_UPLOAD_END, end, seconds=wait), chunk, elapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jobfile")
    ap.add_argument("--port", default=None)
    ap.add_argument("--negative", action="store_true")
    args = ap.parse_args()

    size = os.path.getsize(args.jobfile)
    head = p.JobFileHeader.unpack(open(args.jobfile, "rb").read(p.JobFileHeader.SIZE))
    print(f"{args.jobfile}: {size:,} bytes, {head.layer_count} layers, "
          f"job id 0x{head.job_id:X}")

    port = args.port or next((i.device for i in list_ports.comports()
                              if (i.vid, i.pid) == (0x16C0, 0x0483)), None)
    if not port:
        print("no Teensy found; pass --port")
        return 1
    b = Board(port)
    time.sleep(0.3)

    out = b.console("job")
    if "ABSENT" in out:
        print("  board reports no SD card")
        return 1

    if args.negative:
        print()
        print("refusal paths")
        st, at, _ = send_job(b, args.jobfile, corrupt_crc=True, progress=False)
        check(st == p.AckStatus.BAD_CRC,
              f"a bad whole-file CRC is refused at END (got {ACK_NAMES.get(st)})")
        st, at, _ = send_job(b, args.jobfile, skip_chunk=5, progress=False)
        check(st == p.AckStatus.BAD_PARAM,
              f"a dropped chunk is caught at the next one, not absorbed "
              f"(refused at chunk {at}, got {ACK_NAMES.get(st)})")
        out = b.console("job")
        check("none" in out or "job id" in out,
              "the board still answers after two failed uploads")
        check("upload" not in out, "and no upload is left half-open")
        print()
        print(f"{fails} failed")
        return 1 if fails else 0

    print()
    print("upload")
    st, chunks, elapsed = send_job(b, args.jobfile)
    check(st == p.AckStatus.OK,
          f"{chunks} chunks accepted, verified off the card and committed "
          f"(got {ACK_NAMES.get(st)})")
    body = size - p.JobFileHeader.SIZE
    print(f"       {elapsed:.1f} s for {body/1e6:.1f} MB "
          f"= {body/max(0.001, elapsed)/1024:.0f} KB/s")

    out = b.console("job")
    for line in out.splitlines():
        if line.strip() and "job" != line.strip():
            print("       | " + line)
    check(f"{head.layer_count}" in out, "the board reports the layer count back")

    b.ser.close()
    print()
    print(f"{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
