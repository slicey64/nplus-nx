#!/usr/bin/env python3
"""nplus-nx: Renut's Katamari (XDK 2.0.5632) Direct3D device offsets -> N+'s (XDK 2.0.6274).

N+'s CDevice is Katamari's with 8 bytes inserted between +13444 and +13480 and 8 more between +22248 and +22264
(docs/nplus-notes.md, "The device layout"; each boundary bracketed by fields read in N+'s own code):
  Katamari  < 13448          -> same
  Katamari 13448 .. 22251    -> + 8   (export fields 13480.. -> 13488..; CommandBuffer 13492 -> 13500;
                                       PresentFenceTag 14916 -> 14924; SwapCount 16536 -> 16544;
                                       SideBuffer 16704 -> 16712; TitleMinusOne 21672 -> 21680; gamma 22244 -> 22252)
  Katamari 22252 .. 0x5700   -> +16   (BehaviorFlags 22264 -> 22280)
and the allocation is 0x5780 (Katamari 0x5700).

Lists the literals in Renut's code (not comments) in the shifted range, decimal 13448..22399 or hex 0x3488..0x577F,
in app/src/renut_engine/gpu; --apply rewrites them (same spelling: hex stays hex) and marks each changed line with
`// nplus: X->Y`. Lines whose numbers are not device offsets go in SKIP (file:substring). Idempotent: a line already
marked `nplus:` is left alone.
  python tools/nplus/device_offsets.py            # list
  python tools/nplus/device_offsets.py --apply
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
NUM = re.compile(r'(?<![0-9A-Za-z_.])(0x[0-9A-Fa-f]+|\d+)(u|U)?(?![0-9A-Za-z_.])')
LO, HI = 13448, 0x5780

# Literals in range that are not device offsets (reviewed by hand): (file suffix, text on the line).
SKIP = [
]


def shift(v):
    if LO <= v < 22252:
        return v + 8
    if 22252 <= v < HI:
        return v + 16
    return v


def in_range(s, v):
    return LO <= v < HI


def sites():
    for dirpath, _, files in os.walk(GPU):
        for name in sorted(files):
            if not name.endswith(('.cpp', '.h', '.inc')):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, GPU)
            lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
            for i, line in enumerate(lines):
                code, sep, comment = line.partition('//')
                if not code.strip() or code.lstrip().startswith(('*', '/*', '#include')) or 'nplus:' in comment:
                    continue
                vals = [m for m in NUM.finditer(code)
                        if in_range(m.group(1), int(m.group(1), 16) if m.group(1).lower().startswith('0x') else int(m.group(1)))]
                # Device offsets in Renut carry katamari-nx's own remap note (`katamari-nx: X->Y`), except the scaler
                # copy at 0x3584; every other literal in the range was reviewed and is a mask, alignment, dirty bit
                # or time (0x3FFF, 0x4000, 0x3C00, 15000 ms, ...). The allocation size is set by hand (0x5780).
                device_offset = 'katamari-nx:' in line or '0x3584' in line
                if vals and device_offset and 'kDeviceAllocSize' not in line and \
                        not any(rel.endswith(f) and t in line for f, t in SKIP):
                    yield path, rel, i, lines, vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    changed = {}
    n = 0
    for path, rel, i, lines, vals in sites():
        n += 1
        line = lines[i]
        code, sep, comment = line.partition('//')
        notes = []
        new = code
        for m in reversed(vals):
            s = m.group(1)
            v = int(s, 16) if s.lower().startswith('0x') else int(s)
            w = shift(v)
            t = ('0x%X' % w) if s.lower().startswith('0x') else str(w)
            new = new[:m.start(1)] + t + new[m.end(1):]
            notes.insert(0, '%s->%s' % (s, t))
        if not a.apply:
            print('%s:%d: %s   [%s]' % (rel, i + 1, line.strip()[:150], ', '.join(notes)))
            continue
        mark = 'nplus: ' + ', '.join(notes)
        lines[i] = new + (sep + comment.rstrip() + '; ' + mark if sep else '// ' + mark)
        changed[path] = lines
    for path, lines in changed.items():
        open(path, 'w', encoding='utf-8').write('\n'.join(lines))
    print('%d lines%s' % (n, ' rewritten in %d files' % len(changed) if a.apply else ''), file=sys.stderr)


if __name__ == '__main__':
    main()
