#!/usr/bin/env python3
"""Generate protocol/protocol.py from protocol/protocol.h.

protocol.h is the single source of truth for the machine communication
protocol. The PC-side repo consumes the Python bindings emitted here rather
than maintaining its own copy, so the two sides cannot drift apart.

    python tools/gen_protocol.py            # regenerate protocol/protocol.py
    python tools/gen_protocol.py --check    # exit 1 if it is out of date (CI)

Only the region of protocol.h between the PROTOCOL-GEN BEGIN/END markers is
parsed, and only the declaration style used there is supported:

    #define NAME <integer literal | previously defined name>
    typedef enum { NAME = <integer literal>, ... } name_t;
    typedef struct { <fixed-width member>; ... } name_t;

A member is `uint8_t x;`, `int32_t x;`, `char x[16];` or `int16_t x[6];`.
Anything else in the marked region is a hard error rather than a silent skip —
if the header grows a construct this cannot express, that should be noticed at
generation time, not at 3 a.m. on the machine.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
HEADER = REPO_ROOT / "protocol" / "protocol.h"
OUTPUT = REPO_ROOT / "protocol" / "protocol.py"

BEGIN_MARKER = "===== PROTOCOL-GEN BEGIN"
END_MARKER = "===== PROTOCOL-GEN END"

# C fixed-width type -> (struct format char, python element type)
TYPE_MAP = {
    "uint8_t": ("B", "int"),
    "int8_t": ("b", "int"),
    "uint16_t": ("H", "int"),
    "int16_t": ("h", "int"),
    "uint32_t": ("I", "int"),
    "int32_t": ("i", "int"),
    "uint64_t": ("Q", "int"),
    "int64_t": ("q", "int"),
    "char": ("s", "bytes"),
}

SIZE_MAP = {"B": 1, "b": 1, "H": 2, "h": 2, "I": 4, "i": 4, "Q": 8, "q": 8}


class GenError(Exception):
    """Raised when protocol.h contains something the generator cannot express."""


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------


def marked_region(text: str) -> str:
    """Return the declarations between the markers.

    Both markers live inside C comments, so the region is taken from the end of
    the BEGIN comment to the start of the END comment. Slicing at the markers
    themselves would leave half a comment at each edge and feed the prose in
    them to the parser.
    """
    try:
        start = text.index(BEGIN_MARKER)
        end = text.index(END_MARKER)
    except ValueError as exc:
        raise GenError(
            f"{HEADER.name}: could not find the PROTOCOL-GEN BEGIN/END markers"
        ) from exc
    if end < start:
        raise GenError(f"{HEADER.name}: PROTOCOL-GEN END appears before BEGIN")
    body_start = text.index("*/", start) + 2
    comment_start = text.rfind("/*", body_start, end)
    body_end = comment_start if comment_start != -1 else end
    return text[body_start:body_end]


def strip_comments(text: str) -> str:
    """Remove /* */ and // comments, preserving line structure."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def parse_int(literal: str) -> int:
    literal = literal.strip().rstrip("uUlL")
    return int(literal, 0)


def parse_defines(src: str, known: dict[str, int]) -> dict[str, int]:
    """Collect `#define NAME value` constants, resolving aliases to earlier names."""
    defines: dict[str, int] = {}
    for name, value in re.findall(r"^\s*#define\s+(\w+)\s+([^\n\\]+)$", src, flags=re.M):
        value = value.strip()
        if re.fullmatch(r"[+-]?(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", value):
            defines[name] = parse_int(value)
        elif value in defines:
            defines[name] = defines[value]
        elif value in known:
            defines[name] = known[value]
        else:
            raise GenError(
                f"#define {name} {value!r}: not an integer literal or a known "
                "constant. Simplify it, or move it outside the PROTOCOL-GEN markers."
            )
    return defines


def parse_enums(src: str) -> dict[str, list[tuple[str, int]]]:
    """Collect `typedef enum { ... } name_t;` blocks, honouring implicit values."""
    enums: dict[str, list[tuple[str, int]]] = {}
    for body, tag in re.findall(r"typedef\s+enum\s*\{(.*?)\}\s*(\w+)\s*;", src, flags=re.S):
        members: list[tuple[str, int]] = []
        next_value = 0
        for entry in (e.strip() for e in body.split(",")):
            if not entry:
                continue
            if "=" in entry:
                name, _, literal = entry.partition("=")
                value = parse_int(literal)
            else:
                name, value = entry, next_value
            name = name.strip()
            if not re.fullmatch(r"\w+", name):
                raise GenError(f"enum {tag}: cannot parse member {entry!r}")
            members.append((name, value))
            next_value = value + 1
        enums[tag] = members
    return enums


def parse_structs(src: str) -> dict[str, list[tuple[str, str, int]]]:
    """Collect `typedef struct { ... } name_t;` blocks.

    Returns tag -> [(member_name, c_type, array_len)], array_len 0 for scalars.
    """
    structs: dict[str, list[tuple[str, str, int]]] = {}
    for body, tag in re.findall(r"typedef\s+struct\s*\{(.*?)\}\s*(\w+)\s*;", src, flags=re.S):
        members: list[tuple[str, str, int]] = []
        for statement in (s.strip() for s in body.split(";")):
            if not statement:
                continue
            match = re.fullmatch(r"(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?", statement)
            if not match:
                raise GenError(f"struct {tag}: cannot parse member {statement!r}")
            c_type, name, count = match.group(1), match.group(2), match.group(3)
            if c_type not in TYPE_MAP:
                raise GenError(
                    f"struct {tag}.{name}: type {c_type!r} is not a supported "
                    f"fixed-width type ({', '.join(sorted(TYPE_MAP))})"
                )
            if c_type == "char" and not count:
                raise GenError(
                    f"struct {tag}.{name}: bare `char` is ambiguous on the wire; "
                    "use int8_t/uint8_t for a byte or char[N] for text"
                )
            members.append((name, c_type, int(count) if count else 0))
        if not members:
            raise GenError(f"struct {tag}: no members found")
        structs[tag] = members
    return structs


# --------------------------------------------------------------------------
# Name mapping
# --------------------------------------------------------------------------


def pascal(tag: str) -> str:
    """`sys_hello_t` -> `SysHello`, `machine_state_e` -> `MachineState`."""
    stem = re.sub(r"_(t|e)$", "", tag)
    return "".join(part.capitalize() for part in stem.split("_") if part)


def strip_prefix(members: list[tuple[str, int]]) -> list[tuple[str, int]]:
    """Drop the shared prefix from enum member names.

    Strips the longest common run of leading `_`-separated tokens, so
    `MSG_PING` reads as `MsgId.PING` and `FAN_MODE_MANUAL` as `FanMode.MANUAL`
    rather than `FanMode.MODE_MANUAL`. At least one token is always kept, and
    the whole thing is skipped if any result would be empty or start with a
    digit.
    """
    names = [n for n, _ in members]
    if len(names) < 2:
        return members

    tokens = [n.split("_") for n in names]
    common = 0
    while all(len(t) > common + 1 for t in tokens) and len({t[common] for t in tokens}) == 1:
        common += 1
    if common == 0:
        return members

    stripped = ["_".join(t[common:]) for t in tokens]
    if any(not name or name[0].isdigit() for name in stripped):
        return members
    if len(set(stripped)) != len(stripped):
        return members  # stripping collided two members; leave the names alone
    return list(zip(stripped, (v for _, v in members)))


def struct_format(members: list[tuple[str, str, int]]) -> tuple[str, int]:
    fmt = "<"
    size = 0
    for _, c_type, count in members:
        char, _ = TYPE_MAP[c_type]
        if c_type == "char":
            fmt += f"{count}s"
            size += count
        elif count:
            fmt += f"{count}{char}"
            size += count * SIZE_MAP[char]
        else:
            fmt += char
            size += SIZE_MAP[char]
    return fmt, size


# --------------------------------------------------------------------------
# Emission
# --------------------------------------------------------------------------


def emit(source_hash: str, defines: dict[str, int], enums, structs) -> str:
    out: list[str] = []
    w = out.append

    w('"""Moiren SLM machine communication protocol — Python bindings.')
    w("")
    w("GENERATED FILE — DO NOT EDIT.")
    w("Generated from protocol/protocol.h by tools/gen_protocol.py.")
    w("Edit the header, then run `python tools/gen_protocol.py`.")
    w('"""')
    w("")
    w("from __future__ import annotations")
    w("")
    w("import struct")
    w("from dataclasses import dataclass, field")
    w("from enum import IntEnum")
    w("from typing import ClassVar, Tuple")
    w("")
    w(f'SOURCE_HASH = "{source_hash}"  # sha256 of the generated region of protocol.h')
    w("")
    w("")
    w("# --- Constants -----------------------------------------------------------")
    w("")
    for name, value in defines.items():
        w(f"{name} = 0x{value:X}" if value > 9 else f"{name} = {value}")
    w("")
    w("")
    w("# --- Enums ---------------------------------------------------------------")
    for tag, members in enums.items():
        w("")
        w("")
        w(f"class {pascal(tag)}(IntEnum):")
        w(f'    """C `{tag}`."""')
        w("")
        for name, value in strip_prefix(members):
            w(f"    {name} = 0x{value:02X}")
    w("")
    w("")
    w("# --- Payload structs -----------------------------------------------------")
    w("# Wire encoding is little-endian and packed, matching #pragma pack(1) in the")
    w("# header. SIZE is the exact on-wire byte count of the payload.")

    for tag, members in structs.items():
        cls = pascal(tag)
        fmt, size = struct_format(members)
        w("")
        w("")
        w(f'_S_{cls.upper()} = struct.Struct("{fmt}")')
        w("")
        w("")
        w("@dataclass")
        w(f"class {cls}:")
        w(f'    """C `{tag}` — {size} bytes on the wire."""')
        w("")
        for name, c_type, count in members:
            if c_type == "char":
                w(f'    {name}: bytes = b""')
            elif count:
                w(f"    {name}: Tuple[int, ...] = field(default_factory=tuple)")
            else:
                w(f"    {name}: int = 0")
        w("")
        w(f'    FORMAT: ClassVar[str] = "{fmt}"')
        w(f"    SIZE: ClassVar[int] = {size}")
        w("")
        w("    def pack(self) -> bytes:")
        args = []
        for name, c_type, count in members:
            if c_type == "char":
                args.append(f"self.{name}.ljust({count}, b'\\x00')[:{count}]")
            elif count:
                args.append(f"*tuple(self.{name})[:{count}]")
            else:
                args.append(f"self.{name}")
        w(f"        return _S_{cls.upper()}.pack(")
        for arg in args:
            w(f"            {arg},")
        w("        )")
        w("")
        w("    @classmethod")
        w(f'    def unpack(cls, data: bytes) -> "{cls}":')
        w(f"        v = _S_{cls.upper()}.unpack(data[: cls.SIZE])")
        w("        return cls(")
        index = 0
        for name, c_type, count in members:
            if c_type == "char":
                w(f"            {name}=v[{index}],")
                index += 1
            elif count:
                w(f"            {name}=tuple(v[{index}:{index + count}]),")
                index += count
            else:
                w(f"            {name}=v[{index}],")
                index += 1
        w("        )")

    w("")
    w("")
    w(RUNTIME_SECTION.rstrip("\n"))
    w("")
    return "\n".join(out)


# Hand-written helpers appended to every generated file. They depend only on
# the constants emitted above, so they stay correct as the header evolves.
RUNTIME_SECTION = '''
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
'''


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


def generate() -> tuple[str, str]:
    header_text = HEADER.read_text(encoding="utf-8")
    region = marked_region(header_text)
    source_hash = hashlib.sha256(region.encode("utf-8")).hexdigest()[:16]

    src = strip_comments(region)
    enums = parse_enums(src)
    known = {name: value for members in enums.values() for name, value in members}
    defines = parse_defines(src, known)
    structs = parse_structs(src)

    if "PROTOCOL_VERSION" not in defines:
        raise GenError("protocol.h does not #define PROTOCOL_VERSION")

    collisions: dict[str, list[str]] = {}
    for tag in list(enums) + list(structs):
        collisions.setdefault(pascal(tag), []).append(tag)
    for py_name, tags in collisions.items():
        if len(tags) > 1:
            raise GenError(
                f"{' and '.join(tags)} both map to the Python name {py_name!r}. "
                "Rename one of them in protocol.h."
            )

    return emit(source_hash, defines, enums, structs), source_hash


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify protocol.py is up to date; exit 1 if not (used by CI)",
    )
    args = parser.parse_args()

    try:
        generated, source_hash = generate()
    except GenError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.check:
        if not OUTPUT.exists():
            print(f"error: {OUTPUT} does not exist. Run: python tools/gen_protocol.py",
                  file=sys.stderr)
            return 1
        if OUTPUT.read_text(encoding="utf-8") != generated:
            print(
                f"error: {OUTPUT.relative_to(REPO_ROOT)} is out of date with "
                f"protocol.h (expected source hash {source_hash}).\n"
                "       Run: python tools/gen_protocol.py",
                file=sys.stderr,
            )
            return 1
        print(f"protocol.py is up to date (source hash {source_hash})")
        return 0

    OUTPUT.write_text(generated, encoding="utf-8")
    print(f"wrote {OUTPUT.relative_to(REPO_ROOT)} (source hash {source_hash})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
