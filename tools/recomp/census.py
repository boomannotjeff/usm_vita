"""Count instruction mnemonics, prefixes and fs: use across every function, to size the recompiler."""
import collections
import capstone
from usm_image import Image, load_cfg

img = Image()
funcs, imports = load_cfg()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
mn, fn_with = collections.Counter(), collections.Counter()
fs_funcs = 0
for f in funcs.values():
    seen, fs = set(), False
    for lo, hi in f.ranges:
        for i in md.disasm(img.read(lo, hi - lo + 1), lo):
            m = i.mnemonic
            mn[m] += 1
            seen.add(m)
            fs |= 'fs:' in i.op_str
    for m in seen: fn_with[m] += 1
    fs_funcs += fs
print(len(funcs), 'functions,', sum(mn.values()), 'instructions,', len(mn), 'mnemonics,', fs_funcs, 'functions touch fs:')
print('imports by dll:', collections.Counter(d for d, _ in imports.values()))
for m, c in mn.most_common():
    print(f'{m:12} {c:8} {fn_with[m]:6}')
