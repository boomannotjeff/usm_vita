"""Static recompiler: USM.exe x86 -> C, one C function per x86 function (runtime: src/recomp/rt.h).

usage: recomp.py <out dir> [file of hex addresses | all]

Every instruction is translated literally (registers, flags, guest memory, the guest stack), so the result
behaves like the original by construction instead of depending on a decompiler. Functions that use an
instruction not handled here are left out (listed in skipped.tsv) and keep running as original code.
"""
import glob
import os
import re
import sys

import capstone
import regflow
from capstone import x86 as X

from usm_image import Image, load_cfg


class Unsupported(Exception):
    pass


R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
# functions replaced by hand-written native code (src/vita/native.c): their translation is emitted as fo_XXXX, kept
# for the self-check, and f_XXXX comes from the native file
NATIVE = {int(l.split()[0], 16) for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'native.txt'))
          if l.strip() and l[0] != '#'}
# natives that keep the MSVC convention (src/vita/native.c math helpers: stack args and ecx in, eax/ecx/edx out); the
# others (pipeline fences, pass-throughs) count as reading and changing every register
NATIVE_ABI = {0x411a50, 0x504270, 0x413530, 0x4126e0, 0x413e90, 0x4139a0, 0x414360, 0x48e900, 0x507130, 0x822160}
# per-thread globals (tools/recomp/tls.txt: address size): a fixed-address access inside one becomes c->tls[k] + offset
TLS_ROWS = [l.split() for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tls.txt')) if l.strip() and l[0] != '#']
TLS = [(int(r[0], 16), int(r[1], 16)) for r in TLS_ROWS]
# third column: copy directions for the pipeline, in (main -> worker at hand-off), out (worker -> main at the fence),
# scratch (the worker gets its own scratch arena pointer), comma-separated, or - for none
TLS_FL = [sum({'in': 1, 'out': 2, 'scratch': 4, 'stack': 8}[f] for f in r[2].split(',') if f != '-') if len(r) > 2 else 0 for r in TLS_ROWS]
assert len(TLS) <= 16
# functions the profiler saw running on the Vita (tools/recomp/hot.txt): gcc's hot attribute groups them in .text.hot
HOT = {int(l.split()[0], 16) for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hot.txt'))
       if l.strip() and l[0] != '#'}
SUB = {}
for r in R32:
    SUB[r[1:]] = (r, 0, 16)  # ax, cx, dx, bx, sp, bp, si, di
for r, b in [('eax', 'a'), ('ecx', 'c'), ('edx', 'd'), ('ebx', 'b')]:
    SUB[b + 'l'] = (r, 0, 8)
    SUB[b + 'h'] = (r, 8, 8)

COND = {
    'o': 'of', 'no': '!of', 'b': 'cf', 'ae': '!cf', 'e': 'zf', 'ne': '!zf', 'be': '(cf|zf)', 'a': '!(cf|zf)',
    's': 'sf', 'ns': '!sf', 'p': 'pf', 'np': '!pf', 'l': '(sf!=of)', 'ge': '(sf==of)',
    'le': '(zf|(sf!=of))', 'g': '!(zf|(sf!=of))',
}
UNCOND_END = {'jmp', 'ret', 'int3', 'hlt'}

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True


def hx(v):
    return '0x%xu' % (v & 0xffffffff)


def mask(w):
    return hx((1 << w) - 1)


def sign(e, w):
    return '((%s >> %d) & 1)' % (e, w - 1)


class Fn:
    def __init__(self, f, img, index, funcs):
        self.f, self.img, self.index, self.funcs = f, img, index, funcs
        self.out = []
        self.standalone_only = False  # set by a setjmp call (see insn)
        self.ft = 'f80'  # x87 value type (emit: rc_ff when single() holds)
        self.insns = []
        self.ranges = list(f.ranges)
        for lo, hi in f.ranges:
            seq = list(md.disasm(img.read(lo, hi - lo + 1), lo))
            if not seq or seq[-1].address + seq[-1].size != hi + 1:
                raise Unsupported('range does not decode cleanly at %x' % lo)
            self.insns.append(seq)
        self.extend()

    def find_tables(self):
        """Switch tables Ghidra did not resolve (`jmp [reg*4 + table]` with no targets, FUN_007e6c9b): read the
        table from the image while the entries look like code near this function. MSVC puts any byte index
        table right after it, and those bytes do not look like code addresses."""
        text = next(s for s in self.img.sections if s[2] == '.text')
        for seq in self.insns:
            for i in seq:
                if i.mnemonic != 'jmp' or i.operands[0].type != X.X86_OP_MEM:  # also when Ghidra has some cases: it misses some
                    continue
                m = i.operands[0].mem
                if not m.index or m.scale != 4 or m.base or m.segment:
                    continue
                tbl, ts = m.disp & 0xffffffff, []
                while len(ts) < 1024:
                    try:
                        t = self.img.u32(tbl + 4 * len(ts))
                    except KeyError:
                        break
                    if not (text[0] <= t < text[0] + text[1] and abs(t - self.f.entry) < 0x10000):
                        break
                    ts.append(t)
                if ts:
                    self.f.jumps[i.address] = sorted(set(self.f.jumps.get(i.address, [])) | set(ts))

    def inside(self, a):
        return any(lo <= a <= hi for lo, hi in self.ranges)

    def extend(self):
        """Ghidra ends a body early when it wrongly thinks a call never returns (FUN_007df8cc). Code that the
        function jumps or falls into outside its body, and that is not another function's entry, is part of it:
        decode it as an extra chunk, until an unconditional end, known code, or another function."""
        starts = {i.address for seq in self.insns for i in seq}
        for _ in range(64):
            self.find_tables()  # chunks added last round can hold switch tables too
            pending = set()
            for seq in self.insns:
                for i in seq:
                    if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                        pending.add(i.operands[0].imm & 0xffffffff)
                if seq[-1].mnemonic not in UNCOND_END:
                    pending.add(seq[-1].address + seq[-1].size)
            for ts in self.f.jumps.values():
                pending.update(ts)
            # only code: Ghidra lists an import thunk's IAT slot (in .rdata) as its jmp's "target"
            text = next(x for x in self.img.sections if x[2] == '.text')
            pending = {t for t in pending if text[0] <= t < text[0] + text[1] and t not in starts and t not in Fn.real and not self.inside(t)}
            if not pending:
                return
            for t in sorted(pending):
                if t in starts or self.inside(t):
                    continue
                seq, a = [], t
                while len(seq) < 4000:
                    try:
                        i = next(md.disasm(self.img.read(a, 16), a), None)
                    except KeyError:
                        raise Unsupported('jump outside the image to %x' % a)
                    if i is None:
                        raise Unsupported('undecodable code at %x' % a)
                    seq.append(i)
                    a += i.size
                    if i.mnemonic in UNCOND_END or a in starts or a in Fn.real or self.inside(a):
                        break
                if self.inside(a) and a not in starts:
                    raise Unsupported('chunk runs into the middle of an instruction at %x' % a)
                self.insns.append(seq)
                self.ranges.append((t, a - 1))
                starts.update(i.address for i in seq)
        raise Unsupported('body keeps growing')

    # ---- operands ----
    def ea(self, op):
        m = op.mem
        parts = []
        if m.segment == X.X86_REG_FS:
            parts.append('c->fsbase')
        elif m.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES):
            raise Unsupported('segment')
        if m.base:
            parts.append(self.reg32(m.base))
        if m.index:
            parts.append('%s*%d' % (self.reg32(m.index), m.scale) if m.scale != 1 else self.reg32(m.index))
        if not parts:
            for k, (lo, n) in enumerate(TLS):
                if lo <= (m.disp & 0xffffffff) < lo + n:
                    return '(uint32_t)(c->tls[%d] + %s)' % (k, hx((m.disp & 0xffffffff) - lo))
        if m.disp or not parts:
            parts.append(hx(m.disp))
        return '(uint32_t)(%s)' % ' + '.join(parts)

    def reg32(self, r):
        n = self.i.reg_name(r)
        if n not in R32:
            raise Unsupported('address register ' + n)
        return n

    def rget(self, name):
        if name in R32:
            return name
        if name in SUB:
            r, sh, w = SUB[name]
            return '((%s >> %d) & %s)' % (r, sh, mask(w)) if sh else '(%s & %s)' % (r, mask(w))
        raise Unsupported('register ' + name)

    def rset(self, name, v):
        if name in R32:
            return '%s = %s;' % (name, v)
        if name in SUB:
            r, sh, w = SUB[name]
            m = ((1 << w) - 1) << sh
            return '%s = (%s & %s) | (((uint32_t)(%s) & %s) << %d);' % (r, r, hx(~m), v, mask(w), sh)
        raise Unsupported('register ' + name)

    def get(self, op, addr=None):
        """value of an integer operand, zero-extended to uint32_t"""
        if op.type == X.X86_OP_REG:
            return self.rget(self.i.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return hx(op.imm & ((1 << (op.size * 8)) - 1))
        if op.type == X.X86_OP_MEM:
            if op.size not in (1, 2, 4, 8):
                raise Unsupported('%d-byte memory operand' % op.size)
            return 'LD%d(%s)' % (op.size * 8, addr or self.ea(op))
        raise Unsupported('operand')

    def set(self, op, v, addr=None):
        if op.type == X.X86_OP_REG:
            return self.rset(self.i.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            return 'ST%d(%s, %s);' % (op.size * 8, addr or self.ea(op), v)
        raise Unsupported('operand')

    def rmw(self, op):
        """(prelude, addr name or None) so a read-modify-write evaluates a memory address once"""
        if op.type == X.X86_OP_MEM:
            return 'uint32_t A = %s; ' % self.ea(op), 'A'
        return '', None

    # ---- flags ----
    @staticmethod
    def szp(r, w):
        return 'zf = %s == 0; sf = %s; pf = PARITY(%s);' % (r, sign(r, w), r)

    # ---- control flow ----
    def jump(self, t):
        if self.inside(t):
            return ('BARRIER; ' if t <= self.i.address else '') + 'goto L_%x;' % t
        return '%sSYNC_OUT; rc_tail(c, %s); return;' % (self.spill(), hx(t))

    # ---- static x87 stack ----
    # In compiled code the x87 stack depth at each instruction is fixed. Where that holds for a whole function,
    # ST(i) becomes a local (s0..s7, kept in registers by the compiler) instead of c->st[(top+i)&7] in memory.
    # The Ctx stack stays the interface: values are spilled to it before calls and exits, and read back after.
    X87_TOK = re.compile(r'ST\((\d)\)|FPUSH\(|FPOP\(\)|FSW\(\)')

    @classmethod
    def x87_walk(cls, text, d):
        """(rewritten text, depth after) for the x87 code `text` run at depth d, or None if it reaches below
        the values this function pushed or past 8"""
        out, k = [], 0
        while True:
            m = cls.X87_TOK.search(text, k)
            if not m:
                out.append(text[k:])
                return ''.join(out), d
            out.append(text[k:m.start()])
            tok = m.group(0)
            if tok.startswith('ST('):
                n = int(m.group(1))
                if n >= d:
                    return None
                out.append('s%d' % (d - 1 - n))
                k = m.end()
            elif tok == 'FPOP()':
                if d == 0:
                    return None
                d -= 1
                k = m.end()
            elif tok == 'FSW()':
                out.append('FSWS(%d)' % d)
                k = m.end()
            else:  # FPUSH( expr ): expr at the depth before the push
                depth, j = 1, m.end()
                while depth:
                    depth += {'(': 1, ')': -1}.get(text[j], 0)
                    j += 1
                inner = cls.x87_walk(text[m.end():j - 1], d)
                if inner is None or inner[1] != d or d >= 8:
                    return None
                out.append('s%d = (%s)' % (d, inner[0]))
                d += 1
                k = j

    def spill(self):
        if not self.static:
            return ''
        return ''.join('FPUSH(s%d); ' % j for j in range(self.cur_d))

    @staticmethod
    def fill(n):
        return ''.join('s%d = ST(0); FPOP(); ' % j for j in reversed(range(n)))

    @staticmethod
    def is_x87(i):
        return i.mnemonic.startswith('f') or i.mnemonic == 'wait'

    # x87 signature of a callee: (values it takes from ST, values it leaves). Analysed per function (fsig, filled
    # by main before emitting), plus the CRT helpers that pass floats on the x87 stack.
    fsig = {}
    CRT_FSIG = {'_ftol': (1, 0), '_ftol2': (1, 0), '_ftol2_sse': (1, 0), '_CIpow': (2, 1), '_CIfmod': (2, 1),
                '_CIatan2': (2, 1), '_CIsqrt': (1, 1), '_CIsin': (1, 1), '_CIcos': (1, 1), '_CItan': (1, 1),
                '_CIatan': (1, 1), '_CIasin': (1, 1), '_CIacos': (1, 1), '_CIexp': (1, 1), '_CIlog': (1, 1),
                '_CIlog10': (1, 1), '_CIsinh': (1, 1), '_CIcosh': (1, 1), '_CItanh': (1, 1)}

    def call_sig(self, i):
        """(takes, leaves) for a call instruction, or None when unknown"""
        op = i.operands[0]
        if op.type == X.X86_OP_IMM:
            return Fn.fsig.get(op.imm & 0xffffffff)
        if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index:
            return self.CRT_FSIG.get(self.imports.get(op.mem.disp & 0xffffffff, ('', ''))[1])
        return None

    def x87_depths(self, entry_depth=0):
        """depth before each instruction, the call result deltas and the exit depth, or None if the function
        does not keep a fixed x87 depth (or uses no x87 at all)"""
        order = [i for seq in self.insns for i in seq]
        at = {i.address: i for i in order}
        nxt_of = {}
        for seq in self.insns:
            for k, i in enumerate(seq):
                nxt_of[i.address] = seq[k + 1].address if k + 1 < len(seq) else i.address + i.size
        if not any(self.is_x87(i) for i in order):
            return None
        texts = {}
        for i in order:
            if self.is_x87(i):
                self.i = i
                texts[i.address] = self.x87(i, i.mnemonic, i.operands)
        if self.f.entry not in at:
            return None
        depth, call_r, todo, exits = {self.f.entry: entry_depth}, {}, [self.f.entry], set()
        while todo:
            a = todo.pop()
            i, d = at[a], depth[a]
            m = i.mnemonic
            if a in texts:
                w = self.x87_walk(texts[a], d)
                if w is None:
                    return None
                d_out = w[1]
            elif m == 'call':
                sig = self.call_sig(i)
                if sig:
                    if d < sig[0]:
                        return None
                    r = sig[1] - sig[0]
                else:
                    # unknown callee: a float result is in ST(0), which the first x87 instruction after it shows,
                    # but only when nothing else is on the stack to read instead
                    if d:
                        return None
                    r, b = 0, nxt_of[a]
                    while b in at and not self.is_x87(at[b]) and not at[b].group(capstone.CS_GRP_JUMP) and at[b].mnemonic not in ('call', 'ret'):
                        b = nxt_of[b]
                    if b in texts and self.x87_walk(texts[b], d) is None:
                        r = 1
                call_r[a] = r
                d_out = d + r
            else:
                d_out = d
            if d_out > 8:
                return None
            succ = []
            if m == 'jmp':
                if i.operands[0].type == X.X86_OP_IMM:
                    succ.append(i.operands[0].imm & 0xffffffff)
                else:
                    succ += self.f.jumps.get(a, [])
            elif m in ('ret', 'int3', 'hlt'):
                if m == 'ret':
                    exits.add(d_out)
            else:
                if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                    succ.append(i.operands[0].imm & 0xffffffff)
                succ.append(nxt_of[a])
            for t in succ:
                if t not in at:
                    continue  # leaves the function
                if t in depth:
                    if depth[t] != d_out:
                        return None
                else:
                    depth[t] = d_out
                    todo.append(t)
        if len(exits) > 1:
            return None
        return depth, call_r, (exits.pop() if exits else entry_depth)

    def signature(self):
        """(takes, leaves, analysis) with the fewest taken values that analyses cleanly, or None"""
        for n in (0, 1, 2):
            sd = self.x87_depths(n)
            if sd:
                return n, sd[2], sd
        return None

    def single(self):
        """x87 math in single precision: the game made its device without D3DCREATE_FPU_PRESERVE, so D3D set the
        x87 to 24-bit precision on the game thread, which is what this matches (and floats are cheaper on ARM). Not
        for a function with an 8- or 10-byte x87 memory operand (doubles, 64-bit integers moved through fild/fistp,
        fsave images): those keep f80."""
        for seq in self.insns:
            for i in seq:
                if i.mnemonic.startswith('f') and any(o.type == X.X86_OP_MEM and o.size not in (2, 4) for o in i.operands):
                    return False
        return True

    def emit(self):
        f = self.f
        self.labels = set()
        for seq in self.insns:
            for i in seq:
                if (i.group(capstone.CS_GRP_JUMP)) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                    if self.inside(i.operands[0].imm):
                        self.labels.add(i.operands[0].imm)
        for ts in f.jumps.values():
            self.labels.update(t for t in ts if self.inside(t))
        for lo, hi in self.ranges[1:]:
            self.labels.add(lo)
        o = self.out
        self.static, self.cur_d = False, 0
        sig = self.signature()
        if sig:
            self.static, (self.depth, self.call_r, _) = True, sig[2]
        o.append('void %s%s_%x(Ctx* restrict c) {' % ('__attribute__((hot)) ' if f.entry in HOT else '', 'fo' if f.entry in NATIVE else 'f', f.entry))  # guest memory never aliases the Ctx
        self.ft = 'f80'
        if self.static:
            self.ft = 'rc_ff' if self.single() else 'f80'
            o.append('  %s s0, s1, s2, s3, s4, s5, s6, s7;  /* static x87 stack, takes %d leaves %d */ %s' % (self.ft, sig[0], sig[1], self.fill(sig[0])))
        o.append('  uint32_t %s;' % ', '.join('%s = c->%s' % (r, r) for r in R32))
        o.append('  uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0, fcc = 0;  /* fcc: x87 C0/C2/C3 */')
        o.append('  RC_RETSAVE;')
        o.append('  RC_HIT(%d);' % self.index[f.entry])
        if self.insns[0][0].address != f.entry:  # the entry is not in the first body range
            self.labels.add(f.entry)
            o.append('  goto L_%x;' % f.entry)
        for seq in self.insns:
            for i in seq:
                self.i = i
                if i.address in self.labels:
                    o.append('L_%x:' % i.address)
                o.append('  /* %x %s %s */ %s' % (i.address, i.mnemonic, i.op_str, self.insn_static(i)))
            last = seq[-1]
            if last.mnemonic not in UNCOND_END:
                nxt = last.address + last.size
                if self.inside(nxt):
                    self.labels.add(nxt)
                if self.static and last.address in self.depth:
                    self.i = last
                    w = self.x87_walk(self.x87(last, last.mnemonic, last.operands), self.depth[last.address]) if self.is_x87(last) else None
                    self.cur_d = w[1] if w else self.depth[last.address] + self.call_r.get(last.address, 0)
                o.append('  ' + self.jump(nxt))
        missing = self.labels - {i.address for seq in self.insns for i in seq}
        if missing:
            raise Unsupported('jump into the middle of an instruction at %x' % min(missing))
        o.append('}')
        text = '\n'.join(o)
        undefined = set(re.findall(r'goto (L_[0-9a-f]+);', text)) - set(re.findall(r'^(L_[0-9a-f]+):', text, re.M))
        if undefined:  # an overlapping decode that jumps outside what it covers
            raise Unsupported('jump to %s, which is not part of this body' % min(undefined))
        return text

    # register flow (regflow.py): what a callee reads and may change; filled by main before emitting
    leaves = frozenset()  # tiny leaf functions: direct calls use their inline copy fi_X (leaves.h)
    reads, clob, cclob, retw = {}, {}, {}, {}  # reads/cclob: as seen from a call site; clob: the function's own; retw: what its ret writes

    @staticmethod
    def sync_sel(rd, cl):
        """(spill, reload) around a call: only the callee's inputs out and what it may change back in (release
        builds; the hook build syncs everything, its callees may be original code)"""
        if rd is None or cl is None:
            return 'SYNC_OUT', 'SYNC_IN'
        return ('SYNC_OUT_SEL(%s)' % ', '.join(['c->%s = %s' % (r, r) for r in ['esp'] + regflow.names(rd)] +
                                               ['RC_POISON(%s)' % r for r in R32 if r != 'esp' and r not in regflow.names(rd)]),
                'SYNC_IN_SEL(%s)' % ', '.join('%s = c->%s' % (r, r) for r in ['esp'] + regflow.names(cl)))

    def insn_static(self, i):
        if not self.static:
            return self.insn(i)
        if i.address not in self.depth:  # unreachable from the entry
            return 'SYNC_OUT; rc_trap(c, %s, "unreachable"); return;' % hx(i.address)
        d = self.cur_d = self.depth[i.address]
        text = self.insn(i)
        if self.is_x87(i):
            return self.x87_walk(text, d)[0]
        if i.mnemonic == 'call':
            r = self.call_r[i.address]
            return '%s{ unsigned fd0 = c->fdepth; %s RC_CHECKFPU(fd0 + %d, %s); %s}' % (self.spill(), text, r, hx(i.address), self.fill(d + r))
        if i.mnemonic == 'ret':
            return self.spill() + text
        return text

    # mmx.h has one helper per name here: mmx_<mnemonic>(dest, src)
    MMX_OPS = frozenset('paddb paddw paddd psubb psubw psubd paddsb paddsw psubsb psubsw paddusb paddusw psubusb psubusw '
                  'pmullw pmulhw pmaddwd pcmpeqb pcmpeqw pcmpeqd pcmpgtb pcmpgtw pcmpgtd psllw pslld psllq psrlw psrld '
                  'psrlq psraw psrad pand pandn por pxor punpcklbw punpcklwd punpckldq punpckhbw punpckhwd punpckhdq '
                  'packsswb packuswb packssdw'.split())

    def mmx(self, i, m, ops):
        """MMX: mm<n> is c->mm[n], a 64-bit value"""
        def mm(op):
            n = self.i.reg_name(op.reg) if op.type == X.X86_OP_REG else ''
            return 'c->mm[%s]' % n[2:] if n.startswith('mm') else None

        def get(op):
            return mm(op) or '(uint64_t)' + self.get(op)

        def put(op, v):
            r = mm(op)
            return '%s = %s;' % (r, v) if r else self.set(op, '(uint32_t)(%s)' % v if op.size == 4 else v)
        if m == 'movq':
            return put(ops[0], get(ops[1]))
        if m == 'movd':
            return put(ops[0], '(uint64_t)(uint32_t)' + get(ops[1]))
        if m in self.MMX_OPS:
            return put(ops[0], 'mmx_%s(%s, %s)' % (m, get(ops[0]), get(ops[1])))
        raise Unsupported('mmx ' + m)

    # ---- instructions ----
    def insn(self, i):
        m = i.mnemonic
        ops = i.operands
        if m.startswith('f') or m in ('wait',):
            return self.x87(i, m, ops)
        if 'xmm' in i.op_str:
            raise Unsupported('simd ' + m)
        if 'mm' in i.op_str.replace('mmword', ''):
            return self.mmx(i, m, ops)
        nxt = i.address + i.size
        w = ops[0].size * 8 if ops else 32
        M = mask(w)

        # emms/femms only reset the MMX tags: cpuid hides MMX, so they only run in feature checks
        if m in ('nop', 'emms', 'femms') or m.startswith('prefetch'):
            return ''
        if m == 'mov':
            return self.set(ops[0], self.get(ops[1]))
        if m in ('movzx', 'movsx'):
            sw = ops[1].size * 8
            v = self.get(ops[1])
            if m == 'movsx':
                v = '(uint32_t)(int%d_t)%s' % (sw, v)
            return self.set(ops[0], '(%s & %s)' % (v, M))
        if m == 'lea':
            return self.set(ops[0], self.ea(ops[1]))
        if m == 'push':
            v = self.get(ops[0])
            if ops[0].type == X.X86_OP_IMM:
                v = hx(ops[0].imm)  # push imm8/imm32 pushes the sign-extended dword
            sz = 2 if ops[0].size == 2 else 4
            return '{ uint32_t v = %s; esp -= %d; ST%d(esp, v); }' % (v, sz, sz * 8)
        if m == 'pop':
            sz = 2 if ops[0].size == 2 else 4
            return '{ uint32_t v = LD%d(esp); esp += %d; %s }' % (sz * 8, sz, self.set(ops[0], 'v'))
        if m == 'pushal':
            return '{ uint32_t s = esp; %s esp -= 32; }' % ' '.join(
                'ST32(s - %d, %s);' % (4 * (k + 1), 's' if r == 'esp' else r) for k, r in enumerate(R32))
        if m == 'popal':
            return '{ %s esp += 32; }' % ' '.join(
                '%s = LD32(esp + %d);' % (r, 28 - 4 * k) for k, r in enumerate(R32) if r != 'esp')
        if m == 'leave':
            return 'esp = ebp; ebp = LD32(esp); esp += 4;'
        if m == 'xchg':
            a, b = ops
            if a.type == X.X86_OP_MEM:
                a, b = b, a
            if b.type == X.X86_OP_MEM:  # implicitly locked on x86
                if b.size != 4:
                    raise Unsupported('xchg%d' % (b.size * 8))
                return self.set(a, '__atomic_exchange_n((rc_u32*)RC_P(%s), %s, __ATOMIC_SEQ_CST)' % (self.ea(b), self.get(a)))
            return '{ uint32_t t = %s; %s %s }' % (self.get(a), self.set(a, self.get(b)), self.set(b, 't'))
        if m in ('add', 'sub', 'cmp', 'adc', 'sbb', 'and', 'or', 'xor', 'test'):
            pre, A = self.rmw(ops[0])
            a, b = self.get(ops[0], A), self.get(ops[1])
            if ops[1].type == X.X86_OP_IMM:
                b = hx(ops[1].imm & ((1 << w) - 1))
            s = '{ %suint32_t a = %s, b = %s, r; ' % (pre, a, b)
            if m in ('add', 'adc'):
                cin = ' + cf' if m == 'adc' else ''
                s += 'r = (a + b%s) & %s; ' % (cin, M)
                s += 'cf = %s; ' % ('(uint64_t)a + b + cf > %s' % M if m == 'adc' else 'r < a')
                s += 'of = %s; ' % sign('((a ^ r) & (b ^ r))', w)
            elif m in ('sub', 'cmp', 'sbb'):
                cin = ' - cf' if m == 'sbb' else ''
                s += 'r = (a - b%s) & %s; ' % (cin, M)
                s += 'cf = %s; ' % ('(uint64_t)a < (uint64_t)b + cf' if m == 'sbb' else 'a < b')
                s += 'of = %s; ' % sign('((a ^ b) & (a ^ r))', w)
            else:
                op = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[m]
                s += 'r = a %s b; cf = 0; of = 0; ' % op
            s += self.szp('r', w)
            if m not in ('cmp', 'test'):
                s += ' ' + self.set(ops[0], 'r', A)
            return s + ' }'
        if m in ('inc', 'dec', 'neg', 'not'):
            pre, A = self.rmw(ops[0])
            s = '{ %suint32_t a = %s, r; ' % (pre, self.get(ops[0], A))
            if m == 'inc':
                s += 'r = (a + 1) & %s; of = r == %s; %s' % (M, hx(1 << (w - 1)), self.szp('r', w))
            elif m == 'dec':
                s += 'r = (a - 1) & %s; of = a == %s; %s' % (M, hx(1 << (w - 1)), self.szp('r', w))
            elif m == 'neg':
                s += 'r = (0 - a) & %s; cf = a != 0; of = a == %s; %s' % (M, hx(1 << (w - 1)), self.szp('r', w))
            else:
                s += 'r = ~a & %s;' % M
            return s + ' ' + self.set(ops[0], 'r', A) + ' }'
        if m in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror'):
            pre, A = self.rmw(ops[0])
            n = self.get(ops[1]) if len(ops) > 1 else '1'
            s = '{ %suint32_t a = %s, n = %s & 31, r; if (n) { ' % (pre, self.get(ops[0], A), n)
            if m in ('shl', 'sal'):
                s += 'r = (uint32_t)(((uint64_t)a << n) & %s); cf = n <= %d ? (a >> (%d - n)) & 1 : 0; of = %s ^ cf; ' % (M, w, w, sign('r', w))
            elif m == 'shr':
                s += 'r = n < 32 ? a >> n : 0; cf = (a >> (n - 1)) & 1; of = %s; ' % sign('a', w)
            elif m == 'sar':
                s += 'int32_t sa = (int32_t)(int%d_t)a; r = (uint32_t)(sa >> (n > %d ? %d : n)) & %s; cf = (sa >> (n - 1 > %d ? %d : n - 1)) & 1; of = 0; ' % (w, w - 1, w - 1, M, w - 1, w - 1)
            elif m == 'rol':
                s += 'uint32_t k = n %% %d; r = k ? ((a << k) | (a >> (%d - k))) & %s : a; cf = r & 1; of = %s ^ cf; ' % (w, w, M, sign('r', w))
            else:
                s += 'uint32_t k = n %% %d; r = k ? ((a >> k) | (a << (%d - k))) & %s : a; cf = %s; of = %s ^ ((r >> %d) & 1); ' % (w, w, M, sign('r', w), sign('r', w), w - 2)
            if m not in ('rol', 'ror'):
                s += self.szp('r', w) + ' '
            return s + self.set(ops[0], 'r', A) + ' } }'
        if m in ('rcl', 'rcr'):
            # rotate the w+1 bit value cf:a; count is masked to 5 bits, then taken mod w+1
            pre, A = self.rmw(ops[0])
            n = self.get(ops[1]) if len(ops) > 1 else '1'
            W = w + 1
            s = '{ %suint32_t a = %s, k = (%s & 31) %% %d; if (k) { uint64_t v = ((uint64_t)cf << %d) | a, r; ' % (pre, self.get(ops[0], A), n, W, w)
            if m == 'rcl':
                s += 'r = ((v << k) | (v >> (%d - k))) & 0x%xull; cf = (r >> %d) & 1; of = %s ^ cf; ' % (W, (1 << W) - 1, w, sign('(uint32_t)r', w))
            else:
                s += 'of = %s ^ cf; r = ((v >> k) | (v << (%d - k))) & 0x%xull; cf = (r >> %d) & 1; ' % (sign('a', w), W, (1 << W) - 1, w)
            return s + self.set(ops[0], '((uint32_t)r & %s)' % M, A) + ' } }'
        if m in ('shld', 'shrd'):
            if w != 32:
                raise Unsupported(m + '16')
            pre, A = self.rmw(ops[0])
            s = '{ %suint32_t a = %s, b = %s, n = %s & 31, r; if (n) { ' % (pre, self.get(ops[0], A), self.get(ops[1]), self.get(ops[2]))
            if m == 'shld':
                s += 'r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1; '
            else:
                s += 'r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1; '
            s += 'of = %s ^ %s; %s ' % (sign('r', 32), sign('a', 32), self.szp('r', 32))
            return s + self.set(ops[0], 'r', A) + ' } }'
        if m == 'imul' and len(ops) > 1:
            if len(ops) == 2:
                a, b = self.get(ops[0]), self.get(ops[1])
            else:
                a, b = self.get(ops[1]), hx(ops[2].imm)
            return '{ int64_t p = (int64_t)(int%d_t)%s * (int%d_t)%s; uint32_t r = (uint32_t)p & %s; cf = of = p != (int%d_t)r; %s %s }' % (
                w, a, w, b, M, w, self.szp('r', w), self.set(ops[0], 'r'))
        if m in ('mul', 'imul'):
            v = self.get(ops[0])
            if w == 32:
                if m == 'mul':
                    return '{ uint64_t p = (uint64_t)eax * %s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); cf = of = edx != 0; }' % v
                return '{ int64_t p = (int64_t)(int32_t)eax * (int32_t)%s; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32); cf = of = p != (int32_t)eax; }' % v
            if w == 8:
                if m == 'mul':
                    return '{ uint32_t p = (eax & 0xff) * %s; %s cf = of = p > 0xff; }' % (v, self.rset('ax', 'p'))
                return '{ int32_t p = (int8_t)eax * (int8_t)%s; %s cf = of = p != (int8_t)p; }' % (v, self.rset('ax', 'p'))
            raise Unsupported(m + '16')
        if m in ('div', 'idiv'):
            v = self.get(ops[0])
            if w != 32:
                raise Unsupported(m + str(w))
            if m == 'div':
                return '{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t d = %s; eax = (uint32_t)(n / d); edx = (uint32_t)(n %% d); }' % v
            return '{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d = (int32_t)%s; eax = (uint32_t)(n / d); edx = (uint32_t)(n %% d); }' % v
        if m == 'cdq':
            return 'edx = (int32_t)eax < 0 ? 0xffffffffu : 0;'
        if m == 'cwde':
            return 'eax = (uint32_t)(int16_t)eax;'
        if m == 'cbw':
            return self.rset('ax', '(uint16_t)(int8_t)eax')
        if m == 'cwd':
            return self.rset('dx', '(int16_t)eax < 0 ? 0xffff : 0')
        if m.startswith('set'):
            return self.set(ops[0], COND[m[3:]])
        if m == 'sahf':
            return 'cf = (eax >> 8) & 1; pf = (eax >> 10) & 1; zf = (eax >> 14) & 1; sf = (eax >> 15) & 1;'
        if m == 'lahf':
            return self.rset('ah', '(sf << 7) | (zf << 6) | (pf << 2) | 2 | cf')
        if m == 'cld':
            return ''
        if m in ('clc', 'stc', 'cmc'):
            return {'clc': 'cf = 0;', 'stc': 'cf = 1;', 'cmc': 'cf ^= 1;'}[m]
        if m == 'cpuid':  # a CPU without MMX/SSE/3DNow!, so the game never picks its SIMD paths (left untranslated)
            return 'rc_cpuid(&eax, &ebx, &ecx, &edx);'
        if m == 'bswap':
            return self.set(ops[0], '__builtin_bswap32(%s)' % self.get(ops[0]))
        if m in ('bt',):
            return 'cf = (%s >> (%s & %d)) & 1;' % (self.get(ops[0]), self.get(ops[1]), w - 1)
        if m == 'int3':
            return 'SYNC_OUT; rc_trap(c, %s, "int3"); return;' % hx(i.address)
        if m.split()[-1][:-1] in ('movs', 'stos', 'cmps', 'scas', 'lods') and m.split()[-1][-1] in 'bwd':
            return self.string(m)

        # control flow
        if m == 'jmp':
            if ops[0].type == X.X86_OP_IMM:
                return self.jump(ops[0].imm)
            s = '{ uint32_t t = %s; ' % self.get(ops[0])
            ts = sorted(set(t for t in self.f.jumps.get(i.address, []) if self.inside(t)))
            if ts:
                s += 'switch (t) { %s default: break; } ' % ' '.join('case %s: goto L_%x;' % (hx(t), t) for t in ts)
            return s + '%sSYNC_OUT; rc_jmp(c, t, %s); return; }' % (self.spill(), hx(i.address))
        if m[0] == 'j' and m[1:] in COND:
            return 'if (%s) { %s }' % (COND[m[1:]], self.jump(ops[0].imm))
        if m == 'jecxz':
            return 'if (!ecx) { %s }' % self.jump(ops[0].imm)
        if m == 'call':
            if (ops[0].type == X.X86_OP_MEM and not ops[0].mem.base and not ops[0].mem.index and self.imports.get(ops[0].mem.disp & 0xffffffff, ('', ''))[1] == 'setjmp3')                     or (ops[0].type == X.X86_OP_IMM and (ops[0].imm & 0xffffffff) in self.setjmp_thunks):
                # setjmp in this function's own C frame, which longjmp (shimmed: rc_longjmp) returns to. Only the
                # standalone build can route longjmp there, so the hook build leaves these functions original.
                self.standalone_only = True
                return '{ rc_jmprec* rec_ = rc_setjmp_rec(c, LD32(esp)); SYNC_OUT; if (!__builtin_setjmp(rec_->buf)) { rc_setjmp_save(c, rec_); c->eax = 0; } SYNC_IN; }'
            if ops[0].type == X.X86_OP_IMM:
                t = ops[0].imm & 0xffffffff
                if t in self.index:
                    k = self.index[t]
                    so, si = self.sync_sel(Fn.reads.get(t), Fn.cclob.get(t))
                    return ('%s; if (RC_ON(%d)) { c->esp -= 4; ST32(c->esp, %s); %s_%x(c); RC_CHECKRET(%s, %s, %s); } '
                            'else rc_native(c, %s); %s;' % (so, k, hx(nxt), 'fi' if t in Fn.leaves else 'f', t, hx(nxt), hx(t), hx(nxt), hx(t), si))
                return 'SYNC_OUT; rc_native(c, %s); SYNC_IN;' % hx(t)
            if ops[0].type == X.X86_OP_MEM and not ops[0].mem.base and not ops[0].mem.index and (ops[0].mem.disp & 0xffffffff) in self.imports:
                so, si = 'SYNC_OUT', 'SYNC_IN'  # an import: its shim may read any register
            else:
                so, si = self.sync_sel(regflow.ABI_READS, regflow.ABI_CLOB)  # vtable / pointer: the MSVC convention
            return '{ uint32_t t = %s; %s; rc_call(c, t, %s); %s; }' % (self.get(ops[0]), so, hx(nxt), si)
        if m == 'ret':
            n = ops[0].imm if ops else 0
            mine = Fn.clob.get(self.f.entry)
            ret = 'SYNC_OUT'
            if mine is not None:
                kept = [r for r in R32 if r != 'esp' and r not in regflow.names(mine)]
                ret = ''.join('RC_RETCHK(%s, %s); ' % (r, hx(self.f.entry)) for r in kept)
                ret += 'SYNC_RET_SEL(%s)' % ', '.join('c->%s = %s' % (r, r) for r in ['esp'] + regflow.names(Fn.retw[self.f.entry]))
            return 'c->target = LD32(esp); esp += %d; %s; return;' % (4 + n, ret)
        raise Unsupported(m)

    def string(self, m):
        parts = m.split()
        rep, base = (parts[0] if len(parts) > 1 else ''), parts[-1]
        n = {'b': 1, 'w': 2, 'd': 4}[base[-1]]
        w = n * 8
        acc = self.rget({1: 'al', 2: 'ax', 4: 'eax'}[n])
        kind = base[:-1]
        if kind == 'movs':
            body = 'ST%d(edi, LD%d(esi)); esi += %d; edi += %d;' % (w, w, n, n)
        elif kind == 'stos':
            body = 'ST%d(edi, %s); edi += %d;' % (w, acc, n)
        elif kind == 'lods':
            body = '%s esi += %d;' % (self.rset({1: 'al', 2: 'ax', 4: 'eax'}[n], 'LD%d(esi)' % w), n)
        else:
            a, b = ('LD%d(esi)' % w, 'LD%d(edi)' % w) if kind == 'cmps' else (acc, 'LD%d(edi)' % w)
            inc = 'esi += %d; edi += %d;' % (n, n) if kind == 'cmps' else 'edi += %d;' % n
            body = '{ uint32_t a = %s, b = %s, r = (a - b) & %s; cf = a < b; of = %s; %s } %s' % (
                a, b, mask(w), sign('((a ^ b) & (a ^ r))', w), self.szp('r', w), inc)
        if not rep:
            return body
        if kind in ('movs', 'stos', 'lods'):
            if rep != 'rep':
                raise Unsupported(m)
            return 'while (ecx) { %s ecx--; }' % body
        stop = {'repe': '!zf', 'repne': 'zf'}.get(rep)
        if not stop:
            raise Unsupported(m)
        return 'while (ecx) { %s ecx--; if (%s) break; }' % (body, stop)

    # ---- x87 ----
    def fop(self, op):
        """an x87 operand as an f80 expression"""
        if op.type == X.X86_OP_REG:
            return 'ST(%d)' % self.sti(op)
        a = self.ea(op)
        return {4: '(%s)LDF32(%%s)' % self.ft, 8: '(f80)LDF64(%s)', 10: 'LDF80(%s)'}[op.size] % a

    def iop(self, op):
        a = self.ea(op)
        return {2: '(%s)(int16_t)LD16(%%s)' % self.ft, 4: '(%s)(int32_t)LD32(%%s)' % self.ft, 8: '(f80)(int64_t)LD64(%s)'}[op.size] % a

    def sti(self, op):
        n = self.i.reg_name(op.reg)
        if not n.startswith('st'):
            raise Unsupported('x87 operand ' + n)
        return int(n[3]) if n.startswith('st(') else 0

    def x87(self, i, m, ops):
        if m in ('wait', 'fwait', 'ffree', 'fnop'):
            return ''
        if m == 'fnclex':
            return 'FNCLEX();'
        if m == 'fld':
            return 'FPUSH(%s);' % self.fop(ops[0])
        if m == 'fild':
            return 'FPUSH(%s);' % self.iop(ops[0])
        consts = {'fld1': 0, 'fldz': 1, 'fldpi': 2, 'fldl2e': 3, 'fldl2t': 4, 'fldlg2': 5, 'fldln2': 6}
        if m in consts:
            return 'FPUSH(rc_fconst(%d));' % consts[m]
        if m in ('fst', 'fstp'):
            pop = ' FPOP();' if m == 'fstp' else ''
            op = ops[0]
            if op.type == X.X86_OP_REG:
                return 'ST(%d) = ST(0);%s' % (self.sti(op), pop)
            a = self.ea(op)
            return {4: 'STF32(%s, ST(0));', 8: 'STF64(%s, ST(0));', 10: 'STF80(%s, ST(0));'}[op.size] % a + pop
        if m in ('fist', 'fistp'):
            op = ops[0]
            s = 'ST%d(%s, rc_fist%d(ST(0)));' % (op.size * 8, self.ea(op), op.size * 8)
            return s + (' FPOP();' if m == 'fistp' else '')
        if m == 'fxch':
            k = self.sti(ops[0]) if ops else 1
            if len(ops) == 2:
                k = max(self.sti(ops[0]), self.sti(ops[1]))
            return '{ f80 t = ST(0); ST(0) = ST(%d); ST(%d) = t; }' % (k, k)
        arith = {'fadd': '{a} + {b}', 'fsub': '{a} - {b}', 'fsubr': '{b} - {a}', 'fmul': '{a} * {b}',
                 'fdiv': '{a} / {b}', 'fdivr': '{b} / {a}'}
        base = m[:-1] if m.endswith('p') and m[:-1] in arith else m
        if base in arith or (m[:2] == 'fi' and 'f' + m[2:] in arith):
            if m[:2] == 'fi':
                return 'ST(0) = %s;' % arith['f' + m[2:]].format(a='ST(0)', b=self.iop(ops[0]))
            pop = m != base
            if len(ops) == 2:
                d, s = self.sti(ops[0]), self.sti(ops[1])
            elif len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
                return 'ST(0) = %s;' % arith[base].format(a='ST(0)', b=self.fop(ops[0]))
            elif len(ops) == 1:
                d, s = (self.sti(ops[0]), 0) if pop else (0, self.sti(ops[0]))
            else:
                d, s = 1, 0
            r = 'ST(%d) = %s;' % (d, arith[base].format(a='ST(%d)' % d, b='ST(%d)' % s))
            return r + (' FPOP();' if pop else '')
        if m in ('fcom', 'fcomp', 'fucom', 'fucomp', 'ficom', 'ficomp'):
            b = 'ST(1)' if not ops else (self.iop(ops[0]) if m.startswith('fi') else self.fop(ops[0]))
            return 'FCMP(ST(0), %s);%s' % (b, ' FPOP();' if m.endswith('p') else '')
        if m in ('fcompp', 'fucompp'):
            return 'FCMP(ST(0), ST(1)); FPOP(); FPOP();'
        if m == 'ftst':
            return 'FCMP(ST(0), 0);'
        if m in ('fcomi', 'fcomip', 'fucomi', 'fucomip'):
            k = self.sti(ops[-1]) if ops else 1
            s = '{ f80 a = ST(0), b = ST(%d); zf = a == b || a != a || b != b; pf = a != a || b != b; cf = a < b || pf; of = sf = 0; }' % k
            return s + (' FPOP();' if m.endswith('p') else '')
        if m == 'fnstsw':
            return self.set(ops[0], 'FSW()') if ops else self.rset('ax', 'FSW()')
        if m == 'fnstcw':
            return 'ST16(%s, c->fcw);' % self.ea(ops[0])
        if m == 'fldcw':
            return 'rc_fldcw(c, LD16(%s));' % self.ea(ops[0])
        one = {'fchs': '-ST(0)', 'fabs': '__builtin_fabsl(ST(0))', 'fsqrt': 'rc_fsqrt(ST(0))',
               'fsin': 'rc_fsin(ST(0))', 'fcos': 'rc_fcos(ST(0))', 'frndint': 'rc_frndint(ST(0))'}
        if m in one:
            return 'ST(0) = %s;' % one[m]
        if m == 'fsincos':
            return '{ f80 x = ST(0); ST(0) = rc_fsin(x); FPUSH(rc_fcos(x)); }'
        if m == 'fptan':
            return 'ST(0) = rc_fptan(ST(0)); FPUSH(rc_fconst(0));'
        if m in ('fpatan', 'fyl2x'):
            return '{ f80 r = rc_%s(ST(1), ST(0)); FPOP(); ST(0) = r; }' % m
        raise Unsupported(m)


PRELUDE = '''#include "rt.h"
#define SYNC_OUT (%s)
#define SYNC_IN (%s)
extern volatile long lifted_hits[];
/* Instrumentation for the hook build (hit counts for the journal, per-function enable for the quarantine, return
   checks). RC_RELEASE compiles it out: every function translated, no counters. */
/* static x87 stack: the status word's TOP as the real stack would have it */
#define FSWS(d) ((uint16_t)((rc_fsw(c) & ~0x3800) | (((c->ftop - (d)) & 7) << 11)))
#ifdef RC_RELEASE
/* register flow (tools/recomp/regflow.py): a direct call spills only what the callee reads, reloads what it changes */
#define SYNC_OUT_SEL(...) (__VA_ARGS__)
#define SYNC_IN_SEL(...) (__VA_ARGS__)
#define SYNC_RET_SEL(...) (__VA_ARGS__)
#ifdef RC_RETCHK /* vpc check: a register the flow says the function keeps must still match the Ctx at its ret */
void rc_retbad(uint32_t fn, const char* reg, uint32_t local, uint32_t ctx);
#define RC_RETSAVE uint32_t rk_eax = eax, rk_ecx = ecx, rk_edx = edx, rk_ebx = ebx, rk_ebp = ebp, rk_esi = esi, rk_edi = edi;     (void)rk_eax, (void)rk_ecx, (void)rk_edx, (void)rk_ebx, (void)rk_ebp, (void)rk_esi, (void)rk_edi
#define RC_RETCHK(r, fn) do { if (r != rk_##r) rc_retbad(fn, #r, r, rk_##r); } while (0)
/* (poisoning a callee's non-inputs does not work: the poison is a Ctx change nobody restores) */
#define RC_POISON(r) ((void)0)
#else
#define RC_RETSAVE ((void)0)
#define RC_RETCHK(r, fn) ((void)0)
#define RC_POISON(r) ((void)0)
#endif
#define RC_CHECKFPU(n, at) ((void)0)
#define RC_HIT(k) ((void)0)
#ifdef RC_PROFILE /* the profiler's shadow call stack (rt.h) */
#define RC_ON(k) (rc_prof_push(c, (k)), 1)
#define RC_CHECKRET(t, callee, next) rc_prof_pop(c)
#else
#define RC_ON(k) 1
#define RC_CHECKRET(t, callee, next) ((void)0)
#endif
#else
#define SYNC_OUT_SEL(...) SYNC_OUT
#define SYNC_IN_SEL(...) SYNC_IN
#define SYNC_RET_SEL(...) SYNC_OUT
#define RC_RETSAVE ((void)0)
#define RC_RETCHK(r, fn) ((void)0)
#define RC_POISON(r) ((void)0)
#define RC_CHECKFPU(n, at) do { if (c->fdepth != (n)) rc_trap(c, at, "x87 depth after a call is not what the static stack assumed"); } while (0)
#define RC_HIT(k) (lifted_hits[k]++)
#define RC_ON(k) rc_on[k]
#define RC_CHECKRET(t, callee, next) do { if (c->target != (t)) rc_bad_return(c, callee, next); } while (0)
#endif
''' % (', '.join('c->%s = %s' % (r, r) for r in R32), ', '.join('%s = c->%s' % (r, r) for r in R32))


def main():
    out = sys.argv[1]
    which = sys.argv[2] if len(sys.argv) > 2 else 'all'
    img = Image()
    funcs, imports = load_cfg()
    Fn.imports = imports
    # `jmp [setjmp3]` thunks: their callers count as setjmp callers too
    slots = {a for a, (_, n) in imports.items() if n == 'setjmp3'}
    Fn.setjmp_thunks = set()
    for a in funcs:
        i = next(md.disasm(img.read(a, 6), a), None)
        if i and i.mnemonic == 'jmp' and i.operands[0].type == X.X86_OP_MEM and not i.operands[0].mem.base and (i.operands[0].mem.disp & 0xffffffff) in slots:
            Fn.setjmp_thunks.add(a)
    print('setjmp thunks:', ' '.join('%x' % a for a in sorted(Fn.setjmp_thunks)))
    # Real functions: called somewhere (any E8 rel32 that lands on an entry; false hits only make it "real") or
    # pointed to by data (vtables, callbacks). Other Ghidra "functions" are chunks of a bigger one (FUN_006d0ae1
    # starts with a je on flags set by FUN_006d0ac0): jumps into them are inlined instead of tail calls.
    import struct
    Fn.real = set()
    for va, size, name, data in img.sections:
        if name == '.text':
            for k in range(len(data) - 5):
                if data[k] == 0xE8:
                    t = (va + k + 5 + struct.unpack_from('<i', data, k + 1)[0]) & 0xffffffff
                    if t in funcs:
                        Fn.real.add(t)
        for k in range(0, len(data) - 3, 1 if name == '.text' else 4):
            v = struct.unpack_from('<I', data, k)[0]
            if v in funcs:
                Fn.real.add(v)
    Fn.real -= {a for a, f in funcs.items() if f.name.startswith('DATA_')}
    print('%d of %d entries are real functions, %d are chunks' % (len(Fn.real), len(funcs), len(funcs) - len(Fn.real)))
    want = sorted(funcs) if which == 'all' else sorted(int(l.split()[0], 16) for l in open(which) if l.strip() and l[0] != '#')
    os.makedirs(out, exist_ok=True)
    ok, skipped = {}, []
    # first pass: which functions translate at all (needed before emitting, for direct calls)
    for a in want:
        if a not in funcs:
            skipped.append((a, 'not a function in cfg.txt'))
            continue
        try:
            fn = Fn(funcs[a], img, _AnyIndex(), funcs)
            fn.emit()
            ok[a] = fn
        except Unsupported as e:
            skipped.append((a, str(e)))
    addrs = sorted(ok)
    index = {a: k for k, a in enumerate(addrs)}
    if os.environ.get('RC_NO_REGFLOW') != '1':
        reads, Fn.clob, Fn.retw = regflow.analyse(ok, set(ok), NATIVE_ABI, NATIVE, Fn.setjmp_thunks, imports)
        # at a call site: a native.txt address runs native.c's f_X, not the translation the flow describes
        Fn.reads, Fn.cclob = dict(reads), dict(Fn.clob)
        for a in NATIVE:
            Fn.reads[a], Fn.cclob[a] = (regflow.ABI_READS, regflow.ABI_CLOB) if a in NATIVE_ABI else (regflow.ALL, regflow.ALL)
    # The 5-byte hook jmp must stay inside the function and no code may branch into the bytes it replaces.
    targets = set()
    for fn in ok.values():
        for seq in fn.insns:
            for i in seq:
                if (i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL)) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                    targets.add(i.operands[0].imm & 0xffffffff)
        for ts in fn.f.jumps.values():
            targets.update(ts)
    entries = sorted(funcs)
    nxt = dict(zip(entries, entries[1:]))

    def hookable(a):
        if a not in Fn.real or funcs[a].name.startswith(('DISC_', 'DATA_')):  # chunk (entered by a jump with live flags) or discovered (may be data that decodes)
            return False
        first = [hi - lo + 1 for lo, hi in funcs[a].ranges if lo == a]
        return first and first[0] >= 5 and nxt.get(a, 1 << 32) - a >= 5 and not any(t in targets for t in range(a + 1, a + 5))
    hooks = {a for a in addrs if hookable(a)}
    with open(os.path.join(out, 'recomp_tls.c'), 'w') as t:
        t.write('#include "rt.h"\n/* the real addresses of the per-thread globals (tools/recomp/tls.txt) */\n')
        t.write('void rc_tls_init(Ctx* c) {\n')
        for k, (lo, n) in enumerate(TLS):
            t.write('    c->tls[%d] = 0x%xu;  /* %d bytes */\n' % (k, lo, n))
        t.write('}\n')
        # for the pipeline (native.c): sizes and copy directions (1 in, 2 out, 4 scratch); one extra entry for an empty list
        t.write('const unsigned rc_tls_n = %d;\n' % len(TLS))
        t.write('const uint32_t rc_tls_addr0[] = {%s};\n' % ', '.join(['0x%xu' % lo for lo, n in TLS] + ['0']))
        t.write('const uint32_t rc_tls_size[] = {%s};\n' % ', '.join(['0x%xu' % n for lo, n in TLS] + ['0']))
        t.write('const uint8_t rc_tls_flag[] = {%s};\n' % ', '.join([str(f) for f in TLS_FL] + ['0']))
    with open(os.path.join(out, 'recomp.h'), 'w') as h:
        h.write(PRELUDE)
        for a in addrs:
            h.write('void f_%x(Ctx* c);\n' % a)
            if a in NATIVE:
                h.write('void fo_%x(Ctx* c);\n' % a)
    # x87 signatures: callers depend on callees, so iterate to a fixed point
    thunk_sigs = {}  # `jmp [import]` thunks to the CRT x87 helpers
    for a in funcs:
        i = next(md.disasm(img.read(a, 6), a), None)
        if i and i.mnemonic == 'jmp' and i.operands[0].type == X.X86_OP_MEM and not i.operands[0].mem.base and not i.operands[0].mem.index:
            name = imports.get(i.operands[0].mem.disp & 0xffffffff, ('', ''))[1]
            if name in Fn.CRT_FSIG:
                thunk_sigs[a] = Fn.CRT_FSIG[name]
    Fn.fsig = dict(thunk_sigs)
    for rnd in range(12):  # until no signature changes, so callers and callees agree
        sigs = dict(thunk_sigs)
        for a in addrs:
            fn = Fn(funcs[a], img, index, funcs)
            g = fn.signature()
            if g:
                sigs[a] = g[:2]
        done = sigs == Fn.fsig
        Fn.fsig = sigs
        if done:
            break
    print('%d functions have a static x87 stack (signatures settled after %d rounds%s)' % (len(Fn.fsig), rnd + 1, '' if done else ', NOT settled'))
    # Tiny leaves (no calls, no jumps out of the body, at most 16 instructions): an always-inline copy in leaves.h for
    # direct calls, so gcc sees both sides of the call and drops most of the register traffic through the Ctx. f_X
    # stays for calls through pointers and the tables.
    Fn.leaves = set()
    if os.environ.get('RC_NO_LEAVES') != '1':
        for a, fn in ok.items():
            body = [i for seq in fn.insns for i in seq]
            if a in NATIVE or a not in Fn.real or len(body) > 16 or any(i.group(capstone.CS_GRP_CALL) for i in body):
                continue
            leaves_body = all(not i.group(capstone.CS_GRP_JUMP) or (i.operands[0].type == X.X86_OP_IMM and fn.inside(i.operands[0].imm & 0xffffffff))
                              for i in body)
            ends_inside = all(seq[-1].mnemonic in UNCOND_END or fn.inside(seq[-1].address + seq[-1].size) for seq in fn.insns)
            if leaves_body and ends_inside:
                Fn.leaves.add(a)
    with open(os.path.join(out, 'leaves.h'), 'w') as h:
        h.write('/* tiny leaf functions as always-inline copies for direct calls (recomp.py Fn.leaves) */\n')
        for a in sorted(Fn.leaves):
            text = Fn(funcs[a], img, index, funcs).emit()
            head, rest = text.split('\n', 1)
            h.write('static inline __attribute__((always_inline)) void fi_%x(Ctx* restrict c) {\n%s\n\n' % (a, rest))
    print('%d tiny leaves inlined at direct calls' % len(Fn.leaves))
    per = 400
    code_ranges = []  # (lo, hi, function index): every byte each translation was made from, inlined chunks included
    standalone_only = set()  # setjmp callers: their longjmp needs the standalone build's shim
    for old in glob.glob(os.path.join(out, 'recomp_[0-9][0-9][0-9].c')):
        os.remove(old)  # fewer functions means fewer files; a stale one would define functions twice
    for n in range(0, len(addrs), per):
        with open(os.path.join(out, 'recomp_%03d.c' % (n // per)), 'w') as c:
            c.write('#include "recomp.h"\n#include "leaves.h"\n\n')
            for a in addrs[n:n + per]:
                fn = Fn(funcs[a], img, index, funcs)
                c.write(fn.emit() + '\n\n')
                code_ranges.extend((lo, hi, index[a]) for lo, hi in fn.ranges)
                if fn.standalone_only:
                    standalone_only.add(a)
    with open(os.path.join(out, 'recomp_table.c'), 'w') as t:
        t.write('#include "recomp.h"\n')
        # native.txt functions: src/vita/native.c defines f_X; builds without it (the PC ones) run the translation
        t.write(''.join('__attribute__((weak)) void f_%x(Ctx* c) { fo_%x(c); }\n' % (a, a) for a in addrs if a in NATIVE))
        t.write('const unsigned rc_count = %d, lifted_count = %d;\n' % (len(addrs), len(addrs)))
        t.write('const uint32_t rc_addr[] = {%s};\n' % ','.join(hx(a) for a in addrs))
        t.write('rc_fn const rc_impl[] = {%s};\n' % ','.join('f_%x' % a for a in addrs))
        t.write('void* const lifted_impl[] = {%s};\n' % ','.join('(void*)f_%x' % a for a in addrs))
        t.write('const uint32_t rc_entries[] = {%s};\nconst unsigned rc_nentries = %d;\n' % (','.join(hx(a) for a in sorted(funcs)), len(funcs)))
        t.write('unsigned char rc_on[%d];\nvolatile long lifted_hits[%d];\n' % (len(addrs), len(addrs)))
        # hook stubs: push the translated function, enter through the bridge (x86 only: the Vita has no native
        # code to enter from)
        t.write('#if defined(__i386__) && !defined(USM_VPC)\n__asm__(".text\\n"\n')
        for a in addrs:
            t.write('  "_rcs_%x: .byte 0x68\\n .long _f_%x\\n jmp _rc_enter\\n"\n' % (a, a))  # push imm32
        t.write(');\n')
        t.write(''.join('extern char rcs_%x[];\n' % a for a in addrs))
        t.write('struct lifted_entry { unsigned addr; void* fn; };\n')
        t.write('const struct lifted_entry lifted_table[] = {%s};\n' % ','.join('{%s, %s}' % (hx(a), 'rcs_%x' % a if a in hooks else '0') for a in addrs))
        # every function's native-to-translated entry, hookable or not (the standalone build enters through these)
        t.write('void* const rc_stub[] = {%s};\n#endif\n' % ','.join('rcs_%x' % a for a in addrs))
        t.write('const unsigned char rc_standalone_only[] = {%s};\n' % ','.join('1' if a in standalone_only else '0' for a in addrs))
        code_ranges.sort()
        t.write('/* code each translation was made from: {lo, hi, index}, sorted by lo */\n')
        t.write('const uint32_t rc_ranges[][3] = {%s};\nconst unsigned rc_nranges = %d;\n' % (
            ','.join('{%s,%s,%d}' % (hx(lo), hx(hi), k) for lo, hi, k in code_ranges), len(code_ranges)))
    with open(os.path.join(out, 'skipped.tsv'), 'w') as s:
        for a, why in skipped:
            s.write('%x\t%s\n' % (a, why))
    print('%d functions translated (%d can be hooked), %d skipped' % (len(addrs), len(hooks), len(skipped)))


class _AnyIndex(dict):
    """first pass: every callee looks translatable, only the instruction coverage is being checked"""
    def __contains__(self, k):
        return False

    def __getitem__(self, k):
        return 0


if __name__ == '__main__':
    main()
