"""Talk to a board over USB and check it behaves: console, packets, and E-stop.

Bring-up tool, not a unit test -- it needs real hardware attached.

    python tools/probe_board.py                 # auto-detect, safe checks only
    python tools/probe_board.py --port COM6
    python tools/probe_board.py --estop         # also latch and verify E-stop

The interesting check is the interleave one. The Teensy carries the text
console and the binary protocol on the same USB port, so this sends half a
typed command, injects a packet, then sends the rest. The console must still
see one clean line. On firmware without the split it comes back mangled --
"upt" + packet + "ime" arrives as "uptZifime".

--estop latches the board, which needs a reset to clear. It does not touch a
laser unless one is connected: it drives the command lines low, asserts the
laser's E-stop input and opens the interlock.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "protocol"))

import serial
from serial.tools import list_ports

import protocol as p

TEENSY_VID_PID = (0x16C0, 0x0483)

ACK_NAMES = {v: k for k, v in vars(p.AckStatus).items() if isinstance(v, int)}
LASER_NAMES = {v: k for k, v in vars(p.LaserState).items() if isinstance(v, int)}

fails = 0


def check(ok, label):
    global fails
    print(("  ok   " if ok else "  FAIL ") + label)
    if not ok:
        fails += 1


def find_port():
    for info in list_ports.comports():
        if (info.vid, info.pid) == TEENSY_VID_PID:
            return info.device
    return None


class Board:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.01)
        self.dec = p.PacketDecoder()
        self.split = _Splitter()

    def gather(self, seconds):
        """Read for a while and split packets from console text, the way any
        host on this link has to -- non-packet bytes are skipped, not errors."""
        text, packets = bytearray(), []
        end = time.time() + seconds
        while time.time() < end:
            # read(n) blocks for the whole port timeout when fewer than n
            # bytes are waiting, which turns every exchange into a stall.
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                text.extend(chunk)
                packets.extend(self.dec.feed(chunk))
            else:
                time.sleep(0.002)
        return bytes(text), packets

    def send(self, msg, payload=b"", seq=1):
        self.ser.write(p.Packet(src=p.NodeId.PC, dst=p.NodeId.TEENSY_GALVO,
                                msg=msg, flags=0, seq=seq,
                                payload=payload).pack())

    def ask(self, msg, payload=b"", seq=1, seconds=1.2):
        self.send(msg, payload, seq)
        return self.gather(seconds)

    def close(self):
        self.ser.close()


class _Splitter:
    """The same routing rule node::poll() uses, so console output can be shown
    without packet bytes leaking into it. Filtering by "is it printable" is not
    enough: plenty of packet bytes are."""

    def __init__(self):
        self.n = 0          # bytes still expected in the packet being skipped
        self.hdr = bytearray()
        self.in_packet = False

    def feed(self, raw):
        out = bytearray()
        for byte in raw:
            if not self.in_packet:
                if byte == p.PACKET_SOF0:
                    self.in_packet = True
                    self.hdr = bytearray([byte])
                    self.n = 0
                else:
                    out.append(byte)
                continue
            self.hdr.append(byte)
            if len(self.hdr) == 2 and byte != p.PACKET_SOF1:
                self.in_packet = False          # not a packet after all
                out.extend(self.hdr[1:])
            elif len(self.hdr) == p.PACKET_HEADER_LEN:
                self.n = self.hdr[-1] + p.PACKET_CRC_LEN
            elif self.n and len(self.hdr) >= p.PACKET_HEADER_LEN + self.n:
                self.in_packet = False
        return bytes(out)


def text_of(raw, splitter=None):
    """Console output only, with packet bytes removed."""
    clean = (splitter or _Splitter()).feed(raw)
    return "".join(chr(c) for c in clean if 32 <= c < 127 or c in (10, 13))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--estop", action="store_true",
                    help="also latch E-stop (needs a board reset afterwards)")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("no Teensy found; pass --port")
        return 1

    print(f"port {port}\n")
    try:
        b = Board(port)
    except Exception as exc:
        print(f"cannot open {port}: {exc}\n(close any serial monitor first)")
        return 1
    time.sleep(0.3)

    print("unprompted traffic")
    _, packets = b.gather(2.0)
    kinds = {q.msg for q in packets}
    check(p.MsgId.HEARTBEAT in kinds, "heartbeat is being published")
    check(p.MsgId.GALVO_STATUS in kinds, "galvo status is being published")

    print("\nconsole still works")
    raw, _ = b.ask(p.MsgId.PING, seq=1)  # flush
    b.ser.write(b"link\r\n")
    raw, _ = b.gather(1.5)
    out = text_of(raw, b.split)
    check("proto v" in out, "'link' prints the protocol version")
    for line in out.splitlines():
        if line.strip():
            print("       | " + line)

    print("\npackets work on the same wire")
    _, packets = b.ask(p.MsgId.PING, seq=7)
    check(any(q.msg == p.MsgId.PONG for q in packets), "PING answered with PONG")

    print("\na packet injected mid-keystroke does not corrupt the typed line")
    b.ser.write(b"upt")
    b.send(p.MsgId.PING, seq=8)
    b.ser.write(b"ime\r\n")
    raw, packets = b.gather(1.5)
    out = text_of(raw, b.split)
    check("uptime_ms=" in out, "the console still saw one clean 'uptime'")
    check(any(q.msg == p.MsgId.PONG for q in packets), "and the packet was answered")

    print("\nlaser commands are refused, not silently accepted")
    arm = p.LaserArm(arm=1, key=p.LASER_ARM_KEY).pack()
    _, packets = b.ask(p.MsgId.LASER_ARM, arm, seq=11)
    acks = [p.SysAck.unpack(q.payload) for q in packets if q.msg == p.MsgId.ACK]
    check(any(a.status == p.AckStatus.REFUSED for a in acks),
          "MSG_LASER_ARM answered REFUSED")

    if args.estop:
        print("\nE-stop")
        _, packets = b.ask(p.MsgId.ESTOP, seq=13)
        acks = [p.SysAck.unpack(q.payload) for q in packets if q.msg == p.MsgId.ACK]
        check(any(a.status == p.AckStatus.OK for a in acks), "MSG_ESTOP acknowledged")

        _, packets = b.ask(p.MsgId.STATE_REQUEST, seq=14)
        state = next((p.StateReport.unpack(q.payload)
                      for q in packets if q.msg == p.MsgId.STATE), None)
        check(state is not None and state.state == p.MachineState.ESTOP,
              "state is STATE_ESTOP")
        check(state is not None and (state.fault_flags & p.FAULTBIT_ESTOP),
              "FAULTBIT_ESTOP is latched")
        laser = next((p.GalvoStatus.unpack(q.payload)
                      for q in packets if q.msg == p.MsgId.GALVO_STATUS), None)
        check(laser is not None and laser.laser_state == p.LaserState.FAULT,
              "laser reports FAULT")

        _, packets = b.ask(p.MsgId.FAULT_CLEAR, seq=16)
        acks = [p.SysAck.unpack(q.payload) for q in packets if q.msg == p.MsgId.ACK]
        check(any(a.status == p.AckStatus.REFUSED for a in acks),
              "MSG_FAULT_CLEAR cannot clear an E-stop over the link")

        _, packets = b.ask(p.MsgId.PING, seq=17)
        check(any(q.msg == p.MsgId.PONG for q in packets),
              "board still answers while latched")
        print("  NOTE: board is latched. Reset it before further use.")

    b.close()
    print(f"\n{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
