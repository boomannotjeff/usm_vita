/* COM objects the game sees (D3D9, DirectSound, DirectInput): guest memory whose first word points to a guest vtable
   of shim slots. Methods given a C function run it; the rest are survey stubs that return S_OK (0). */
#pragma once
#include "vita.h"

typedef struct { const char* name; int pops; } ComMethod;
typedef struct { const char* name; const ComMethod* m; int n; } ComIface;
extern const ComIface g_ifaces[];  /* com_gen.c (tools/recomp/gen_com.py) */

/* The vtable of an interface, built once: impl lists the methods that have C code. AddRef/Release/QueryInterface
   default to a reference count that never frees. */
uint32_t com_vtable(const char* iface, const ShimDef* impl);
uint32_t com_vtable_ext(const char* iface, const ShimDef* impl, const ShimDef* more);  /* `more` overrides `impl` */
/* a new object: `size` bytes of guest memory (at least 8), vtable in word 0, reference count in word 1 */
uint32_t com_new(uint32_t vtable, uint32_t size);
