"""Run difftest.exe over every translated function in parallel shards, restarting a shard after a function
wrecks the tester.
usage: dt_drive.py <gen dir> [flags: e = through the hook stub, n = every call nested through stubs] [shards]
  -> <gen dir>/difftest<flags>.txt"""
import os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

gen = sys.argv[1]
flags = sys.argv[2] if len(sys.argv) > 2 else ''
shards = int(sys.argv[3]) if len(sys.argv) > 3 else os.cpu_count()
exe = os.path.join(gen, 'difftest.exe')
count = len(re.search(r'rc_addr\[\] = \{([^}]*)\}', open(os.path.join(gen, 'recomp_table.c')).read())[1].split(','))


def shard(i):
    lo, hi = count * i // shards, count * (i + 1) // shards
    prog = os.path.join(gen, 'dt_progress_%s%d.txt' % (flags, i))
    env = dict(os.environ, DT_PROGRESS=prog)
    lines, k = [], lo
    while k < hi:
        if os.path.exists(prog): os.remove(prog)
        # a tester stuck on one function (no progress for 60 s) is killed and that function marked HANG
        log = prog + '.out'
        with open(log, 'w') as lf:
            p = subprocess.Popen([exe, str(k), str(hi - k), flags], cwd=gen, env=env, stdout=lf, stderr=subprocess.DEVNULL)
            last, since = None, time.time()
            while p.poll() is None:
                time.sleep(2)
                cur = open(prog).read() if os.path.exists(prog) else None
                if cur != last:
                    last, since = cur, time.time()
                elif time.time() - since > 60:
                    p.kill(); p.wait()
                    break
        out = open(log).read()
        lines += [l for l in out.splitlines() if l.startswith(('PASS ', 'FAIL ', 'UNTEST '))]
        if p.returncode == 0 and out.rstrip().splitlines() and out.rstrip().splitlines()[-1].startswith('PASS '):
            break
        if not os.path.exists(prog) or not open(prog).read().strip():
            lines.append('CRASH  ?  tester failed to start (exit %x)' % (p.returncode & 0xffffffff))
            break
        n, addr = open(prog).read().split()
        lines.append('CRASH  %s  took the tester down or hung (exit %x)' % (addr, p.returncode & 0xffffffff))
        k = int(n) + 1
    return lines


with ThreadPoolExecutor(shards) as ex:
    results = list(ex.map(shard, range(shards)))
with open(os.path.join(gen, 'difftest%s.txt' % flags), 'w') as out:
    for r in results:
        out.write(''.join(l + '\n' for l in r))
c = {}
for r in results:
    for l in r:
        c[l.split()[0]] = c.get(l.split()[0], 0) + 1
print(flags or 'direct', c)
