# usm_vpc.exe: the Vita build's code (translated game + src/vita runtime, shims and renderer) on the PC, through the
# stand-ins in src/vpc (Win32 threads, desktop GL for vitaGL, keyboard/script for the pad). For finding renderer bugs
# without the device. Runs from the PC game folder (USM_DIR): USM.exe + Data, writes log.txt/registry.txt there.
#   cmake -S vpc -B build-vpc -G Ninja -DCMAKE_C_COMPILER=i686-w64-mingw32-clang "-DUSM_DIR=C:/Games/USM/"
#   cmake --build build-vpc
cmake_minimum_required(VERSION 3.20)
project(usm_vpc C)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(USM_DIR "" CACHE STRING "the game folder (USM.exe, Data), with a trailing slash")
if(NOT USM_DIR)
    message(FATAL_ERROR "set USM_DIR to your game folder, e.g. -DUSM_DIR=C:/Games/USM/")
endif()
file(GLOB RECOMP_GEN CONFIGURE_DEPENDS ${SRC}/recomp/gen/recomp_*.c)
if(NOT RECOMP_GEN)
    message(FATAL_ERROR "src/recomp/gen is empty: run  py tools/recomp/recomp.py src/recomp/gen all")
endif()
file(GLOB VITA_SRC CONFIGURE_DEPENDS ${SRC}/vita/*.c)

add_executable(usm_vpc ${RECOMP_GEN} ${SRC}/recomp/fpu_port.c ${VITA_SRC} ${SRC}/vpc/vpc.c)
# src/vpc first: its psp2/ and vitaGL.h stand in for the VitaSDK's
target_include_directories(usm_vpc PRIVATE ${SRC}/vpc ${SRC}/recomp ${SRC}/recomp/gen ${SRC}/vita)
target_compile_definitions(usm_vpc PRIVATE RC_RETCHK RC_RELEASE RC_PORTABLE USM_VPC USM_DEBUG USM_VITAGL "USM_DIR=\"${USM_DIR}\"")
# as the Vita build, plus: unsigned char like ARM, SSE doubles for the portable x87, and the clang 23 miscompile
# workaround all x86 builds of the translated code need (see the top-level CMakeLists.txt)
target_compile_options(usm_vpc PRIVATE -O2 -std=gnu11 -w -fno-strict-aliasing -fwrapv -funsigned-char -msse2 -mfpmath=sse
    "SHELL:-mllvm -fixup-byte-word-insts=false" "SHELL:-include ${SRC}/vpc/vpc.h")
# low addresses stay free for the guest image (vpc_image_alloc); the main thread runs the game (4 MB on the Vita)
target_link_options(usm_vpc PRIVATE -static -Wl,--image-base,0x10000000 -Wl,--stack,0x800000)
include(${CMAKE_CURRENT_SOURCE_DIR}/../vendor/ffmpeg/ffmpeg.cmake)
target_link_libraries(usm_vpc PRIVATE ffbink opengl32 gdi32 winmm pthread)
