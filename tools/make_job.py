"""Convert a Lachesis .lpbf export into the machine's binary job file.

    python tools/make_job.py "test export.lpbf" -o job.moi
    python tools/make_job.py FILE -o job.moi --layers 1-20     # a slice, for testing

A .lpbf is a zip: a manifest.json plus one DXF per layer. The DXFs are enormous
-- a 60 MB archive expands to 836 MB -- because every coordinate is ASCII and
every polyline carries a block of XDATA. The binary form is roughly twenty
times smaller and needs no parsing on the board.

WHAT MAPS TO WHAT

Each LWPOLYLINE is one scan segment, with POWER (watts) and SPEED (mm/s) in its
LACHESIS XDATA. Those become a laser_params_t. Its vertices become
vector_point_t: the first with the laser OFF, because that is the jump to the
start of the segment, and the rest with it on.

Consecutive segments sharing a parameter set are collected into one
vector_group_t. **Order is never changed.** The slicer chose that scan order
for thermal reasons, so segments are only grouped where they are already
adjacent -- grouping by sorting would silently reorder the scan.

Power is per-mille of the source's full scale, taken from the manifest's
machine_profile.max_laser_power_w, because the firmware has no idea what a watt
is on this machine.

Each layer gets its own CRC, checked when the layer is READ rather than only at
upload. A card can develop bad sectors weeks after a correct write, so that is
the check which actually protects a print.
"""

import argparse
import json
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "protocol"))

import protocol as p

MAGIC = b"MOIRENJB"
FORMAT_VERSION = 1


def parse_layer(text):
    """Return [(power_w, speed_mm_s, region, [(x_mm, y_mm), ...]), ...] in file
    order. A DXF is pairs of lines: a group code, then its value."""
    lines = text.split("\n")
    polys = []
    cur = None
    last_key = None
    i, n = 0, len(lines) - 1
    while i < n:
        code = lines[i].strip()
        val = lines[i + 1].strip()
        i += 2
        if code == "0":
            cur = {"pts": [], "xd": {}, "region": None} if val == "LWPOLYLINE" else None
            if cur is not None:
                polys.append(cur)
            continue
        if cur is None:
            continue
        if code == "8":
            cur["region"] = val
        elif code == "10":
            cur["pts"].append([float(val), 0.0])
        elif code == "20":
            if cur["pts"]:
                cur["pts"][-1][1] = float(val)
        elif code == "1000":
            last_key = val
        elif code in ("1040", "1070", "1071"):
            if last_key:
                cur["xd"][last_key] = float(val) if code == "1040" else int(val)
    return [(q["xd"].get("POWER", 0.0), q["xd"].get("SPEED", 0.0),
             q["region"], q["pts"]) for q in polys if q["pts"]]


def params_blob(power_w, speed_mm_s, max_w, on_delay_us, off_delay_us):
    power_pm = 0 if max_w <= 0 else int(round(power_w / max_w * 1000.0))
    power_pm = max(0, min(1000, power_pm))
    return p.LaserParams(
        power_pm=power_pm,
        freq_hz=0,                       # CW source; no pulse rate to set
        pulse_width_ns=0,
        mark_speed_mm_s=int(round(speed_mm_s)),
        jump_speed_mm_s=int(round(speed_mm_s)),
        on_delay_us=int(round(on_delay_us)),
        off_delay_us=int(round(off_delay_us)),
        poly_delay_us=0,
    ).pack()


def build_layer(segments, index, z_um, max_w, on_us, off_us):
    """One layer: groups of points, each group preceded by its parameters."""
    body = bytearray()
    groups = 0
    total_points = 0

    run = []
    run_key = None

    def flush():
        nonlocal groups, total_points
        if not run:
            return
        pts = bytearray()
        count = 0
        for _, _, _, verts in run:
            for k, (x_mm, y_mm) in enumerate(verts):
                # First vertex of a segment is the jump to its start, so the
                # laser is off on the way there.
                flags = 0 if k == 0 else p.POINT_FLAG_LASER_ON
                pts += p.VectorPoint(x_um=int(round(x_mm * 1000.0)),
                                     y_um=int(round(y_mm * 1000.0)),
                                     flags=flags).pack()
                count += 1
        body.extend(p.VectorGroup(point_count=count).pack())
        body.extend(params_blob(run_key[0], run_key[1], max_w, on_us, off_us))
        body.extend(pts)
        groups += 1
        total_points += count
        run.clear()

    for seg in segments:
        key = (seg[0], seg[1])
        if key != run_key:
            flush()
            run_key = key
        run.append(seg)
    flush()

    # Mark the very last point of the layer, so a reader knows where it ends
    # without trusting a count it may have mis-parsed.
    if total_points:
        last = len(body) - 1
        body[last] = body[last] | p.POINT_FLAG_LAST

    header = p.LayerHeader(layer_index=index, group_count=groups,
                           byte_count=len(body), z_um=z_um,
                           crc=p.crc16_ccitt(bytes(body))).pack()
    return header + bytes(body), total_points


def verify(path):
    """Read the file back the way the firmware will, and check every claim it
    makes about itself. Cheap, and it means a bad job file is caught here
    rather than after it has been pushed to a card."""
    raw = open(path, "rb").read()
    head = p.JobFileHeader.unpack(raw[:p.JobFileHeader.SIZE])
    body = raw[p.JobFileHeader.SIZE:]
    problems = []

    if head.magic[:8] != MAGIC:
        problems.append(f"magic is {head.magic!r}")
    if head.total_bytes != len(body):
        problems.append(f"total_bytes {head.total_bytes} but {len(body)} follow")
    if p.crc16_ccitt(body) != head.file_crc:
        problems.append("whole-file CRC mismatch")

    off, layers, points = 0, 0, 0
    while off < len(body):
        lh = p.LayerHeader.unpack(body[off:off + p.LayerHeader.SIZE])
        off += p.LayerHeader.SIZE
        chunk = body[off:off + lh.byte_count]
        if len(chunk) != lh.byte_count:
            problems.append(f"layer {lh.layer_index} truncated")
            break
        if p.crc16_ccitt(chunk) != lh.crc:
            problems.append(f"layer {lh.layer_index} CRC mismatch")
        # walk its groups so a wrong group_count or point_count is caught here
        gi, seen = 0, 0
        while gi < len(chunk):
            g = p.VectorGroup.unpack(chunk[gi:gi + p.VectorGroup.SIZE])
            gi += p.VectorGroup.SIZE + p.LaserParams.SIZE
            gi += g.point_count * p.VectorPoint.SIZE
            seen += 1
            points += g.point_count
        if seen != lh.group_count:
            problems.append(f"layer {lh.layer_index}: {seen} groups, "
                            f"header says {lh.group_count}")
        if gi != len(chunk):
            problems.append(f"layer {lh.layer_index}: groups do not fill the layer")
        off += lh.byte_count
        layers += 1

    if layers != head.layer_count:
        problems.append(f"{layers} layers present, header says {head.layer_count}")
    return layers, points, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lpbf")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--job-id", type=lambda s: int(s, 0), default=1)
    ap.add_argument("--layers", default=None,
                    help="1-based inclusive range, e.g. 1-20, for testing")
    ap.add_argument("--laser-watts", type=float, default=500.0,
                    help="source full scale, used only when the manifest does "
                         "not carry max_laser_power_w (older exports do not)")
    args = ap.parse_args()

    z = zipfile.ZipFile(args.lpbf)
    man = json.loads(z.read("manifest.json"))
    mp = man.get("machine_profile", {})
    max_w = float(mp.get("max_laser_power_w", 0.0))
    on_us = float(mp.get("laser_on_delay_us", 0.0))
    off_us = float(mp.get("laser_off_delay_us", 0.0))
    thickness = int(man.get("slicing_parameters", {}).get("layer_thickness_um", 0))
    layers = man["layers"]

    from_manifest = max_w > 0
    if not from_manifest:
        # Older exports have a shorter machine_profile. Powers are meaningless
        # without a full scale to divide by, so fall back -- but say so, because
        # a wrong full scale silently scales every power in the job.
        max_w = args.laser_watts

    lo, hi = 1, len(layers)
    if args.layers:
        a, _, b = args.layers.partition("-")
        lo, hi = int(a), int(b or a)

    print(f"{args.lpbf}")
    src_note = ("from manifest" if from_manifest else
                f"NOT IN THIS FILE -- using --laser-watts {max_w}")
    print(f"  {len(layers)} layers, {thickness} um each")
    print(f"  laser full scale: {max_w} W ({src_note})")
    if (lo, hi) != (1, len(layers)):
        print(f"  converting layers {lo}..{hi} only")

    body = bytearray()
    used = 0
    points = 0
    dxf_bytes = 0        # only the layers actually converted, so a --layers
                         # subset does not get compared against the whole job
    for meta in layers:
        idx = meta["index"]
        if idx < lo or idx > hi:
            continue
        name = f"layer_{idx:05d}.dxf"
        try:
            raw = z.read(name)
        except KeyError:
            continue
        dxf_bytes += len(raw)
        segs = parse_layer(raw.decode("utf-8", "replace"))
        blob, n = build_layer(segs, used, int(meta["z_height_um"]),
                              max_w, on_us, off_us)
        body.extend(blob)
        points += n
        used += 1
        if used % 100 == 0:
            print(f"    {used} layers, {points:,} points, {len(body)/1e6:.1f} MB")

    head = p.JobFileHeader(magic=MAGIC, format_version=FORMAT_VERSION,
                           layer_count=used, job_id=args.job_id,
                           layer_thickness_um=thickness,
                           total_bytes=len(body),
                           file_crc=p.crc16_ccitt(bytes(body))).pack()

    with open(args.out, "wb") as f:
        f.write(head)
        f.write(body)

    src = os.path.getsize(args.lpbf)
    print(f"\n  wrote {args.out}")
    print(f"    {used} layers, {points:,} points, {len(head) + len(body):,} bytes")
    total = len(head) + len(body)
    # Compare against the DXF the board would otherwise have to parse, not
    # against the zip: the archive is already compressed, so that comparison
    # flatters nothing and explains nothing.
    print(f"    {dxf_bytes / max(1, total):.0f}x smaller than the DXFs inside "
          f"({dxf_bytes/1e6:.0f} MB), {src/1e6:.0f} MB as a compressed archive")

    layers_seen, points_seen, problems = verify(args.out)
    if problems:
        print()
        print("  VERIFY FAILED")
        for msg in problems[:10]:
            print(f"    {msg}")
        return 1
    print(f"  verified: {layers_seen} layers, {points_seen:,} points, "
          f"every layer CRC and group count consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
