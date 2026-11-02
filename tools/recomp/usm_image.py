"""USM.exe image and Ghidra control-flow export (cfg.txt from ExportCFG.java), shared by the recompiler tools."""
import os
import struct

EXE = os.environ.get('USM_EXE', 'USM.exe')  # your own copy of the game's exe (see README)
CFG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'cfg.txt')


class Image:
    def __init__(self, path=EXE):
        d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        nsec, = struct.unpack_from('<H', d, pe + 6)
        optsz, = struct.unpack_from('<H', d, pe + 20)
        self.base, = struct.unpack_from('<I', d, pe + 24 + 28)
        self.sections = []  # (va, vsize, name, bytes)
        for i in range(nsec):
            o = pe + 24 + optsz + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode()
            vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
            self.sections.append((self.base + va, vsz, name, d[raw:raw + rsz].ljust(vsz, b'\0')))

    def read(self, va, n):
        for s, sz, _, b in self.sections:
            if s <= va < s + sz:
                return b[va - s:va - s + n]
        raise KeyError(hex(va))

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]


class Func:
    __slots__ = ('entry', 'name', 'ranges', 'blocks', 'jumps')

    def __init__(self, entry, name):
        self.entry, self.name, self.ranges, self.blocks, self.jumps = entry, name, [], set(), {}


def load_cfg(path=CFG, extra=True):
    """Ghidra's export, plus the functions discover.py found (cfg_extra.txt) unless extra=False"""
    import os
    funcs, imports, f = {}, {}, None
    paths = [path] + ([path.replace('cfg.txt', 'cfg_extra.txt')] if extra and os.path.exists(path.replace('cfg.txt', 'cfg_extra.txt')) else [])
    for line in (l for p in paths for l in open(p)):
        p = line.split()
        k = p[0]
        if k == 'F':
            f = funcs[int(p[1], 16)] = Func(int(p[1], 16), p[2])
        elif k == 'R':
            f.ranges.append((int(p[1], 16), int(p[2], 16)))
        elif k == 'B':
            f.blocks.add(int(p[1], 16))
        elif k == 'J' and ':' not in p[2]:  # jmp through an import slot (EXTERNAL:..) is not a switch
            f.jumps.setdefault(int(p[1], 16), []).append(int(p[2], 16))
        elif k == 'I':
            imports[int(p[1], 16)] = (p[2], p[3])
    return funcs, imports
