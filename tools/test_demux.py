"""Check that a text console and the binary protocol can share one port.

The Teensy carries both on a single USB serial connection with no mode switch
and no second port. That works because a packet always starts with 0xA5, which
a terminal cannot produce: it is outside the 7-bit range a keyboard emits, and
the console only ever inserts 0x20..0x7E. src/teensy-galvo/node.cpp routes each
received byte to the link if a packet is in progress or this byte opens one,
and to the console otherwise.

This exercises that rule against the real framing from the generated bindings.
It MIRRORS the C++ decision rather than compiling it, so it catches a broken
rule and not a broken transcription -- keep the two in step by hand.

The case worth having a test for is the last one: a packet cut off part-way
leaves the decoder mid-frame, and without a timeout it eats every keystroke
typed afterwards while waiting for bytes that are never coming.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "protocol"))
import protocol as p

class Link:
    """Same states and transitions as MoirenLink::feedByte."""
    def __init__(self):
        self.reset(); self.packets = []
    def reset(self):
        self.state = 0; self.buf = bytearray(); self.plen = 0
    def receiving(self):
        return self.state != 0
    def feed(self, b):
        if self.state == 0:
            if b == p.PACKET_SOF0: self.state = 1
        elif self.state == 1:
            if b == p.PACKET_SOF1: self.state = 2; self.buf = bytearray()
            elif b != p.PACKET_SOF0: self.state = 0
        elif self.state == 2:
            self.buf.append(b)
            if len(self.buf) == p.PACKET_HEADER_LEN - 2:
                self.plen = self.buf[-1]
                if self.plen > p.PACKET_MAX_PAYLOAD: self.state = 0
                else: self.state = 3 if self.plen else 4
        elif self.state == 3:
            self.buf.append(b)
            if len(self.buf) == (p.PACKET_HEADER_LEN - 2) + self.plen: self.state = 4
        elif self.state == 4:
            self.buf.append(b)
            if len(self.buf) == (p.PACKET_HEADER_LEN - 2) + self.plen + 2:
                body = self.buf[:-2]
                want = p.crc16_ccitt(bytes(body))
                got = self.buf[-2] | (self.buf[-1] << 8)
                if want == got: self.packets.append(bytes(body))
                self.state = 0

def route(stream, gap_after=None):
    """The node::poll() rule, plus the gap timeout."""
    link, text = Link(), bytearray()
    for i, b in enumerate(stream):
        if gap_after is not None and i == gap_after and link.receiving():
            link.reset()                      # PACKET_GAP_TIMEOUT_MS fired
        if link.receiving() or b == p.PACKET_SOF0:
            link.feed(b)
        else:
            text.append(b)
    return link, bytes(text)

def pkt(msg, payload=b""):
    return p.Packet(src=p.NodeId.PC, dst=p.NodeId.TEENSY_GALVO, msg=msg,
                    flags=0, seq=1, payload=payload).pack()

fails = 0
def check(ok, label):
    global fails
    print(("  ok   " if ok else "  FAIL ") + label)
    if not ok: fails += 1

# 1. text, packet, text
ping = pkt(p.MsgId.PING)
link, text = route(b"help\r\n" + ping + b"status\r\n")
check(text == b"help\r\nstatus\r\n", "console text passes through untouched around a packet")
check(len(link.packets) == 1, "the packet is decoded")

# 2. payload full of bytes that would otherwise look like text or framing
nasty = bytes([0xA5, 0x5A, 0x0D, 0x0A, 0x08, 0x1B, 0x7F] * 8)
link, text = route(b"x" + pkt(p.MsgId.LOG, nasty) + b"y")
check(text == b"xy", "a payload containing SOF, newline, backspace and ESC never reaches the console")
check(len(link.packets) == 1 and link.packets[0][7:] == nasty, "that payload survives intact")

# 3. every printable console character routes as text
printable = bytes(range(0x20, 0x7F))
link, text = route(printable)
check(text == printable, "no printable character is ever mistaken for a packet start")

# 4. truncated packet, then typing: the timeout is what saves the console
full = pkt(p.MsgId.PING)
truncated = full[:len(full) - 3]
link, text = route(truncated + b"help\r\n")
check(text == b"", "without a timeout, a truncated packet swallows console input")
link, text = route(truncated + b"help\r\n", gap_after=len(truncated))
check(text == b"help\r\n", "with the gap timeout, the console recovers after a truncated packet")

# 5. back-to-back packets with no gap
link, text = route(pkt(p.MsgId.PING) + pkt(p.MsgId.STATE_REQUEST))
check(len(link.packets) == 2 and text == b"", "back-to-back packets both decode")

print("\n%d failed" % fails)
sys.exit(1 if fails else 0)
