"""Compare two profiles taken at the same frame rate (same work per second): vanilla (nothing translated) and
translated. Per original function: share of main-thread time in each, and the extra time the translation costs.
usage: prof_compare.py <vanilla usm_re_profile.txt> <translated usm_re_profile.txt> [usm_re.asi]"""
import bisect, glob, os, subprocess, sys
from usm_image import load_cfg

van, tra = sys.argv[1], sys.argv[2]
asi = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'usm_re.asi')
nm = glob.glob(os.path.expandvars(r'%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*\llvm-mingw-*\bin\llvm-nm.exe'))[0]
syms = sorted((int(p[0], 16) - 0x10000000, p[2].lstrip('_')) for p in
              (l.split() for l in subprocess.run([nm, '-n', asi], capture_output=True, text=True).stdout.splitlines())
              if len(p) == 3 and p[1] in 'tT')
sym_at = [a for a, _ in syms]
funcs, _ = load_cfg()
rng = sorted((lo, hi, a) for a, f in funcs.items() for lo, hi in f.ranges)
rlo = [r[0] for r in rng]


def exe_fn(pc):
    k = bisect.bisect_right(rlo, pc) - 1
    while k >= 0 and rng[k][0] > pc - 0x10000:
        if rng[k][0] <= pc <= rng[k][1]:
            return '%06x' % rng[k][2]
        k -= 1
    return 'exe?%06x' % (pc & ~0xff)


def load(path):
    by, total = {}, 0
    for line in open(path):
        p = line.split()
        if p[0] == 'total':
            total = int(p[1])
        elif p[0] == 'exe':
            name = exe_fn(int(p[1], 16))
            by[name] = by.get(name, 0) + int(p[2])
        else:
            i = bisect.bisect_right(sym_at, int(p[0], 16)) - 1
            n = syms[i][1] if i >= 0 else '?'
            name = n[2:] if n.startswith('f_') else '[' + n + ']'
            by[name] = by.get(name, 0) + int(p[1])
    return {k: 100.0 * v / total for k, v in by.items()}, total


v, vt = load(van)
t, tt = load(tra)
print('vanilla: game code %.1f%%   translated: game code + runtime %.1f%%' % (sum(v.values()), sum(t.values())))
rows = sorted(set(v) | set(t), key=lambda k: -(t.get(k, 0) - v.get(k, 0)))
print('%-12s %8s %8s %8s %6s' % ('function', 'vanilla', 'transl', 'extra', 'ratio'))
for k in rows[:40]:
    a, b = v.get(k, 0), t.get(k, 0)
    print('%-12s %7.2f%% %7.2f%% %7.2f%% %6s' % (k, a, b, b - a, '%.1fx' % (b / a) if a else '-'))
