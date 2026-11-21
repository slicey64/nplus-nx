# Query the call graph: q.py callees F | callers F | info F
import json,sys,collections
g=json.load(open('out/nplus_cg.json'))
rev=collections.defaultdict(set)
for f,v in g.items():
    for c in v['calls']: rev[c].add(f)
cmd=sys.argv[1]
for f in sys.argv[2:]:
    if cmd=='callees':
        seen=[];[seen.append(c) for c in g[f]['calls'] if c not in seen]
        print(f, '(%d insns):'%g[f]['n'], ' '.join(seen))
    elif cmd=='callers':
        print(f,'<-',' '.join('%s(%d)'%(c,g[c]['n']) for c in sorted(rev[f])))
