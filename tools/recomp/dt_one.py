"""Difftest specific functions against the build's own translation (src/recomp/gen; build_difftest.py it first).
usage: dt_one.py <hex addr>... [-v verbose] [-e through the hook stub] [-n every call nested through stubs]"""
import os, re, subprocess, sys

gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'recomp', 'gen')
addrs = re.search(r'rc_addr\[\] = \{([^}]*)\}', open(os.path.join(gen, 'recomp_table.c')).read())[1].split(',')
flags = ''.join(x[1:] for x in sys.argv[1:] if x.startswith('-'))
for a in [x for x in sys.argv[1:] if not x.startswith('-')]:
    k = addrs.index('0x%xu' % int(a, 16))
    out = subprocess.run([os.path.join(gen, 'difftest.exe'), str(k), '1', flags], cwd=gen, capture_output=True, text=True).stdout
    print('\n'.join(l for l in out.splitlines() if l.startswith(('PASS   ', 'FAIL', 'UNTEST', '  it'))))
