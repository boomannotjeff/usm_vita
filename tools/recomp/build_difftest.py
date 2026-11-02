"""Builds difftest.exe from a recomp.py output dir. usage: build_difftest.py <gen dir>"""
import glob, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

gen = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', '..', 'src', 'recomp')
cc = glob.glob(os.path.expandvars(r'%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*\llvm-mingw-*\bin\i686-w64-mingw32-clang.exe'))[0]
flags = ['-std=gnu11', '-O2', '-w', '-mno-sse', '-mfpmath=387', '-fno-strict-aliasing', '-fwrapv', '-mllvm', '-fixup-byte-word-insts=false', '-I' + src, '-I' + gen]
obj = os.path.join(gen, 'obj')
os.makedirs(obj, exist_ok=True)


def cc1(src_file, extra=()):
    o = os.path.join(obj, os.path.splitext(os.path.basename(src_file))[0] + '.o')
    if os.path.exists(o) and os.path.getmtime(o) > max(os.path.getmtime(src_file), os.path.getmtime(os.path.join(src, 'rt.h'))) and 'recomp_' in o:
        return o
    r = subprocess.run([cc, *flags, *extra, '-c', src_file, '-o', o], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr[:3000])
    return o


with ThreadPoolExecutor(8) as ex:
    objs = list(ex.map(cc1, sorted(glob.glob(os.path.join(gen, 'recomp_*.c')))))
objs += [cc1(os.path.join(src, 'rt.c')), cc1(os.path.join(here, 'difftest.c'), ['-O0'])]
r = subprocess.run([cc, *objs, '-o', os.path.join(gen, sys.argv[2] if len(sys.argv) > 2 else 'difftest.exe'), '-Wl,--image-base,0x10000000', '-static'], capture_output=True, text=True)
sys.exit(r.stderr[:3000]) if r.returncode else print('built', os.path.join(gen, sys.argv[2] if len(sys.argv) > 2 else 'difftest.exe'))
