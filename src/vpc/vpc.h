/* usm_vpc.exe: the Vita build's code (src/vita + the translated game) on the PC, for debugging the renderer without the
   device. Force-included into every source file (-include vpc.h); gives the newlib/POSIX behaviour the Vita code
   relies on. The sce and vitaGL stand-ins are in psp2/ and vitaGL.h next to this file; vpc.c implements them. */
#pragma once
/* the CRT declares these, newlib doesn't: shims_crt.c uses the names for its own shims, so the CRT headers below
   declare them under other names */
#define __p__fmode vpc_hidden___p__fmode
#define _aligned_free vpc_hidden__aligned_free
#define _aligned_malloc vpc_hidden__aligned_malloc
#define _getcwd vpc_hidden__getcwd
#define _msize vpc_hidden__msize
#define _onexit vpc_hidden__onexit
#define _strcmpi vpc_hidden__strcmpi
#define _strdup vpc_hidden__strdup
#define _strlwr vpc_hidden__strlwr
#define _strnicmp vpc_hidden__strnicmp
#define _strupr vpc_hidden__strupr
#include <direct.h>
#include <io.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#undef __p__fmode
#undef _aligned_free
#undef _aligned_malloc
#undef _getcwd
#undef _msize
#undef _onexit
#undef _strcmpi
#undef _strdup
#undef _strlwr
#undef _strnicmp
#undef _strupr

/* newlib lets memalign blocks go to free/realloc/malloc_usable_size; the CRT's don't, so every allocation in our code
   goes through one allocator with a header (vpc.c) */
void* vpc_memalign(size_t align, size_t n);
void* vpc_malloc(size_t n);
void* vpc_calloc(size_t n, size_t m);
void* vpc_realloc(void* p, size_t n);
void vpc_free(void* p);
size_t vpc_usable(void* p);
char* vpc_strdup(const char* s);
struct mallinfo { size_t uordblks; };
struct mallinfo vpc_mallinfo(void);
#define memalign vpc_memalign
#define malloc vpc_malloc
#define calloc vpc_calloc
#define realloc vpc_realloc
#define free vpc_free
#define strdup vpc_strdup
#define malloc_usable_size vpc_usable
#define mallinfo vpc_mallinfo

#define mkdir(p, mode) _mkdir(p)
ssize_t pread(int fd, void* buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off);

/* the guest image at host 0x410000: rc_guest = 0x10000, so every host address is a positive guest address and none
   can reach the shim range (SHIM_BASE) */
void* vpc_image_alloc(size_t n);
