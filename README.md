# Ultimate Spider-Man

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
crawl, rear touchpad camera lock, Start pause, Select map, D-pad menus. Prompts show the Vita's buttons as icons.
Quick-time events (left/right click): L and R.

## Settings
Press **Settings** on the game's LiveArea page to change these. Each one is saved as a small text file in
`ux0:data/usm/`, so you can also edit them by hand:

| File | Default | What it does |
|---|---|---|
| `density.txt` | `0.15` | People and traffic, 0..1 (`1` = as the PC game). The biggest speed setting. |
| `distance.txt` | `120` | View distance; past it the city fades into fog. `0` = everything, no fog. |
| `resolution.txt` | `960x544` | Size of the 3D picture, e.g. `640x362`, scaled to the screen. Costs little speed on the Vita. |
| `texture_max.txt` | `256` | Largest texture kept. `0` = full size. |
| `dxt_native.txt` | `2` | World texture detail: `1` high, `2` medium, `3` low, `0` the older blurry path. |
| `anim_half.txt` | `12` | Characters farther than this (metres) from Spider-Man animate every other frame. `0` = off. |
| `build_cull.txt` | `2` | The game stops preparing objects past this many times the view distance. `0` = the game's own (3 km). |
| `fps_cap.txt` | `30` | Frame rate cap. `0` = none. `20`, `30` and `60` are held by the display, for even frames. |
| `cut_fog.txt` | `0` | `1` keeps the fog in cutscenes. Faster, but hides the far city. |
| `smoke.txt` | `2` | Particles drawn from big smoke and fire effects: `1` all, `2` every other one, `3` every third. |
| `map_lod.txt` | `4` | How small (pixels) far boxes may get before they are dropped from the zoomed-out map and from heavy cutscenes. `0` = keep all. |
| `language.txt` | `0` | `0` English, `1` French, `2` German, `3` Spanish, `4` Italian. See Languages. |

For debugging only:

| File | Default | What it does |
|---|---|---|
| `debug.txt` | off | Create it (empty is fine) to write `log.txt`. The previous launch's log is kept as `log_prev.txt`. |
| `early_cull.txt` | `1` | `0` off; `2` checks the skips and logs mistakes. |
| `pipe.txt` | on | `0` draws on the game thread instead of the other cores. |
| `overlap.txt` | `1` | `0` stops the second core from running while the next frame is prepared. |
| `gl_cache.txt` | `1` | `0` sends every graphics call again on each draw. |
| `release_later.txt` | `1` | `0` releases graphics data at once, which pauses more often. |
| `vsync.txt` | `1` | `0` holds the frame rate cap with a timer instead of the display. |

If the game stops with an error, `ux0:data/usm/crash.txt` has a short note about it, even without `debug.txt`. Attach
it to bug reports.

The first time each area runs, it can pause briefly while shaders compile. They are then kept in
`ux0:data/usm/shadercache/`.

## Languages
The game picks its text from `Data/usm_lte.usm` (English), `usm_ltf.usm` (French), `usm_ltg.usm` (German),
`usm_lts.usm` (Spanish) or `usm_lti.usm` (Italian). A PC install only has its own language, and none of this is shipped
here, so to play in another language use the `Data` folder of that language's PC release, then pick it in Settings. If
the file is missing the game stays in English.

## Which game files work
Use the `Data` folder of a full PC install (about 1.6 GB, with `Data/movies`, `Data/Sound`, `Data/packs`). Cut-down
copies are often smaller because music, speech or movies were removed: the game may still run, but those parts will be
silent or missing, and anything else missing can crash it later.

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