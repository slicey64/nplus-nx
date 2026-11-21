#!/usr/bin/env python3
"""nplus-nx: applies app/config/nplus_hook_map.tsv to Renut's hook lines (app/src/renut_engine/gpu/d3d/hooks/*.cpp).

Each active hook of the base (BANJO_RENUT_HOOK / _RAW at Beautiful Katamari's address) becomes:
  - the N+ address of its row (nplus_addr sub_X; `sub_X@other_hook` binds another hook function there, e.g. the
    Flags forms of SetGammaRamp / SetPWLGamma), marked `// nplus: <katamari> -> <n+> (<status>)`;
  - BANJO_RENUT_UNHOOKED_HOOK / _UNHOOKED_HOOK_RAW (nplus_addr '-'): N+'s own code runs, marked with the row's reason.
Rows still '?' stop the run. Idempotent: a line already marked `nplus:` is left alone.
After it: python tools/post_codegen.py in full (docs/xdk-notes.md: otherwise calls to the moved functions stay direct
and skip the hooks).
  python tools/nplus/hook_apply.py [--dry-run]
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hookmap  # noqa: E402

ROOT = hookmap.ROOT
PAT = re.compile(r'^(\s*)BANJO_RENUT_HOOK(_RAW)?\((sub_[0-9A-F]{8})(?:,\s*(\w+))?\)')
# The render and sampler state getters / setters: Renut's hook names its state index; N+'s state tables give the
# function ({getter, setter, default} per state, 12 bytes each; tools/nplus notes: 0x8274D980 render (101),
# 0x8274DE40 sampler (20), both read in the image; every entry is a generated function start).
STATE = re.compile(r'^(\s*)BANJO_RENUT_HOOK\((sub_[0-9A-F]{8}),\s*((Get|Set)(Render|Sampler)StateHook<(\d+)>)\)')
STATE_TABLES = {'Render': (0x8274D980, 101), 'Sampler': (0x8274DE40, 20)}
IMAGE = os.path.join(ROOT, 'out', 'renut_image.bin')


def state_target(image, gs, kind, index):
    import struct
    base, count = STATE_TABLES[kind]
    if index >= count:
        return None
    return struct.unpack_from('>I', image, base + index * 12 + (0 if gs == 'Get' else 4) - 0x82000000)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    rows = {(r['katamari_addr'], r['hook']): r for r in hookmap.load() if not r['kind'].startswith('UNHOOKED')}
    open_rows = [r for r in rows.values() if r['nplus_addr'] == '?']
    if open_rows:
        sys.exit('open rows: ' + ', '.join(r['hook'] for r in open_rows))
    hooked = unhooked = states = 0
    image = open(IMAGE, 'rb').read()
    for path in sorted(glob.glob(os.path.join(ROOT, 'app/src/renut_engine/gpu/d3d/hooks/*.cpp'))):
        lines = open(path, encoding='utf-8').read().split('\n')
        changed = False
        for i, line in enumerate(lines):
            s = STATE.match(line)
            if s and 'nplus:' not in line:
                indent, kat, fn, gs, kind, idx = s.groups()
                t = state_target(image, gs, kind, int(idx))
                rest = line[s.end():]
                if t is None:
                    lines[i] = indent + 'BANJO_RENUT_UNHOOKED_HOOK(%s, %s)' % (kat, fn) + rest.rstrip() + \
                        ' // nplus: unhooked, no %s state %s in N+\'s table' % (kind.lower(), idx)
                    unhooked += 1
                else:
                    lines[i] = indent + 'BANJO_RENUT_HOOK(sub_%08X, %s)' % (t, fn) + rest.rstrip() + \
                        ' // nplus: %s -> sub_%08X (%s state table %s)' % (kat, t, kind.lower(), idx)
                    states += 1
                changed = True
                continue
            m = PAT.match(line)
            if not m or 'nplus:' in line:
                continue
            indent, raw, kat, fn = m.group(1), m.group(2), m.group(3), m.group(4)
            r = rows.get((kat, fn or '(raw)'))
            if r is None:
                sys.exit('%s:%d: no row for %s %s' % (path, i + 1, kat, fn))
            rest = line[m.end():]
            target = r['nplus_addr']
            if target.startswith('sub_'):
                addr, _, other = target.partition('@')
                new_fn = other or fn
                head = 'BANJO_RENUT_HOOK%s(%s%s)' % (raw or '', addr, ', ' + new_fn if new_fn else '')
                note = ' // nplus: %s -> %s (%s)' % (kat, addr, r['status'])
                hooked += 1
            else:
                head = 'BANJO_RENUT_UNHOOKED_HOOK%s(%s%s)' % (raw or '', kat, ', ' + fn if fn else '')
                note = ' // nplus: unhooked, %s' % r['evidence'][:100]
                unhooked += 1
            # A raw hook's body follows on the same line ('{'): keep it before the note.
            if rest.lstrip().startswith('{'):
                lines[i] = indent + head + ' {' + note + rest.lstrip()[1:]
            else:
                lines[i] = indent + head + rest.rstrip() + note
            changed = True
        if changed and not a.dry_run:
            open(path, 'w', encoding='utf-8').write('\n'.join(lines))
    print('%d hooks moved to N+, %d state hooks from N+\'s tables, %d left unhooked%s' % (
        hooked, states, unhooked, ' (dry run)' if a.dry_run else ''))


if __name__ == '__main__':
    main()
