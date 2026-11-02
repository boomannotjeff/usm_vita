"""Find functions Ghidra missed: about a quarter of .text is in no Ghidra function, mostly methods only reachable
through vtables. A candidate entry is the start of an uncovered run that something points to (a dword in data,
a push/mov immediate, a call) or that sits 16-aligned after padding. It is kept if it decodes as a function:
following its control flow reaches only uncovered code and ends in ret/jmp. Repeats until nothing new is found.

usage: discover.py   -> tools/recomp/data/cfg_extra.txt  (F/R lines, read by load_cfg after cfg.txt)
"""
import bisect, os, struct

import capstone
from capstone import x86 as X

from usm_image import Image, load_cfg, CFG

OUT = CFG.replace('cfg.txt', 'cfg_extra.txt')
img = Image()
funcs, _ = load_cfg(extra=False)
va, size, _, data = next(s for s in img.sections if s[2] == '.text')
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

cov = bytearray(size)
for f in funcs.values():
    for lo, hi in f.ranges:
        if va <= lo and hi < va + size:
            cov[lo - va:hi - va + 1] = b'\1' * (hi - lo + 1)


def covered(a):
    return not (va <= a < va + size) or cov[a - va]


def refs():
    """every address something points to or calls"""
    r = set()
    for sva, ssize, name, sdata in img.sections:
        for k in range(0, len(sdata) - 3, 1 if name == '.text' else 4):
            v = struct.unpack_from('<I', sdata, k)[0]
            if name != '.text':
                r.add(v)
            elif k and (sdata[k - 1] == 0x68 or 0xb8 <= sdata[k - 1] <= 0xbf):  # push imm32 / mov reg, imm32
                r.add(v)
            elif k and sdata[k - 1] == 0xe8:  # call rel32
                r.add((sva + k + 4 + struct.unpack_from('<i', sdata, k)[0]) & 0xffffffff)
    return r


def body(entry):
    """[(lo, hi)] of the function at entry by recursive descent, or None if it is not plausible code"""
    todo, seen, ranges = [entry], set(), []
    while todo:
        a = lo = todo.pop()
        if a in seen:
            continue
        while True:
            if a in seen:  # fell into code already decoded
                break
            if covered(a):
                if a in funcs:  # falls into a known function
                    break
                return None  # runs into the middle of known code: not a function start
            i = next(md.disasm(img.read(a, 16), a), None)
            if i is None or (i.mnemonic == 'int3' and a == entry):
                return None
            seen.add(a)
            a += i.size
            if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                t = i.operands[0].imm & 0xffffffff
                if not covered(t):
                    todo.append(t)
                elif t not in funcs:
                    return None  # jumps into the middle of known code
            if i.mnemonic in ('ret', 'jmp', 'int3') or len(seen) > 20000:
                break
        if len(seen) > 20000:
            return None
        if a > lo:
            ranges.append((lo, a - 1))
    return ranges


found = {}
r = refs()
for rnd in range(60):
    cands = set()
    k = 0
    while k < size:
        if cov[k]:
            k += 1
            continue
        s = k
        while s < size and not cov[s] and data[s] in (0xcc, 0x90):
            s += 1
        e = s
        while e < size and not cov[e]:
            e += 1
        if s < e:
            a = va + s
            if a in r or (a % 16 == 0 and s > k):
                cands.add(a)
        k = e
    new = 0
    for a in sorted(cands):
        if a in found or a in funcs:
            continue
        b = body(a)
        if not b:
            continue
        found[a] = b
        new += 1
        for lo, hi in b:
            cov[lo - va:hi - va + 1] = b'\1' * (hi - lo + 1)
    print('round %d: %d candidates, %d new functions' % (rnd, len(cands), new))
    if not new:
        break

# Pointers from data (vtables, callback and constructor tables) to code that is not a function start: native code
# can enter there (FUN-less static initialisers inside an unwind funclet's range, 0x8473c0). Decode each on its
# own, overlapping known code if need be: its translation is only used when something really enters there.
def body_free(entry):
    todo, seen, ranges = [entry], set(), []
    while todo:
        a = lo = todo.pop()
        while a not in seen:
            if a != entry and a in funcs:
                break  # falls into a known function
            i = next(md.disasm(img.read(a, 16), a), None)
            if i is None or i.mnemonic in ('int3', 'hlt') and a == entry:
                return None
            seen.add(a)
            a += i.size
            if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                t = i.operands[0].imm & 0xffffffff
                if not (va <= t < va + size):
                    return None
                if t not in funcs:
                    todo.append(t)
            if i.mnemonic in ('ret', 'jmp', 'int3') or len(seen) > 20000:
                break
        if len(seen) > 20000:
            return None
        if a > lo:
            ranges.append((lo, a - 1))
    return ranges


data_refs, holes = set(), set()
# entries usm_pc.exe saw native code jump to (its survey mode logs every one): translate them all in one go
try:
    for line in open(os.path.join(os.environ.get('USM_GAME', '.'), 'usm_pc_holes.txt')):  # written by usm_pc.exe
        v = int(line.split()[0], 16)
        if v not in funcs:
            data_refs.add(v)
            holes.add(v)
except FileNotFoundError:
    pass
def in_text(sdata, k):
    """the dword at k is part of a string: UTF-16 (0x0065004c is "Le") or ASCII ("ics\0" is 0x00736369)"""
    w = sdata[max(0, k - 8):k + 12]
    pr = lambda b: 0x20 <= b < 0x7f
    utf16 = sum(1 for j in range(0, len(w) - 1, 2) if w[j + 1] == 0 and (pr(w[j]) or w[j] == 0))
    return utf16 >= len(w) // 2 - 1 or sum(1 for b in w if pr(b)) >= len(w) - 4


for sva, ssize, name, sdata in img.sections:
    if name in ('.rdata', '.data'):
        for k in range(0, len(sdata) - 3, 4):
            v = struct.unpack_from('<I', sdata, k)[0]
            if va <= v < va + size and v not in funcs and v not in found and not in_text(sdata, k):
                data_refs.add(v)
    elif name == '.text':  # push imm32 / mov reg, imm32: a function pointer handed to a callee (constructor
        for k in range(1, len(sdata) - 3):  # iterators, CreateThread, atexit; FUN-less ctor 0x75a800)
            if sdata[k - 1] == 0x68 or 0xb8 <= sdata[k - 1] <= 0xbf:
                v = struct.unpack_from('<I', sdata, k)[0]
                if va <= v < va + size and v not in funcs and v not in found:
                    data_refs.add(v)
extra, data_only = 0, set()
for a in sorted(data_refs):
    b = body_free(a)
    # real data-entered code is small (static ctors, funclets: 11..6657 bytes); a big body (30 KB+) is a number that only
    # looks like an address, falling into some large function that would be translated again for nothing
    if b and (a in holes or sum(hi - lo + 1 for lo, hi in b) <= 16384):
        found[a] = b
        data_only.add(a)
        extra += 1
print('%d more entries that data points to inside known code' % extra)

# Direct call targets inside known code are function starts too (FUN_007e7248 sits right after a jump table that
# Ghidra counted into the function before it). Called, so real functions (DISC_), not data-only entries.
called = 0
for k in range(len(data) - 5):
    if data[k] == 0xE8:
        t = (va + k + 5 + struct.unpack_from('<i', data, k + 1)[0]) & 0xffffffff
        if va <= t < va + size and t not in funcs and t not in found and t not in data_only:
            b = body_free(t)
            if b:
                found[t] = b
                called += 1
print('%d more entries that calls point to inside known code' % called)

left = sum(1 for k in range(size) if not cov[k] and data[k] not in (0xcc, 0x90))
print('%d functions found; %x non-padding bytes of .text still uncovered' % (len(found), left))
with open(OUT, 'w') as f:
    for a, b in sorted(found.items()):
        # DATA_: an entry only for native callers; jumps from other code into it stay inlined (recomp.py)
        f.write('F %08x %s_%08x\n' % (a, 'DATA' if a in data_only else 'DISC', a))
        for lo, hi in sorted(b):
            f.write('R %08x %08x\n' % (lo, hi))
        f.write('B %08x\n' % a)
print('wrote', OUT)
