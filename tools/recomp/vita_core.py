"""Reads a Vita crash dump (psp2core-*.psp2dmp, gzipped ELF core) of usm.vpk: each thread's registers and the
translated functions (f_XXXXXX = guest address) and C functions found on the crashed thread's stack, newest first.
usage: py tools/recomp/vita_core.py <dump> [build-vita/usm]"""
import gzip
import os
import struct
import subprocess
import sys

dump = sys.argv[1]
elf = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', '..', 'build-vita', 'usm')
d = gzip.open(dump).read()
phoff, phnum = struct.unpack_from('<I', d, 28)[0], struct.unpack_from('<H', d, 44)[0]
notes, segs = {}, []
for i in range(phnum):
    t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<8I', d, phoff + 32 * i)
    if t == 1 and fs:
        segs.append((va, off, fs))
    elif t == 4:
        k = off
        while k < off + fs:
            nsz, dsz, typ = struct.unpack_from('<3I', d, k)
            s = k + 12 + ((nsz + 3) & ~3)
            notes[d[k + 12:k + 12 + nsz].rstrip(b'\0').decode()] = d[s:s + dsz]
            k = s + ((dsz + 3) & ~3)


def rd32(a):
    for va, off, fs in segs:
        if va <= a and a + 4 <= va + fs:
            return struct.unpack_from('<I', d, off + a - va)[0]


# symbols of the eboot (linked at 0x81000000, where the Vita loads it)
nm = subprocess.run([r'C:\vitasdk\bin\arm-vita-eabi-nm', '-n', elf], capture_output=True, text=True).stdout
syms = [(int(a, 16), n) for a, t, n in (l.split()[:3] for l in nm.splitlines() if len(l.split()) >= 3) if t in 'tTwW']
text_end = max(a for a, n in syms)


# where the loader really put the code segment (MODULE_INFO, module "usm"): addresses in the dump are relative to it
mi = notes['MODULE_INFO']
k = mi.index(b'usm\0')
while not (struct.unpack_from('<I', mi, k)[0] == 5 and 0x81000000 <= struct.unpack_from('<I', mi, k + 4)[0] < 0x82000000):
    k += 4
slide = struct.unpack_from('<I', mi, k + 4)[0] - 0x81000000
print('code segment at %08x (slide %x)' % (0x81000000 + slide, slide))


def sym(a):
    a -= slide
    lo, hi = 0, len(syms)
    while lo < hi:
        m = (lo + hi) // 2
        if syms[m][0] <= a: lo = m + 1
        else: hi = m
    return '%s+%x' % (syms[lo - 1][1], a - syms[lo - 1][0]) if lo else '?'


regs = notes['THREAD_REG_INFO']
n = struct.unpack_from('<I', regs, 4)[0]
k = 8
crash_sp = None
for t in range(n):
    size, uid = struct.unpack_from('<II', regs, k)
    r = struct.unpack_from('<17I', regs, k + 8)
    print('thread %08x: pc=%08x (%s) lr=%08x (%s) sp=%08x' % (uid, r[15], sym(r[15]), r[14], sym(r[14]), r[13]))
    print('  ' + ' '.join('r%d=%08x' % (i, r[i]) for i in range(13)))
    if t == 0:
        crash_sp = r[13]
    k += size
print('stack of the first thread (return addresses, newest first):')
seen = 0
for o in range(0, 0x4000, 4):
    v = rd32(crash_sp + o)
    if v is None:
        break
    if 0x81000000 + slide <= v <= text_end + slide and v & 1:
        s = sym(v - 1)
        if not s.startswith(('rc_ranges', 'rc_addr', 'rc_entries', '.')):
            print('  %08x %s' % (v - 1, s))
            seen += 1
            if seen > 40:
                break
