# usm

A PS Vita port of Ultimate Spider-Man (PC, 2005), made by statically recompiling the PC game's `USM.exe` to C
(every x86 instruction becomes equivalent C, like N64Recomp / XenonRecomp), then running that on the Vita with
its own Windows, Direct3D 9, DirectSound and input layer on top of vitaGL.

**No game code or data is in this repo.** You need your own copy of the PC game. The translated code is
generated on your machine from your `USM.exe`.

## What you need
- The PC version's `USM.exe` (SHA-256 `5d554ef69e8066eed9e327a4fe331992124aee6de16af54bd7ac5c5dcb0488b8`) and its
  `Data` folder.
- [VitaSDK](https://vitasdk.org) with vitaGL, vitashark and mathneon (`vdpm vitaGL vitashark mathneon`).
- Python 3 with `capstone`, and `cmake` + `ninja`: `py -m pip install capstone cmake ninja`.
- On the Vita: `ur0:data/libshacccg.suprx` (the shader compiler, extracted the usual way for vitaGL games).

## Build the VPK
```
set VITASDK=C:\vitasdk
set USM_EXE=C:\Games\USM\USM.exe
build_vita.bat
```
The first run translates `USM.exe` into `src/recomp/gen` (a few minutes, ~230 MB of C). The result is
`build-vita/usm.vpk`.

## Build the PC versions
Needs llvm-mingw (`winget install MartinStorsjo.LLVM-MinGW.UCRT`) besides the above. After the translation step:
`build.bat` builds `build/usm_re.asi` (hooked into the running game through an ASI loader; set `USM_GAME` to have it
copied into the game's `Scripts` folder) and `build/usm_pc.exe` (the game running only translated code: copy it next
to `USM.exe`). The functions in `tools/recomp/native.txt` run their plain translation there; the hand-written
versions in `src/vita/native.c` are Vita-only.

## Install
1. Install `usm.vpk` with VitaShell.
2. Copy `USM.exe` and the `Data` folder from your PC install to `ux0:data/usm/`.
3. That's all for movies and cutscenes too: they play straight from the game's own `Data/movies/pc/*.dat` (Bink),
   decoded with FFmpeg's Bink decoders (LGPL 2.1, `vendor/ffmpeg`).
4. Start the game. If USM.exe or the shader compiler is missing, it says so on screen. With an (empty)
   `ux0:data/usm/debug.txt` it writes `ux0:data/usm/log.txt` (frame times, settings in use, errors).

## Controls
Left stick move, right stick camera, Cross jump, Square punch, Triangle kick, L web zip, R web swing, Circle wall
crawl, Start pause, Select map, D-pad menus. Prompts show the Vita's buttons as icons. Quick-time events
(left/right click): L and R.

## Settings
Optional text files in `ux0:data/usm/`, each holding one number:

| File | Default | What it does |
|---|---|---|
| `density.txt` | `0.15` | People and traffic, 0..1 (1 = as the PC game). The biggest speed setting. |
| `distance.txt` | `120` | View distance; past it the city fades into fog. `0` = everything, no fog. |
| `texture_max.txt` | `256` | Largest texture size kept (`0` = full size). |
| `resolution.txt` | registry | Render size, e.g. `640x362` (from `160x90` to `960x544`). |
| `early_cull.txt` | `1` | `0` turns off skipping far objects before the game prepares them. |
| `build_cull.txt` | `2` | The game stops preparing objects past this many times the view distance. `0` = the game's own (3 km). |
| `anim_half.txt` | `12` | Characters farther than this (metres) from Spider-Man animate every other frame. `0` = off. |
| `fps_cap.txt` | `0` | Show frames at a steady rate (e.g. `20`) instead of whenever ready. |
| `pipe.txt` | on | `0` draws on the game thread instead of a second core (for debugging). |

## Layout
- `tools/recomp/`: the translator (`recomp.py`), its control-flow data (`data/cfg.txt`, exported from Ghidra with
  `tools/ghidra/ExportCFG.java`), and the lists of functions replaced by native code (`native.txt`) or grouped as hot
  (`hot.txt`).
- `src/recomp/`: the runtime the translated code runs on.
- `src/vita/`: the Vita side: memory and loader, Windows/D3D9/DirectSound/input shims, the vitaGL renderer
  (`gl.c`, shaders translated by `d3d_cg.c`), native replacements of hot functions (`native.c`).
- `vita/`: the VPK's CMake project; `vita/art` + `tools/make_livearea.py` make its LiveArea images (`vita/sce_sys`),
  `tools/make_glyphs.py` the button icons (`src/vita/glyphs.h`).
- `vpc/`, `src/vpc/`: the same Vita code built for Windows on desktop GL, for debugging without the device
  (`cmake -S vpc -B build-vpc -G Ninja -DCMAKE_C_COMPILER=i686-w64-mingw32-clang -DUSM_DIR=C:/Games/USM/`).
- `CMakeLists.txt`, `build.bat`, `src/pc/`: the PC builds the translation was proven on (`usm_pc.exe`, standalone;
  `usm_re.asi`, hooked into the running game).
- `docs/`: how the recompiler works (`recomp.md`), the Vita port's design (`vita.md`), notes on the binary.
