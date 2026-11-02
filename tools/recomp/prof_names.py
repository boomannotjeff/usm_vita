"""Name the profiler's samples inside usm_re.asi: which translated functions (and runtime helpers) the main
thread spends its time in. usage: prof_names.py [usm_re.asi] [usm_re_profile.txt]"""
import bisect, glob, os, subprocess, sys

asi = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'usm_re.asi')
prof = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ.get('USM_GAME', '.'), 'usm_re_profile.txt')
nm = glob.glob(os.path.expandvars(r'%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*\llvm-mingw-*\bin\llvm-nm.exe'))[0]
base = 0x10000000  # image base of the .asi as linked
syms = []
for line in subprocess.run([nm, '-n', asi], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16) - base, p[2].lstrip('_')))
addrs = [a for a, _ in syms]
by, total = {}, 0
for line in open(prof):
    if line.startswith('exe '):
        continue
    k, v = line.split()
    if k == 'total':
        total = int(v)
        continue
    i = bisect.bisect_right(addrs, int(k, 16)) - 1
    name = syms[i][1] if i >= 0 else '?'
    by[name] = by.get(name, 0) + int(v)
mine = sum(by.values())
print('%d samples in our module of %d total (%.1f%%)' % (mine, total, 100.0 * mine / total))
for name, v in sorted(by.items(), key=lambda x: -x[1])[:40]:
    print('%6.2f%%  %s' % (100.0 * v / total, name))
