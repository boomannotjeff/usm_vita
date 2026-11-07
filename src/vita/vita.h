/* The Vita build: the translated game code (src/recomp/gen) plus a runtime and C shims for the Windows imports.
   USM.exe and Data/ come from ux0:data/usm/ (the user's own copy); the exe is only read as data.

   Guest memory. Guest addresses are 32-bit like on the PC; RC_P(a) = rc_guest + a with 32-bit wraparound, so every
   host pointer is also a guest address: G(p) below. The image goes in a block allocated before anything else, with
   rc_guest = block - 0x400000, so the game's absolute addresses land in it, and the guest heap is plain malloc
   (later allocations sit above the block: small positive guest addresses). */
#pragma once
#include <stdint.h>
#include <string.h>

#include "rt.h"

#ifndef USM_DIR  /* usm_vpc.exe: the PC game folder */
#define USM_DIR "ux0:data/usm/"
#endif

static inline uint32_t G(const void* p) { return p ? (uint32_t)((uintptr_t)p - (uintptr_t)rc_guest) : 0; }
static inline void* H(uint32_t a) { return a ? (void*)RC_P(a) : NULL; }  /* guest address -> host pointer */
/* G() for fresh allocations: warns (once) if one landed below the image, i.e. got a negative guest address */
void negative_guest(uint32_t a);
static inline uint32_t GA(const void* p) { uint32_t a = G(p); if (a >= 0x80000000u) negative_guest(a); return a; }

/* Shims. Every import (and every COM method) is a fake guest address in [SHIM_BASE, SHIM_BASE + 4 * n); calls to
   one run its C function. Arguments are on the guest stack (no return address pushed): ARG(0) is the first.
   rc_native pops `pops` bytes afterwards (stdcall). Return values go in c->eax (c->edx for 64-bit, FPUSH for st0). */
#define SHIM_BASE 0xfff00000u
#define ARG(i) LD32(c->esp + 4 * (i))
typedef void (*shim_fn)(Ctx* c);

typedef struct { const char* dll; const char* name; int pops; } ImportInfo;
extern const ImportInfo g_imports[];  /* imports_gen.c (tools/recomp/gen_imports.py) */
extern const unsigned g_nimports;

typedef struct { const char* name; shim_fn fn; } ShimDef;
uint32_t shim_addr(const char* dll, const char* name, int pops);  /* the fake address an import binds to */
uint32_t shim_new(const char* name, shim_fn fn, int pops);          /* a shim made at run time (COM methods) */
int shim_call(Ctx* c, uint32_t target);                              /* 0 if target is not a shim */
void shim_log_recent(int n);                                         /* the last n shims called, to the log */

/* host <-> guest calls: run guest code at `fn` with the given stdcall/cdecl arguments, return eax */
uint32_t guest_call(uint32_t fn, int n, const uint32_t* args);

uint32_t crt_data_import(const char* name);  /* shims_crt.c: guest address of a C runtime variable, or 0 */
void* crt_file(uint32_t f);                   /* a guest FILE* as a host FILE* (_iob's streams included) */
void apc_run(Ctx* c);                         /* shims_kernel32.c: run this thread's queued I/O completions */
void vlog(const char* fmt, ...);
void vcrash(const char* fmt, ...);  /* rt_vita.c: vlog + ux0:data/usm/crash.txt, always */
int log_on(void);  /* debug.txt there: log.txt and the speed report */
void* guest_alloc(uint32_t n);  /* zeroed, 16-aligned, never freed: TEB, stacks, COM objects */
/* the game's own allocations (malloc, new, HeapAlloc, VirtualAlloc shims) pass through these, so the speed report
   can tell the game's share of the heap from ours and vitaGL's */
extern uint32_t g_game_bytes;
void* game_mem(void* p);
void* game_realloc(void* p, uint32_t n);
void game_free(void* p);
Ctx* ctx_new(uint32_t stack_size);
