#!/usr/bin/env python3
"""The N+ hook map (app/config/nplus_hook_map.tsv): Renut's hooks at Katamari's addresses (the base) -> N+'s.

Columns: katamari_addr  hook  kind  file  nplus_addr  status  evidence
  nplus_addr: sub_X, '-' (leave unhooked in N+), '?' (not found yet)
  status: hand (read in the image), shape (structure only: check), open
  python tools/nplus/hookmap.py init                  # from the base's hook lines (keeps filled rows)
  python tools/nplus/hookmap.py set HOOK ADDR STATUS EVIDENCE
  python tools/nplus/hookmap.py show [open|done]
"""
import glob, os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAP = os.path.join(ROOT, 'app', 'config', 'nplus_hook_map.tsv')
HDR = ['katamari_addr', 'hook', 'kind', 'file', 'nplus_addr', 'status', 'evidence']
PAT = re.compile(r'^(BANJO_RENUT_(?:UNHOOKED_)?HOOK(?:_RAW)?(?:_SHARED)?)\((sub_[0-9A-F]{8})(?:,\s*(\w+))?\)')


def load():
    rows = []
    if os.path.exists(MAP):
        for l in open(MAP):
            if l.startswith('#') or not l.strip():
                continue
            f = l.rstrip('\n').split('\t')
            if f[0] == 'katamari_addr':
                continue
            rows.append(dict(zip(HDR, f + [''] * (len(HDR) - len(f)))))
    return rows


def save(rows):
    with open(MAP, 'w') as out:
        out.write('# nplus-nx: Renut\'s Direct3D hooks (the ultimate base\'s, at Beautiful Katamari\'s XDK 2.0.5632 addresses)\n'
                  '# at N+\'s (XDK 2.0.6274). Applied by tools/nplus/hook_apply.py. See tools/nplus/hookmap.py for the columns.\n')
        out.write('\t'.join(HDR) + '\n')
        for r in rows:
            out.write('\t'.join(r[h] for h in HDR) + '\n')


def main():
    cmd = sys.argv[1]
    rows = load()
    if cmd == 'init':
        have = {(r['katamari_addr'], r['hook']): r for r in rows}
        new = []
        for f in sorted(glob.glob(os.path.join(ROOT, 'app/src/renut_engine/gpu/d3d/hooks/*.cpp'))):
            for l in open(f):
                m = PAT.match(l.strip())
                if not m:
                    continue
                kind = m.group(1).replace('BANJO_RENUT_', '')
                key = (m.group(2), m.group(3) or '(raw)')
                r = have.get(key) or {'katamari_addr': key[0], 'hook': key[1], 'kind': kind, 'file': os.path.basename(f),
                                      'nplus_addr': '-' if kind.startswith('UNHOOKED') else '?',
                                      'status': 'base-unhooked' if kind.startswith('UNHOOKED') else 'open', 'evidence': ''}
                new.append(r)
        save(new)
        print(len(new), 'rows')
    elif cmd == 'set':
        hook, addr, status, ev = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
        hit = [r for r in rows if (r['hook'] in (hook, hook + '_hook') or r['katamari_addr'] == hook) and not r['kind'].startswith('UNHOOKED')]
        if len(hit) != 1:
            sys.exit('%d rows for %s' % (len(hit), hook))
        hit[0].update(nplus_addr=addr, status=status, evidence=ev)
        save(rows)
    elif cmd == 'show':
        which = sys.argv[2] if len(sys.argv) > 2 else ''
        for r in rows:
            if r['kind'].startswith('UNHOOKED'):
                continue
            if which == 'open' and r['nplus_addr'] != '?':
                continue
            if which == 'done' and r['nplus_addr'] == '?':
                continue
            print('%-46s %s -> %-12s %-6s %s' % (r['hook'], r['katamari_addr'], r['nplus_addr'], r['status'], r['evidence'][:90]))
        n = [r for r in rows if not r['kind'].startswith('UNHOOKED')]
        print('placed %d / %d' % (sum(r['nplus_addr'] != '?' for r in n), len(n)))


if __name__ == "__main__":
    main()
