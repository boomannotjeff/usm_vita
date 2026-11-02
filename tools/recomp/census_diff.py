"""What the Vita build still lacks: compares the PC census (usm_pc_census.txt from usm_pc.exe run with
USM_PC_CENSUS=1) against the shims in src/vita. Lists imports and COM methods the game called that have no C
implementation (they'd hit a survey stub returning 0), and every interface the game asked QueryInterface for.
Without a census file, lists every import of USM.exe (imports_gen.c) that has no shim.
usage: py tools/recomp/census_diff.py [usm_pc_census.txt]"""
import glob
import os
import re
import sys

VITA = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'vita')
DEFAULT = {'QueryInterface', 'AddRef', 'Release'}  # com.c gives every vtable these
# the census names the class it probed; the Vita build serves both with one vtable
ALIAS = {'IDirectSoundBuffer': 'IDirectSoundBuffer8'}
IIDS = {
    '279afa85-4981-11ce-a521-0020af0be560': 'IDirectSoundBuffer',
    '6825a449-7524-4d82-920f-50e36ab3ab1e': 'IDirectSoundBuffer8',
    '279afa84-4981-11ce-a521-0020af0be560': 'IDirectSound3DListener',
    '279afa86-4981-11ce-a521-0020af0be560': 'IDirectSound3DBuffer',
    'b0210783-89cd-11d0-af08-00a0c925cd16': 'IDirectSoundNotify',
    '00000000-0000-0000-c000-000000000046': 'IUnknown',
    '85c31227-3de5-4f00-9b3a-f11ac38c18b5': 'IDirect3DTexture9',
    '0cfbaf3a-9ff6-429a-99b3-a2796af8b89b': 'IDirect3DSurface9',
}


def strip(src):
    """comments out, and object-like macros (shared ShimDef rows) expanded"""
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    macros = dict(re.findall(r'^#define (\w+) (.+)$', src, re.M))
    for _ in range(3):
        for name, body in macros.items():
            src = re.sub(r'\b%s\b(?!\()' % name, lambda m: body, src)
    return src


def tables(src):
    """ShimDef arrays in one file: name -> set of method/import names that have a C function"""
    out = {}
    for m in re.finditer(r'ShimDef (\w+)\[\] = \{(.*?)\};', src, re.S):
        out[m.group(1)] = {n for n, fn in re.findall(r'\{"([^"]+)",\s*(\w+)\}', m.group(2)) if fn != 'NULL'}
    return out


imports, com = {'_iob', '_acmdln', '_adjust_fdiv'}, {}  # C runtime variables: crt_data_import in shims_crt.c
for path in glob.glob(os.path.join(VITA, '*.c')):
    src = strip(open(path, errors='replace').read())
    t = tables(src)
    for name, entries in t.items():
        if name.startswith('shims_'):
            imports |= entries
    for m in re.finditer(r'com_vtable(?:_ext)?\("(\w+)",\s*(\w+)(?:,\s*(\w+))?\)', src):
        have = com.setdefault(m.group(1), set(DEFAULT))
        for tab in m.group(2, 3):
            if tab:
                have |= t.get(tab, set())

methods = {}
for name, body in re.findall(r'ComMethod m_(\w+)\[\] = \{(.*?)\};', open(os.path.join(VITA, 'com_gen.c')).read()):
    methods[name] = re.findall(r'\{"(\w+)", \d+\}', body)

if len(sys.argv) < 2:
    missing = []
    for dll, name in re.findall(r'\{"([^"]+)", "([^"]+)", -?\d+\}', open(os.path.join(VITA, 'imports_gen.c')).read()):
        if name not in imports:
            missing.append('%s!%s' % (dll, name))
    print('%d imports without a shim:' % len(missing))
    for m in missing:
        print('  ' + m)
    # a survey stub returns S_OK without writing the out-parameter: the game then uses garbage
    risky = ['%s::%s' % (i, n) for i, have in sorted(com.items()) for n in methods.get(i, [])
             if n not in have and re.match(r'(Get|Create|Lock|Enum|Find|Query)', n)]
    print('%d COM methods with outputs and no implementation (survey stub would leave them unwritten):' % len(risky))
    for r in risky:
        print('  ' + r)
    sys.exit()

miss_imp, miss_com, iids = [], [], []
for line in open(sys.argv[1]):
    p = line.split()
    if not p:
        continue
    if p[0] == 'import':
        name = p[1].split('!', 1)[1]
        if name not in imports:
            miss_imp.append(p[1])
    elif p[0] == 'com':
        iface = ALIAS.get(p[1], p[1])
        m = methods.get(iface, [])
        k = int(p[2])
        name = m[k] if k < len(m) else '#%d' % k
        if iface not in com:
            miss_com.append('%s::%s (no vtable on the Vita)' % (iface, name))
        elif name not in com[iface]:
            miss_com.append('%s::%s' % (iface, name))
    elif p[0] == 'iid':
        iids.append('%s asked for %s' % (p[1], IIDS.get(p[2], p[2])))

for title, rows in (('imports called without a shim', miss_imp), ('COM methods called without a shim', miss_com),
                    ('QueryInterface requests (check each is served)', iids)):
    print('%s: %d' % (title, len(rows)))
    for r in sorted(set(rows)):
        print('  ' + r)
