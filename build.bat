@echo off
rem Builds Scripts\usm_re.asi. Needs llvm-mingw (winget install MartinStorsjo.LLVM-MinGW.UCRT) and pip cmake + ninja.
rem Set USM_GAME to your game folder (e.g. "C:\Games\USM") to copy the .asi into its Scripts folder.
setlocal
cd /d "%~dp0"

set "PATH=%APPDATA%\Python\Python314\Scripts;%PATH%"
where i686-w64-mingw32-clang++ >nul 2>nul || for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*") do for /d %%e in ("%%d\llvm-mingw-*") do set "PATH=%%e\bin;%PATH%"
where i686-w64-mingw32-clang++ >nul 2>nul || (echo llvm-mingw is needed: winget install MartinStorsjo.LLVM-MinGW.UCRT & goto :end)
where cmake >nul 2>nul || (echo CMake is needed: py -m pip install cmake ninja & goto :end)
where ninja >nul 2>nul || (echo Ninja is needed: py -m pip install ninja & goto :end)

rem Show the build and keep a copy in build-log.txt.
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=i686-w64-mingw32-clang++ -DCMAKE_C_COMPILER=i686-w64-mingw32-clang; if ($LASTEXITCODE) { exit 1 }; cmake --build build; exit $LASTEXITCODE } 2>&1 | Tee-Object -FilePath build-log.txt; exit $LASTEXITCODE" || (echo Build failed, see build-log.txt & goto :end)

if defined USM_GAME (
    copy /y "build\usm_re.asi" "%USM_GAME%\Scripts\" >nul && echo Copied to "%USM_GAME%\Scripts\usm_re.asi"
)
:end
pause
