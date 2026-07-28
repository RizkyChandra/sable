#!/usr/bin/env python3
"""Regenerates the PSD fixtures next to this script.

The fixtures are committed, so this is not part of the build — it is here so
that the binaries have provenance and can be changed without hand-editing
bytes. Run it from anywhere:

    python3 tests/data/make_psd_fixtures.py

It writes two files:

  layered.psd  64x48, six layers including a group, a hidden layer, a clipped
               layer, a non-Normal blend mode and both PSD compression schemes.
  flat.psd     the same artwork with no layer section at all, its merged image
               composited here in floating point. tests.cpp compares Sable's
               flatten() of layered.psd against it, so the check is against an
               independent implementation of the compositing rules rather than
               against Sable itself.

Verified against Pillow's and ImageMagick's PSD readers when written.
"""

import os
import struct

W, H = 64, 48


# --------------------------------------------------------------- compositing
# Mirrors engine/src/io.cpp and canvas.cpp in floating point. Deliberately a
# separate implementation: if it agreed with Sable by construction it would
# prove nothing.

def blend_channel(mode, cs, cb):
    if mode == "norm":
        return cs
    if mode == "mul ":
        return cs * cb
    if mode == "scrn":
        return cs + cb - cs * cb
    if mode == "lite":
        return max(cs, cb)
    if mode == "dark":
        return min(cs, cb)
    raise ValueError("the fixtures do not use " + mode)


def blend_over(mode, src, dst):
    """W3C compositing on straight-alpha colour; both arguments are straight."""
    a_s, a_b = src[3] / 255.0, dst[3] / 255.0
    ao = a_s + a_b * (1.0 - a_s)
    if ao <= 0.0:
        return (0.0, 0.0, 0.0, 0.0)
    out = []
    for i in range(3):
        cs, cb = src[i] / 255.0, dst[i] / 255.0
        co = (a_s * (1.0 - a_b) * cs
              + a_s * a_b * blend_channel(mode, cs, cb)
              + (1.0 - a_s) * a_b * cb)
        out.append(min(1.0, max(0.0, co)) / ao * 255.0)   # back to straight
    return (out[0], out[1], out[2], ao * 255.0)


def composite(layers):
    """Bottom-to-top list of dicts; returns straight-alpha float pixels."""
    buf = [(0.0, 0.0, 0.0, 0.0)] * (W * H)
    clip = None
    for layer in layers:
        clipped = layer["clipping"] and clip is not None
        if not layer["visible"] or layer["opacity"] <= 0:
            if not layer["clipping"]:
                clip = None       # a hidden layer clips nothing above it
            continue

        pixels = (composite(layer["children"]) if layer["kind"] == "folder"
                  else layer_pixels(layer))
        for i in range(W * H):
            src = pixels[i]
            if src[3] == 0:
                continue
            scale = layer["opacity"] / 255.0
            if clipped:
                scale *= clip[i] / 255.0
            if scale <= 0.0:
                continue
            # Opacity scales alpha only: straight-alpha colour is unchanged.
            buf[i] = blend_over(layer["blend"],
                                (src[0], src[1], src[2], src[3] * scale), buf[i])
        if not layer["clipping"]:
            clip = [p[3] for p in pixels]
    return buf


def layer_pixels(layer):
    """A layer's own straight-alpha pixels across the whole canvas."""
    out = [(0.0, 0.0, 0.0, 0.0)] * (W * H)
    left, top, right, bottom = layer["rect"]
    r, g, b, a = layer["colour"]
    for y in range(top, bottom):
        for x in range(left, right):
            out[y * W + x] = (float(r), float(g), float(b), float(a))
    return out


# ---------------------------------------------------------------- PSD output

def be16(v):
    return struct.pack(">H", v & 0xFFFF)


def be32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def packbits(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 128:
            run += 1
        if run >= 3:
            out.append(257 - run)          # -(run - 1) as an unsigned byte
            out.append(data[i])
            i += run
        else:
            start, lit = i, 0
            while i < n and lit < 128:
                if i + 2 < n and data[i] == data[i + 1] == data[i + 2]:
                    break
                i += 1
                lit += 1
            out.append(lit - 1)
            out += data[start:start + lit]
    return bytes(out)


def channel_block(plane, w, h, compression):
    """One channel: its 2-byte compression tag and its data."""
    if w == 0 or h == 0:
        return be16(0)
    if compression == 0:
        return be16(0) + bytes(plane)
    rows = [packbits(bytes(plane[y * w:(y + 1) * w])) for y in range(h)]
    return be16(1) + b"".join(be16(len(r)) for r in rows) + b"".join(rows)


def pascal_name(name):
    raw = name.encode("ascii", "replace")[:255]
    field = bytes([len(raw)]) + raw
    return field + b"\0" * ((4 - len(field) % 4) % 4)


def additional(key, data):
    padded = data + b"\0" * ((4 - len(data) % 4) % 4)
    return b"8BIM" + key + be32(len(padded)) + padded


def unicode_name(name):
    units = name.encode("utf-16-be")
    return additional(b"luni", be32(len(units) // 2) + units)


def layer_record(rec):
    left, top, right, bottom = rec["rect"]
    w, h = right - left, bottom - top

    channels = []
    for cid, plane in rec["planes"]:
        channels.append((cid, channel_block(plane, w, h, rec["compression"])))

    out = bytearray()
    out += struct.pack(">iiii", top, left, bottom, right)
    out += be16(len(channels))
    for cid, block in channels:
        out += struct.pack(">h", cid) + be32(len(block))
    out += b"8BIM" + rec["blend"].encode("ascii")
    out += bytes([rec["opacity"], 1 if rec["clipping"] else 0])
    flags = 0x08 | (0x02 if not rec["visible"] else 0) | (0x01 if rec.get("lock") else 0)
    out += bytes([flags, 0])

    extra = bytearray()
    extra += be32(0)                        # layer mask data: none
    extra += be32(0)                        # layer blending ranges: none
    extra += pascal_name(rec["name"])
    extra += unicode_name(rec["name"])
    if rec.get("section") is not None:
        extra += additional(b"lsct", be32(rec["section"]))
    out += be32(len(extra)) + extra

    return bytes(out), b"".join(block for _, block in channels)


def planes_for(rect, colour):
    """Four straight-alpha planes covering the layer's own rectangle."""
    left, top, right, bottom = rect
    w, h = right - left, bottom - top
    size = w * h
    r, g, b, a = colour
    return [(-1, bytes([a]) * size), (0, bytes([r]) * size),
            (1, bytes([g]) * size), (2, bytes([b]) * size)]


def empty_planes():
    return [(-1, b""), (0, b""), (1, b""), (2, b"")]


def header(channels):
    return (b"8BPS" + be16(1) + b"\0" * 6 + be16(channels) +
            be32(H) + be32(W) + be16(8) + be16(3))


def merged_section(pixels):
    """The flattened composite every viewer that ignores layers will show."""
    planes = []
    for c in (0, 1, 2, 3):
        planes.append(bytes(min(255, max(0, int(round(p[c])))) for p in pixels))
    rows = []
    for plane in planes:
        for y in range(H):
            rows.append(packbits(plane[y * W:(y + 1) * W]))
    return be16(1) + b"".join(be16(len(r)) for r in rows) + b"".join(rows)


def write_layered(path, records, pixels):
    body = bytearray()
    body += struct.pack(">h", -len(records))    # negative: the merged image has alpha
    channel_data = bytearray()
    for rec in records:
        record, data = layer_record(rec)
        body += record
        channel_data += data
    body += channel_data
    if len(body) % 2:
        body += b"\0"

    layer_and_mask = be32(len(body)) + bytes(body) + be32(0)   # no global mask

    out = (header(4) + be32(0) + be32(0) +
           be32(len(layer_and_mask)) + layer_and_mask +
           merged_section(pixels))
    with open(path, "wb") as f:
        f.write(out)


def write_flat(path, pixels):
    out = header(4) + be32(0) + be32(0) + be32(0) + merged_section(pixels)
    with open(path, "wb") as f:
        f.write(out)


# ------------------------------------------------------------------ the art

BASE = {"name": "Base", "rect": (0, 0, W, H), "colour": (40, 80, 160, 255),
        "blend": "norm", "opacity": 255, "clipping": False, "visible": True,
        "compression": 1, "kind": "raster", "children": []}
FILL = {"name": "Fill", "rect": (8, 8, 40, 32), "colour": (255, 64, 64, 255),
        "blend": "norm", "opacity": 255, "clipping": False, "visible": True,
        "compression": 0, "kind": "raster", "children": []}
TINT = {"name": "Tint", "rect": (0, 0, W, H), "colour": (255, 255, 0, 100),
        "blend": "norm", "opacity": 255, "clipping": True, "visible": True,
        "compression": 1, "kind": "raster", "children": []}
SHADOW = {"name": "Shadow", "rect": (0, 0, 16, 16), "colour": (0, 0, 0, 128),
          "blend": "norm", "opacity": 255, "clipping": False, "visible": False,
          "compression": 1, "kind": "raster", "children": []}
GROUP = {"name": "Sky", "rect": (0, 0, 0, 0), "colour": (0, 0, 0, 0),
         "blend": "norm", "opacity": 200, "clipping": False, "visible": True,
         "compression": 0, "kind": "folder", "children": [FILL, TINT, SHADOW]}
INK = {"name": "Ink", "rect": (16, 16, 56, 44), "colour": (0, 200, 80, 200),
       "blend": "mul ", "opacity": 128, "clipping": False, "visible": True,
       "compression": 1, "kind": "raster", "children": []}

STACK = [BASE, GROUP, INK]          # bottom to top


def records_for(layer):
    """PSD writes bottom to top, so a group is: closing marker, children, header."""
    if layer["kind"] != "folder":
        return [dict(layer, planes=planes_for(layer["rect"], layer["colour"]),
                     section=None)]
    out = [dict(layer, name="</Layer group>", rect=(0, 0, 0, 0), section=3,
                blend="norm", opacity=255, clipping=False, visible=True,
                planes=empty_planes())]
    for child in layer["children"]:
        out += records_for(child)
    out.append(dict(layer, section=1, planes=empty_planes()))
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    records = []
    for layer in STACK:
        records += records_for(layer)
    pixels = composite(STACK)
    write_layered(os.path.join(here, "layered.psd"), records, pixels)
    write_flat(os.path.join(here, "flat.psd"), pixels)
    print("wrote layered.psd and flat.psd")


if __name__ == "__main__":
    main()
