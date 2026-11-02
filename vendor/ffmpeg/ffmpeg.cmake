# FFmpeg's Bink video/audio decoders (LGPL 2.1, see COPYING.LGPLv2.1) as a static library: target ffbink.
# The file list is configure's (sources.txt); the config is in config.h.
set(FF_DIR ${CMAKE_CURRENT_LIST_DIR})
file(STRINGS ${FF_DIR}/sources.txt FF_SOURCES)
list(TRANSFORM FF_SOURCES PREPEND ${FF_DIR}/)
add_library(ffbink STATIC ${FF_SOURCES})
target_include_directories(ffbink PUBLIC ${FF_DIR})
target_compile_definitions(ffbink PRIVATE HAVE_AV_CONFIG_H _ISOC11_SOURCE _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
target_compile_options(ffbink PRIVATE -O2 -std=gnu17 -w -fno-strict-aliasing -fno-math-errno -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types)
