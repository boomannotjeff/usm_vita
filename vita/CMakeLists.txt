# Vita build: the translated game code + the Vita runtime and shims (src/vita) -> usm.vpk.
# The game files are not in the VPK: the user copies USM.exe and Data\ from their PC install to ux0:data/usm/.
#   cmake -S vita -B build-vita -G Ninja && cmake --build build-vita
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "")
endif()
project(usm_vita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Ultimate Spider-Man")
set(VITA_TITLEID "USMR00001")
set(VITA_VERSION "01.00")

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
file(GLOB RECOMP_GEN CONFIGURE_DEPENDS ${SRC}/recomp/gen/recomp_*.c)
if(NOT RECOMP_GEN)
    message(FATAL_ERROR "src/recomp/gen is empty: run  py tools/recomp/recomp.py src/recomp/gen all")
endif()
file(GLOB VITA_SRC CONFIGURE_DEPENDS ${SRC}/vita/*.c)
# OFF: the null renderer (no drawing, Present logs the fps), for checking the game's CPU cost alone
option(USM_VITAGL "draw with vitaGL (needs ur0:data/libshacccg.suprx on the Vita)" ON)
if(NOT USM_VITAGL)
    list(FILTER VITA_SRC EXCLUDE REGEX "/gl\\.c$")
endif()

add_executable(usm ${RECOMP_GEN} ${SRC}/recomp/fpu_port.c ${VITA_SRC})
target_include_directories(usm PRIVATE ${SRC}/recomp ${SRC}/recomp/gen ${SRC}/vita)
# main.c logs __DATE__ __TIME__ as the log's first line: rebuild it every time so the stamp is the build's own
add_custom_target(build_stamp ALL COMMAND ${CMAKE_COMMAND} -E touch_nocreate ${SRC}/vita/main.c)
add_dependencies(usm build_stamp)
# RC_RELEASE: no hit counters or return checks; RC_PORTABLE comes from rt.h on non-x86.
# Same semantics flags as the PC build: guest arithmetic wraps, guest memory is accessed through any type.
target_compile_definitions(usm PRIVATE RC_RELEASE)
target_compile_options(usm PRIVATE -O2 -std=gnu11 -w -fno-strict-aliasing -fwrapv)
# no auto-vectorizing: it turns each call's register spill (SYNC_OUT) into integer stores to the stack reloaded by NEON
# (vldr) and stored again (vst1), a store-to-load stall on the Cortex-A9 twice per translated call
target_compile_options(usm PRIVATE -fno-tree-vectorize -fno-tree-slp-vectorize)
# ON: link-time optimization, so small translated helpers inline across files and the register spills around their
# calls (SYNC_OUT/SYNC_IN) cancel out
option(USM_LTO "link-time optimization of the translated code" OFF)
if(USM_LTO)
    target_compile_options(usm PRIVATE -flto=auto)
    target_link_options(usm PRIVATE -flto=auto -O2)
endif()
# ON: the sampling profiler (rt_vita.c) logs the game loop's hottest functions every 10 s; costs a little speed
option(USM_PROFILE "profile the translated code" OFF)
if(USM_PROFILE)
    target_compile_definitions(usm PRIVATE RC_PROFILE)
endif()
# the game's Bink movies are decoded with FFmpeg's Bink decoders (bink.c)
include(${CMAKE_CURRENT_SOURCE_DIR}/../vendor/ffmpeg/ffmpeg.cmake)
target_link_libraries(usm ffbink)
if(USM_VITAGL)
    target_compile_definitions(usm PRIVATE USM_VITAGL)
    target_link_libraries(usm vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub
        SceGxm_stub SceDisplay_stub SceSysmodule_stub SceAppMgr_stub SceCommonDialog_stub SceIme_stub SceAppUtil_stub
        stdc++)
endif()
target_link_libraries(usm pthread m SceLibKernel_stub SceKernelThreadMgr_stub SceProcessmgr_stub ScePower_stub SceCtrl_stub SceTouch_stub SceAudio_stub)

vita_create_self(eboot.bin usm UNSAFE)
vita_create_vpk(usm.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/pic0.png sce_sys/pic0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/settings.png sce_sys/livearea/contents/settings.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
