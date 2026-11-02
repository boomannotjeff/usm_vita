/* FFmpeg 9.0.2 configured for the Vita, Bink decoders only (config_gen.h, from configure --disable-everything
   --disable-asm --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-small); generic C, no arch code.
   The same sources build for vpc (i686 mingw): what the Windows C library lacks is switched here. */
#include "config_gen.h"
#ifdef _WIN32
#undef HAVE_MEMALIGN
#define HAVE_MEMALIGN 0
#undef HAVE_ALIGNED_MALLOC
#define HAVE_ALIGNED_MALLOC 1
#undef HAVE_SYSCONF
#define HAVE_SYSCONF 0
#undef HAVE_ISATTY
#define HAVE_ISATTY 0
#undef HAVE_IO_H
#define HAVE_IO_H 1
#undef HAVE_DOS_PATHS
#define HAVE_DOS_PATHS 1
#undef HAVE_FCNTL
#define HAVE_FCNTL 0
#undef HAVE_GMTIME_R
#define HAVE_GMTIME_R 0
#undef HAVE_LOCALTIME_R
#define HAVE_LOCALTIME_R 0
#undef HAVE_MKSTEMP
#define HAVE_MKSTEMP 0
#undef HAVE_GETRUSAGE
#define HAVE_GETRUSAGE 0
#undef HAVE_STRERROR_R
#define HAVE_STRERROR_R 0
#endif
