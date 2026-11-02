"""Which guest registers each translated function reads from the Ctx on entry, and which it may change, so a direct
call only spills what the callee reads and reloads what it may change (recomp.py SYNC_OUT_SEL / SYNC_IN_SEL).

Both sets include everything below the function: direct callees by their own sets (a fixed point over the call
graph). Anything not analysable counts as all registers: calls to imports and untranslated code, setjmp, indirect
jumps, tail jumps to unknown code. Indirect calls (vtables, function pointers) follow the MSVC convention every
address-taken function keeps: arguments in ecx/edx and the stack, eax/ecx/edx changed, ebx/esi/edi/ebp kept.
A callee-saved register a function pushes at entry and pops before every ret counts as kept, and its push as no read.
"""
import capstone
from capstone import x86 as X

R = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
BIT = {r: 1 << k for k, r in enumerate(R)}
ALL = 0xff
ESP = BIT['esp']
ABI_READS, ABI_CLOB = BIT['ecx'] | BIT['edx'] | ESP, BIT['eax'] | BIT['ecx'] | BIT['edx'] | ESP
SAVED_OK = BIT['ebx'] | BIT['esi'] | BIT['edi'] | BIT['ebp']
# sub-register name -> (32-bit register, partial)
SUBREG = {}
for r in R:
    SUBREG[r] = (r, False)
    SUBREG[r[1:]] = (r, True)
for r, b in [('eax', 'a'), ('ecx', 'c'), ('edx', 'd'), ('ebx', 'b')]:
    SUBREG[b + 'l'] = (r, True)
    SUBREG[b + 'h'] = (r, True)
UNCOND_END = {'jmp', 'ret', 'int3', 'hlt'}


# capstone 5 leaves out implicit registers of these (cmpxchg's eax write, xlatb's ebx/al, enter's esp/ebp, the BCD
# adjusts): taken as reading and changing everything
UNSURE = {'xlatb', 'cmpxchg', 'enter', 'aam', 'aad', 'aaa', 'aas', 'daa', 'das', 'int', 'into', 'bound', 'arpl'}


def access(i):
    """(read mask, fully written mask) of one instruction, from capstone's explicit + implicit operands"""
    if i.mnemonic in UNSURE:
        return ALL, 0
    try:
        rd, wr = i.regs_access()
    except capstone.CsError:
        return ALL, 0
    r = w = 0
    for x in rd:
        s = SUBREG.get(i.reg_name(x))
        if s:
            r |= BIT[s[0]]
    for x in wr:
        s = SUBREG.get(i.reg_name(x))
        if s:
            if s[1]:
                r |= BIT[s[0]]  # a partial write keeps the rest of the register
            else:
                w |= BIT[s[0]]
    ops = i.operands
    if i.mnemonic in ('xor', 'sub', 'sbb') and len(ops) == 2 and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG \
            and ops[0].reg == ops[1].reg and i.mnemonic != 'sbb':
        s = SUBREG.get(i.reg_name(ops[0].reg))
        if s and not s[1]:
            r &= ~BIT[s[0]]  # xor r, r: no read
    return r, w


class Info:
    """per-function facts, independent of the other functions"""
    def __init__(self, fn, translated, native_abi, native, setjmp_thunks, imports):
        self.entry = fn.f.entry
        self.order = [i for seq in fn.insns for i in seq]
        self.at = {i.address: k for k, i in enumerate(self.order)}
        self.inside = fn.inside
        self.jumps = fn.f.jumps
        self.acc = [access(i) for i in self.order]
        self.writes = 0
        for r, w in self.acc:
            self.writes |= w
        for i in self.order:  # partial writes count as changes too
            if i.mnemonic in UNSURE:
                self.writes = ALL
                continue
            try:
                for x in i.regs_access()[1]:
                    s = SUBREG.get(i.reg_name(x))
                    if s:
                        self.writes |= BIT[s[0]]
            except capstone.CsError:
                self.writes = ALL
        # each instruction's kind: ('call', target or None, how), ('tail', target or None), ('ret',), ('jmp', [inside targets], default all)
        self.calls = []  # direct translated callees (for the fixed point)
        self.tails = []
        self.fixed_reads = 0  # reads/clobbers of calls that do not depend on other functions
        self.fixed_clob = 0
        self.kind = []
        for i in self.order:
            m, ops = i.mnemonic, i.operands
            k = None
            if m == 'call':
                t = ops[0].imm & 0xffffffff if ops[0].type == X.X86_OP_IMM else None
                if t is not None and t in setjmp_thunks:
                    k = ('call', ALL, ALL, None)
                elif t is not None:
                    if t in native_abi:
                        k = ('call', ABI_READS, ABI_CLOB, None)
                    elif t in translated and t not in native:
                        k = ('call', 0, 0, t)
                        self.calls.append(t)
                    else:
                        k = ('call', ALL, ALL, None)
                elif ops[0].type == X.X86_OP_MEM and not ops[0].mem.base and not ops[0].mem.index and (ops[0].mem.disp & 0xffffffff) in imports:
                    k = ('call', ALL, ALL, None)  # an import: shims are free to read anything
                else:
                    k = ('call', ABI_READS, ABI_CLOB, None)
                self.fixed_clob |= k[2]
            elif m == 'ret':
                k = ('ret',)
            elif m == 'jmp' and ops[0].type == X.X86_OP_IMM:
                t = ops[0].imm & 0xffffffff
                k = ('goto', t) if self.inside(t) else self.tail(t, translated, native)
            elif m == 'jmp':
                ts = sorted(set(t for t in self.jumps.get(i.address, []) if self.inside(t)))
                k = ('switch', ts)
                self.fixed_clob = ALL  # rc_jmp: anything
            elif m[0] == 'j' and ops and ops[0].type == X.X86_OP_IMM:
                t = ops[0].imm & 0xffffffff
                k = ('cond', t if self.inside(t) else None, None if self.inside(t) else self.tail(t, translated, native))
            elif m in ('int3', 'hlt'):
                k = ('stop',)
            self.kind.append(k)
            # running off the body's end into another function: recomp.py emits a tail call there
            nxt = i.address + i.size
            if (k is None or k[0] in ('call', 'cond')) and not self.inside(nxt):
                self.tail(nxt, translated, native)
        self.saved = self.find_saved()

    def tail(self, t, translated, native):
        if t in translated and t not in native:
            self.tails.append(t)
            return ('tail', t)
        self.fixed_clob = ALL
        return ('tail', None)

    def find_saved(self):
        """callee-saved registers pushed in the entry block (before any write or branch) and popped before every ret"""
        k = self.at.get(self.entry)
        if k is None:
            return 0
        pushed, written = 0, 0
        while k < len(self.order):
            i = self.order[k]
            if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL) or i.mnemonic in UNCOND_END:
                break
            if i.mnemonic == 'push' and i.operands[0].type == X.X86_OP_REG:
                s = SUBREG.get(i.reg_name(i.operands[0].reg))
                if s and not s[1] and BIT[s[0]] & SAVED_OK and not written & BIT[s[0]]:
                    pushed |= BIT[s[0]]
            written |= self.acc[k][1]
            k += 1
            if k < len(self.order) and self.order[k].address != i.address + i.size:
                break  # end of this range
        if not pushed:
            return 0
        rets = [k for k, i in enumerate(self.order) if i.mnemonic == 'ret']
        if not rets:
            return 0
        labels = set()
        for i in self.order:
            if i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                labels.add(i.operands[0].imm & 0xffffffff)
        for ts in self.jumps.values():
            labels.update(ts)
        for k in rets:
            # straight back from the ret: pops count; other code may sit between them if it writes no kept register;
            # stop at control flow, and after a jump target (paths entering there skip what lies before it)
            popped, j = 0, k - 1
            if self.order[k].address in labels:
                return 0
            while j >= 0 and self.order[j].address + self.order[j].size == self.order[j + 1].address:
                i = self.order[j]
                if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL) or i.mnemonic in UNCOND_END:
                    break
                if i.mnemonic == 'pop' and i.operands[0].type == X.X86_OP_REG:
                    s = SUBREG.get(i.reg_name(i.operands[0].reg))
                    if s and not s[1] and not popped & BIT[s[0]]:
                        popped |= BIT[s[0]]
                elif self.acc[j][1] & SAVED_OK & ~popped:
                    break  # writes a kept register before its pop (going backwards: after the restore is not possible here)
                if i.address in labels:
                    break
                j -= 1
            pushed &= popped
        return pushed

    def clobbers(self, clob):
        c = self.writes | self.fixed_clob
        for t in self.calls:
            c |= clob.get(t, ALL)
        c &= ~self.saved
        for t in self.tails:
            c |= clob.get(t, ALL)
        return c | ESP

    def reads(self, reads, clob, myclob):
        """registers live at the entry (backward liveness; calls kill nothing)"""
        n = len(self.order)
        live = [0] * n
        succ = [[] for _ in range(n)]
        const = [0] * n  # live-in contributed at this instruction regardless of successors
        for k, i in enumerate(self.order):
            kd = self.kind[k]
            nxt = self.at.get(i.address + i.size)
            falls = nxt if nxt is not None and self.inside(i.address + i.size) else None
            if kd is None or kd[0] == 'call':
                if falls is not None:
                    succ[k].append(falls)
                else:
                    const[k] |= ALL  # runs off into another function: rc_tail
            elif kd[0] == 'ret':
                const[k] |= myclob | ESP
            elif kd[0] == 'goto':
                if kd[1] in self.at:
                    succ[k].append(self.at[kd[1]])
                else:
                    const[k] |= ALL
            elif kd[0] == 'tail':
                const[k] |= (reads.get(kd[1], ALL) | myclob) if kd[1] is not None else ALL
            elif kd[0] == 'switch':
                succ[k].extend(self.at[t] for t in kd[1] if t in self.at)
                const[k] |= ALL
            elif kd[0] == 'cond':
                if kd[1] is not None and kd[1] in self.at:
                    succ[k].append(self.at[kd[1]])
                elif kd[1] is not None:
                    const[k] |= ALL
                else:
                    t = kd[2]
                    const[k] |= (reads.get(t[1], ALL) | myclob) if t[1] is not None else ALL
                if falls is not None:
                    succ[k].append(falls)
                else:
                    const[k] |= ALL
        gen, kill = [0] * n, [0] * n
        for k, i in enumerate(self.order):
            r, w = self.acc[k]
            kd = self.kind[k]
            if kd and kd[0] == 'call':
                r |= kd[1] if kd[3] is None else reads.get(kd[3], ALL)
                w = 0
            if i.mnemonic == 'push' and i.operands and i.operands[0].type == X.X86_OP_REG:
                s = SUBREG.get(i.reg_name(i.operands[0].reg))
                if s and not s[1] and BIT[s[0]] & self.saved:
                    r &= ~BIT[s[0]]  # saving a kept register is no use of its value
            gen[k], kill[k] = r | const[k], w
        work = list(range(n - 1, -1, -1))
        on = [True] * n
        pred = [[] for _ in range(n)]
        for k in range(n):
            for s in succ[k]:
                pred[s].append(k)
        while work:
            k = work.pop()
            on[k] = False
            out = 0
            for s in succ[k]:
                out |= live[s]
            v = gen[k] | (out & ~kill[k])
            if v != live[k]:
                live[k] = v
                for p in pred[k]:
                    if not on[p]:
                        on[p] = True
                        work.append(p)
        e = self.at.get(self.entry)
        return (live[e] if e is not None else ALL) | ESP


def analyse(fns, translated, native_abi, native, setjmp_thunks, imports, log=print):
    """fns: {addr: Fn}; returns (reads, clobbers) as {addr: mask}"""
    info = {}
    for a, fn in fns.items():
        info[a] = Info(fn, translated, native_abi, native, setjmp_thunks, imports)
    callers = {}
    for a, inf in info.items():
        for t in inf.calls + inf.tails:
            callers.setdefault(t, set()).add(a)
    clob = {a: 0 for a in info}
    work = set(info)
    while work:
        a = work.pop()
        c = info[a].clobbers(clob)
        if c != clob[a]:
            clob[a] = c
            work |= callers.get(a, set())
    # what a ret must write back: its changes, plus every register the function or a callee may have put a value of
    # its own into the Ctx (a full sync around an import, then restored from the stack): the Ctx copy must end as it
    # began for those, as callers that reload everything (natives, tail calls) read it
    touched = {a: 0 for a in info}
    work = set(info)
    while work:
        a = work.pop()
        t = info[a].writes | BIT['eax'] | ESP
        for b in info[a].calls + info[a].tails:
            t |= touched.get(b, ALL)
        if t != touched[a]:
            touched[a] = t
            work |= callers.get(a, set())
    retw = {a: clob[a] | touched[a] for a in info}
    reads = {a: 0 for a in info}
    work = set(info)
    rounds = 0
    while work:
        a = work.pop()
        rounds += 1
        r = info[a].reads(reads, clob, clob[a])
        if r != reads[a]:
            reads[a] = r
            work |= callers.get(a, set())
    full_r = sum(1 for v in reads.values() if v == ALL)
    full_c = sum(1 for v in clob.values() if v == ALL)
    log('register flow: %d functions, %d read every register, %d may change every one (%d evaluations)' % (len(info), full_r, full_c, rounds))
    return reads, clob, retw


def names(mask):
    return [r for r in R if mask & BIT[r] and r != 'esp']
