# Builds out/nplus_cg.json: {func: {"calls": [...], "n": insn_count}} from app/generated.
import re,glob,json,collections
FUNC=re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\(.*?(sub_[0-9A-F]{8}|xstart)')
CALL=re.compile(r'\b(sub_[0-9A-F]{8}|__imp__[A-Za-z0-9_]+)\(ctx, base\)')
INSN=re.compile(r'^\s*// [a-z]')
g={}
cur=None
for f in sorted(glob.glob('app/generated/*recomp*.cpp')):
    for line in open(f,errors='replace'):
        m=FUNC.match(line)
        if m: cur=m.group(1); g.setdefault(cur,{'calls':[],'n':0}); continue
        if cur is None: continue
        if INSN.match(line): g[cur]['n']+=1
        for c in CALL.findall(line):
            c=c[7:] if c.startswith('__imp__sub_') else c
            if c!=cur: g[cur]['calls'].append(c)
json.dump(g,open('out/nplus_cg.json','w'))
print(len(g),'functions', sum(v['n'] for v in g.values()),'insns')
