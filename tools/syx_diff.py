#!/usr/bin/env python3
"""
Diff two raw SysEx captures (.syx files, F0 ... F7 inclusive) to find where a
single changed control lands in a Moog-style bit-packed dump — same 7-bit-
per-byte model as read_bitpacked_field() in src/synthComms.c, and the same
dumpOffset/dumpBitOffset/dumpBitWidth terms used in layouts/voyager.txt
("byte 1" in that file's own comments == payload[0] == the manufacturer ID
byte, right after F0 — NOT the 4-byte moog header skipped separately).

Workflow: capture a baseline (Backup > Panel Dump), change exactly ONE
control on the real hardware, capture again, then:

    python3 tools/syx_diff.py before.syx after.syx [layouts/voyager.txt]

The optional third argument cross-references candidate ranges against a
device's layout file so an already-wired dial (e.g. cutoff drifting from knob
jitter) doesn't get mistaken for the control you actually changed.

A single diff only proves where a field is not what its full width is —
confirm dumpBitWidth by sweeping a control through its full range (as was
done for cutoff/resonance) before committing it to a layout file.
"""
import re
import sys


def load_payload(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 2 or data[0] != 0xF0 or data[-1] != 0xF7:
        raise ValueError(f"{path}: not a raw F0...F7 SysEx capture ({len(data)} bytes)")
    return data[1:-1]  # drop F0/F7 — matches handle_moog_panel_dump()'s skip=1 / trailing-F7 exclusion


def bit(payload, global_bit):
    byte_idx = global_bit // 7
    col = global_bit % 7
    if byte_idx >= len(payload):
        return None
    return (payload[byte_idx] >> col) & 1


def read_field(payload, byte_offset, bit_offset, bit_width):
    value = 0
    start = byte_offset * 7 + bit_offset
    for k in range(bit_width):
        b = bit(payload, start + k)
        if b is None:
            break
        value |= b << k
    return value


def load_layout_dials(path):
    """Parse dumpOffset/dumpBitOffset/dumpBitWidth + name out of a layouts/*.txt file."""
    dials = []
    dial_re = re.compile(r"^dial\s+(\S+)")
    field_re = re.compile(r"(dumpOffset|dumpBitOffset|dumpBitWidth)=(\d+)")
    try:
        with open(path) as f:
            for line in f:
                m = dial_re.match(line)
                if not m:
                    continue
                fields = dict(field_re.findall(line))
                if "dumpOffset" in fields and "dumpBitWidth" in fields:
                    dials.append((
                        m.group(1),
                        int(fields["dumpOffset"]),
                        int(fields.get("dumpBitOffset", 0)),
                        int(fields["dumpBitWidth"]),
                    ))
    except FileNotFoundError:
        print(f"(layout file {path} not found, skipping cross-reference)", file=sys.stderr)
    return dials


def overlapping_dials(dials, start_bit, end_bit):
    hits = []
    for name, off, boff, width in dials:
        d_start = off * 7 + boff
        d_end = d_start + width
        if d_start < end_bit and d_end > start_bit:
            hits.append(name)
    return hits


def main():
    if len(sys.argv) not in (3, 4):
        print(f"usage: {sys.argv[0]} before.syx after.syx [layout.txt]", file=sys.stderr)
        sys.exit(1)

    before = load_payload(sys.argv[1])
    after = load_payload(sys.argv[2])
    dials = load_layout_dials(sys.argv[3]) if len(sys.argv) == 4 else []

    max_len = max(len(before), len(after))
    print(f"before: {len(before)} payload bytes, after: {len(after)} payload bytes")
    if len(before) != len(after):
        print("WARNING: lengths differ — a name field or similar variable-length "
              "data may have shifted everything after it. Byte-level diff below "
              "may be misleading past the first length mismatch.")

    print("\n-- byte-level diff (payload index == dumpOffset; \"byte N\" == "
          "voyager.txt's own 1-based prose convention) --")
    any_byte_diff = False
    for i in range(max_len):
        b0 = before[i] if i < len(before) else None
        b1 = after[i] if i < len(after) else None
        if b0 != b1:
            any_byte_diff = True
            b0s = f"0x{b0:02X}" if b0 is not None else "  --"
            b1s = f"0x{b1:02X}" if b1 is not None else "  --"
            print(f"  dumpOffset={i:4d} (byte {i+1}): {b0s} -> {b1s}")
    if not any_byte_diff:
        print("  (no byte differs — control may not be wired to this dump mode at all)")

    print("\n-- bit-level candidate fields (contiguous differing-bit runs) --")
    total_bits = max_len * 7
    run_start = None
    ranges = []
    for gb in range(total_bits + 1):
        differs = gb < total_bits and bit(before, gb) != bit(after, gb)
        if differs and run_start is None:
            run_start = gb
        elif not differs and run_start is not None:
            ranges.append((run_start, gb))
            run_start = None

    if not ranges:
        print("  (no bit differs)")
    for start, end in ranges:
        width = end - start
        off = start // 7
        boff = start % 7
        old = read_field(before, off, boff, width)
        new = read_field(after, off, boff, width)
        print(f"  dumpOffset={off} dumpBitOffset={boff} dumpBitWidth={width}   "
              f"value {old} -> {new}")
        hits = overlapping_dials(dials, start, end)
        if hits:
            print(f"    overlaps already-wired dial(s): {', '.join(hits)} — "
                  f"likely coincidental drift, not your changed control")


if __name__ == "__main__":
    main()
