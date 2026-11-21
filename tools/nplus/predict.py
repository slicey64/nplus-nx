#!/usr/bin/env python3
"""Predicts N+ addresses of open hooks from the placed ones: the XDK's code keeps its size between 5632 and 6274
in local runs, so nplus = anchor_nplus + (katamari - anchor_katamari) for the nearest anchors. A prediction counts only
where N+ has a function starting exactly there (out/nplus_cg.json); agreement of two anchors makes it stronger.
  python tools/nplus/predict.py [--cg out/nplus_cg.json] [--window 0x3000]
"""
import argparse, json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import hookmap
ap = argparse.ArgumentParser(); ap.add_argument('--cg', default='out/nplus_cg.json'); ap.add_argument('--window', default='0x3000')
a = ap.parse_args(); win = int(a.window, 16)
g = json.load(open(a.cg))
starts = {int(f[4:], 16): v['n'] for f, v in g.items() if f.startswith('sub_')}
rows = [r for r in hookmap.load() if not r['kind'].startswith('UNHOOKED')]
anchors = sorted((int(r['katamari_addr'][4:], 16), int(r['nplus_addr'][4:], 16)) for r in rows
                 if r['nplus_addr'].startswith('sub_') and r['status'] == 'hand')
for r in rows:
    if r['nplus_addr'] != '?':
        continue
    k = int(r['katamari_addr'][4:], 16)
    near = sorted(anchors, key=lambda x: abs(x[0] - k))[:4]
    votes = {}
    for ak, an in near:
        if abs(ak - k) > win:
            continue
        p = an + (k - ak)
        if p in starts:
            votes.setdefault(p, []).append('%X' % ak)
    if votes:
        best = max(votes, key=lambda p: len(votes[p]))
        print('%-44s kat %08X -> sub_%08X (n=%d) anchors %s' % (r['hook'], k, best, starts[best], ','.join(votes[best])))
