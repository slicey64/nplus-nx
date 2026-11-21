# prof.py sub_X ...: size, callers, callees (in order), large struct offsets (>= 10000: device fields) of each function.
import json,sys,re,collections,subprocess
g=json.load(open('out/nplus_cg.json'))
rev=collections.defaultdict(set)
for f,v in g.items():
    for c in v['calls']: rev[c].add(f)
for f in sys.argv[1:]:
    f=f if f.startswith('sub_') else 'sub_'+f.upper().replace('0X','')
    n=g[f]['n']
    out=subprocess.run([sys.executable,'-I','tools/ppc_dis.py','--image','out/renut_image.bin','0x'+f[4:],str(n)],capture_output=True,text=True).stdout
    offs=collections.Counter()
    for m in re.finditer(r'\b(l[bhwd]z|st[bhwd]|lwzu|stwu)\w* r\d+,(-?\d+)\(r(\d+)\)',out):
        o=int(m.group(2))
        if o>=10000: offs[('L' if m.group(1)[0]=='l' else 'S')+str(o)]+=1
    calls=[];[calls.append(c) for c in g[f]['calls'] if c not in calls]
    print(f'{f} n={n} callers={len(rev[f])} [{" ".join(sorted(rev[f])[:8])}]')
    print('   calls:',' '.join(calls))
    if offs: print('   dev:',' '.join(sorted(offs)))
