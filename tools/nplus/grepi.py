# grepi.py REGEX: generated functions whose instruction comments match REGEX (prints func, matching insn lines)
import re,glob,sys
FUNC=re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\(.*?(sub_[0-9A-F]{8}|xstart)')
pat=re.compile(sys.argv[1]); cur=None
for f in sorted(glob.glob('app/generated/*recomp*.cpp')):
    for line in open(f,errors='replace'):
        m=FUNC.match(line)
        if m: cur=m.group(1); continue
        s=line.strip()
        if s.startswith('// ') and pat.search(s): print(cur, s[3:])
