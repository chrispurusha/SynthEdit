#!/usr/bin/env python3
"""
Diff two raw Kronos Object/Current Object Dump captures (.syx, F0..F7
inclusive, as written by tools/kronos_dump / kronos_dump_current) to find
which decoded byte offset a single changed parameter lands at.

Unlike Voyager's dump (see syx_diff.py's own docstring — a continuous 7-bit-
per-byte BITSTREAM), the Kronos's dump payload is Korg's standard 7-in-8
byte packing (7 real bytes carried in every 8 wire bytes, MSBs collected
into a leading byte) — see decode_7to8() in src/synthComms.c, which this
mirrors exactly. Once decoded, each field is one plain byte (or a few,
never sub-byte-packed) at a fixed offset, addressed the same way z1.txt's
dumpOffset= already works — that's what a found offset here should be wired
into a device .txt file's dial as.

Workflow: capture a baseline (tools/kronos_dump_current <dest> <obj> before.syx),
change exactly ONE parameter on the real hardware or via tools/kronos_param,
capture again (after.syx), then:

    python3 tools/kronos_dump_diff.py before.syx after.syx

Prints every decoded byte offset that differs, with both values.
"""
import sys


def load_payload(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 2 or data[0] != 0xF0 or data[-1] != 0xF7:
        raise ValueError(f"{path}: not a raw F0...F7 SysEx capture ({len(data)} bytes)")
    return data[1:-1]


def decode_7to8(midi):
    out = bytearray()
    i = 0
    while i + 7 < len(midi):
        msbs = midi[i]
        for j in range(7):
            out.append(midi[i + 1 + j] | (((msbs >> j) & 1) << 7))
        i += 8
    return bytes(out)


def strip_header(payload):
    # F0 <mfrId=0x42> <0x30|ch> <familyId=0x68> <func> <obj> <version> <7-in-8 data...> (F7 already stripped)
    if len(payload) < 6 or payload[0] != 0x42 or payload[2] != 0x68:
        raise ValueError("not a Kronos-shaped capture (mfrId/familyId mismatch)")
    return payload[6:]


def main():
    if len(sys.argv) != 3:
        print("usage: kronos_dump_diff.py <before.syx> <after.syx>", file=sys.stderr)
        sys.exit(1)

    before = decode_7to8(strip_header(load_payload(sys.argv[1])))
    after = decode_7to8(strip_header(load_payload(sys.argv[2])))

    n = min(len(before), len(after))
    if len(before) != len(after):
        print(f"note: decoded lengths differ (before={len(before)}, after={len(after)}) — comparing first {n} bytes")

    diffs = [i for i in range(n) if before[i] != after[i]]
    if not diffs:
        print("no differences found")
        return

    for i in diffs:
        print(f"offset {i}: {before[i]} (0x{before[i]:02X}) -> {after[i]} (0x{after[i]:02X})")


if __name__ == "__main__":
    main()
