@echo off
rem Builds the PS Vita VPK: build-vita\usm.vpk.
rem Needs: VitaSDK (VITASDK set; vitaGL, vitashark, mathneon from vdpm), Python 3 with capstone (py -m pip install capstone),
rem cmake + ninja (py -m pip install cmake ninja), and USM_EXE set to your own USM.exe (only for the first build:
rem the translator turns it into src\recomp\gen).
setlocal
cd /d "%~dp0"
if not defined VITASDK (echo Set VITASDK to your VitaSDK folder. & exit /b 1)
set "PATH=%VITASDK%\bin;%PATH%"
if not exist src\recomp\gen\recomp.h (
    if not defined USM_EXE (echo Set USM_EXE to your USM.exe, e.g. set USM_EXE=C:\Games\USM\USM.exe & exit /b 1)
    echo Translating USM.exe into src\recomp\gen ^(a few minutes^)...
    py tools\recomp\recomp.py src\recomp\gen all || (echo Translation failed. & exit /b 1)
)
cmake -S vita -B build-vita -G Ninja || exit /b 1
cmake --build build-vita || (echo Build failed. & exit /b 1)
echo Done: build-vita\usm.vpk
