"""Print a function's disassembly next to its generated C. usage: show.py <hex addr> [gen dir]"""
import glob, re, sys
gen = sys.argv[2] if len(sys.argv) > 2 else 'src/recomp/gen'
a = int(sys.argv[1], 16)
for f in glob.glob(gen + '/recomp_*.c'):
    s = open(f).read()
    i = s.find('void f_%x(Ctx* c) {' % a)
    if i >= 0:
        print(s[i:s.index('\n}\n', i) + 2])
        break
