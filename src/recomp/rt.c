/* Recompiled-code runtime: the bridge between native (original) code and translated functions, and x87 helpers.
   See rt.h. Only the PC in-process build lives here for now. */
#include "rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>
#include <intrin.h>

static DWORD g_tls = TLS_OUT_OF_INDEXES;


/* x86 SEH assumes exception records sit at lower addresses the deeper they are, within the TEB stack limits.
   A fault in translated code is dispatched on the host stack, so the host stack is carved from the bottom of
   this thread's own stack reservation: the game keeps the top kGuest bytes (it was built for 64 KB stacks),
   the whole reservation is committed (no guard-page growth, so StackLimit stays put), and StackLimit is set to
   the bottom. Threads created later get bigger stacks through the patched image header (rc_init). */
#define GUEST_STACK 0x10000u
#define THREAD_STACK 0x100000u


/* Which exceptions the game raises in normal play (C++ throws are 0xE06D7363): each code/address pair once. */
static LONG CALLBACK note_exception(EXCEPTION_POINTERS* e) {
    static volatile LONG n;
    static uint64_t seen[256];
    uint64_t key = (uint64_t)e->ExceptionRecord->ExceptionCode << 32 | (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    for (LONG i = 0; i < n && i < 256; i++)
        if (seen[i] == key) return EXCEPTION_CONTINUE_SEARCH;
    LONG i = InterlockedIncrement(&n) - 1;
    if (i < 256) {
        seen[i] = key;
        rc_log("exception %08lx at %08x\n", e->ExceptionRecord->ExceptionCode, (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((constructor)) static void rc_init(void) {
    g_tls = TlsAlloc();
    AddVectoredExceptionHandler(0, note_exception);
    /* CreateThread takes the default reserve from the in-memory exe header */
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)GetModuleHandleA(NULL) + ((IMAGE_DOS_HEADER*)GetModuleHandleA(NULL))->e_lfanew);
    DWORD old;
    if (getenv("RC_NO_HDR") == NULL && nt->OptionalHeader.SizeOfStackReserve < THREAD_STACK &&
        VirtualProtect(&nt->OptionalHeader.SizeOfStackReserve, 4, PAGE_READWRITE, &old)) {
        nt->OptionalHeader.SizeOfStackReserve = THREAD_STACK;
        VirtualProtect(&nt->OptionalHeader.SizeOfStackReserve, 4, old, &old);
    }
}



/* Called from the asm bridge right after native code ran, so it must not change the last-error value. */
Ctx* rc_ctx(void) {
    Ctx* c = g_tls < 64 ? (Ctx*)__readfsdword(0xE10 + g_tls * 4) : (Ctx*)TlsGetValue(g_tls);
    if (c) return c;
    DWORD err = GetLastError();
    c = (Ctx*)VirtualAlloc(NULL, sizeof(Ctx), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    rc_tls_init(c);
    uint32_t lo = __readfsdword(0xE0C), hi = __readfsdword(4), sp;
    __asm__ volatile("mov %%esp, %0" : "=r"(sp));
    uint32_t split = hi - (hi - lo) / 2;
    DWORD old;
    if (getenv("RC_NO_SPLIT") == NULL && split - lo >= 0x8000 && sp > split &&
        VirtualAlloc((void*)(uintptr_t)(lo + 4096), hi - lo - 4096, MEM_COMMIT, PAGE_READWRITE) &&
        VirtualProtect((void*)(uintptr_t)(lo + 4096), hi - lo - 4096, PAGE_READWRITE, &old)) {  /* drops PAGE_GUARD */
        __writefsdword(8, lo + 4096);  /* TEB StackLimit */
        c->host_lo = lo + 4096;
        /* a no-access page between the halves: a game stack that outgrows its half faults here, visibly */
        VirtualProtect((void*)(uintptr_t)(split - 4096), 4096, PAGE_NOACCESS, &old);
        c->host_sp = split - 4096 - 64;
    } else {  /* thread stack too small: a separate host stack, and SEH unwinds across translated code break */
        uint8_t* m = (uint8_t*)VirtualAlloc(NULL, 0x40000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        c->host_lo = (uint32_t)(uintptr_t)m;
        c->host_sp = c->host_lo + 0x40000 - 16;
    }
    rc_log("thread %lu: stack %08x-%08x, host stack %08x-%08x\n", GetCurrentThreadId(), lo, hi, c->host_lo, c->host_sp);
    TlsSetValue(g_tls, c);
    SetLastError(err);
    return c;
}

void rc_log(const char* fmt, ...) {
    static char path[MAX_PATH];
    if (!path[0]) {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char* s = strrchr(path, '\\');
        strcpy(s ? s + 1 : path, "usm_rc.log");
    }
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ---- x87 state across the native boundary: the soft stack <-> the real FPU stack ---- */
static void fpu_to_real(Ctx* c) {
    for (int i = (int)c->fdepth - 1; i >= 0; i--) {
        uint8_t b[10];
#ifdef RC_PORTABLE
        rc_st80(b, ST(i));
#else
        memcpy(b, &ST(i), 10);
#endif
        __asm__ volatile("fldt %0" : : "m"(b));
    }
    c->ftop = (c->ftop + c->fdepth) & 7;
    c->fdepth = 0;
}

static void fpu_from_real(Ctx* c) {
    uint16_t sw;
    __asm__ volatile("fnstsw %0" : "=m"(sw));
    unsigned n = (8 - ((sw >> 11) & 7)) & 7;
    uint8_t v[8][10];
    for (unsigned i = 0; i < n; i++) __asm__ volatile("fstpt %0" : "=m"(v[i]));
    while (n--) {
        c->ftop = (c->ftop - 1) & 7;
        c->fdepth++;
#ifdef RC_PORTABLE
        ST(0) = rc_ld80(v[n]);
#else
        memcpy(&ST(0), v[n], 10);
#endif
    }
}

/* pushal block layout: edi esi ebp esp ebx edx ecx eax */
static void take_regs(Ctx* c, const uint32_t* b) {
    c->edi = b[0], c->esi = b[1], c->ebp = b[2], c->ebx = b[4], c->edx = b[5], c->ecx = b[6], c->eax = b[7];
}

/* entry from native: b[8] = translated function, b[9] = the native caller's return address */
Ctx* rc_enter_c(uint32_t* b) {
    Ctx* c = rc_ctx();
    take_regs(c, b);
    c->esp = (uint32_t)(uintptr_t)&b[9];
    c->target = b[8];
    while (c->nout && c->esp > c->out[c->nout - 1].gsp) c->host_sp = c->out[--c->nout].hsp;  /* see Ctx.out */
    while (c->nent && c->esp > c->ent[c->nent - 1].esp) c->nent--;                               /* unwound too */
    if (c->nent < 256) {
        c->ent[c->nent].fn = c->target, c->ent[c->nent].ebx = c->ebx, c->ent[c->nent].esi = c->esi;
        c->ent[c->nent].edi = c->edi, c->ent[c->nent].ebp = c->ebp, c->ent[c->nent].esp = c->esp;
    }
    c->nent++;
    c->fsbase = __readfsdword(0x18);
#ifdef RC_PORTABLE
    uint16_t cw;  /* native code may have changed the rounding mode: carry it over to the doubles' FPU */
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    if (cw != c->fcw) rc_fldcw(c, cw);
#else
    __asm__ volatile("fnstcw %0" : "=m"(c->fcw));
#endif
    fpu_from_real(c);
    return c;
}

/* Translated functions entered from native code must hand back the callee-saved registers. Log the first few
   that don't, by guest address: a lead when native code crashes on a bad ebx/esi/edi/ebp. */
void rc_leave_c(Ctx* c) {
    fpu_to_real(c);
    if (!c->nent) return;
    unsigned k = --c->nent;
    if (k >= 256) return;
    static volatile long reported;
    if ((c->ebx != c->ent[k].ebx || c->esi != c->ent[k].esi || c->edi != c->ent[k].edi || c->ebp != c->ent[k].ebp) && reported < 20) {
        uint32_t f = c->ent[k].fn;
        for (unsigned i = 0; i < rc_count; i++) if ((uint32_t)(uintptr_t)rc_impl[i] == f) { f = rc_addr[i]; break; }
        InterlockedIncrement(&reported);
        rc_log("callee-saved changed by %06x: ebx %08x->%08x esi %08x->%08x edi %08x->%08x ebp %08x->%08x\n", f,
               c->ent[k].ebx, c->ebx, c->ent[k].esi, c->esi, c->ent[k].edi, c->edi, c->ent[k].ebp, c->ebp);
    }
}

/* Back from native code through resume stub k (b[8] = k): out-call k is the one that returned. Any deeper
   out-calls were unwound by native code (a C++ throw caught further up) and are dropped with their host frames.
   Returns the host stack frame of out-call k to switch back to. */
uint32_t rc_resume_c(Ctx* c, uint32_t* b) {
    unsigned k = b[8];
    take_regs(c, b);
    c->esp = (uint32_t)(uintptr_t)&b[9];
    c->host_sp = c->out[k].hsp;
    c->nout = k;
    return c->out[k].frame;
}

void rc_native_raw(Ctx* c, uint32_t target, uint32_t resume, uint32_t* frame);
extern const uint8_t rc_resume_stubs[];  /* 10 bytes each: push k; jmp _rc_resume */

/* Each hooked function's stub is `push f_XXXXXX; jmp rc_enter` (made by the generated table). */
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _rc_enter\n"
    "_rc_enter:\n"
    "  pushal\n"
    "  push esp\n"
    "  call _rc_enter_c\n"
    "  mov esp, [eax+32]\n"     /* host stack */
    "  push eax\n"
    "  push eax\n"
    "  call [eax+36]\n"         /* f(c) */
    "  call _rc_leave_c\n"      /* arg still on the stack */
    "  add esp, 4\n"
    "  pop eax\n"
    "  mov esp, [eax+16]\n"     /* guest esp after the translated ret */
    "  push dword ptr [eax+36]\n"
    "  mov ecx, [eax+4]\n  mov edx, [eax+8]\n  mov ebx, [eax+12]\n"
    "  mov ebp, [eax+20]\n  mov esi, [eax+24]\n  mov edi, [eax+28]\n  mov eax, [eax]\n"
    "  ret\n"

    ".globl _rc_native_raw\n"
    "_rc_native_raw:\n"
    "  push ebp\n  push ebx\n  push esi\n  push edi\n"
    "  mov eax, [esp+20]\n"
    "  mov ecx, [esp+24]\n"
    "  mov edx, [esp+32]\n"
    "  push dword ptr [eax+32]\n"
    "  mov [eax+32], esp\n"        /* nested entries go below this frame */
    "  mov [edx], esp\n"           /* where its resume stub comes back to */
    "  mov [eax+36], ecx\n"
    "  mov ecx, [esp+32]\n"        /* resume stub (args moved by the push) */
    "  mov esp, [eax+16]\n"
    "  push ecx\n"
    "  push dword ptr [eax+36]\n"
    "  mov ecx, [eax+4]\n  mov edx, [eax+8]\n  mov ebx, [eax+12]\n"
    "  mov ebp, [eax+20]\n  mov esi, [eax+24]\n  mov edi, [eax+28]\n  mov eax, [eax]\n"
    "  ret\n"

    ".globl _rc_resume_stubs\n"
    "_rc_resume_stubs:\n"
    "  .set rc_k, 0\n"
    "  .rept 256\n"
    "  .byte 0x68\n  .long rc_k\n"                       /* push imm32 k */
    "  .byte 0xE9\n  .long _rc_resume - (. + 4)\n"       /* jmp rel32 */
    "  .set rc_k, rc_k + 1\n"
    "  .endr\n"
    "_rc_resume:\n"
    "  pushal\n"
    "  call _rc_ctx\n"
    "  mov ecx, esp\n"
    "  push ecx\n"
    "  push eax\n"
    "  call _rc_resume_c\n"
    "  mov esp, eax\n"
    "  add esp, 4\n"               /* saved host_sp: rc_resume_c restored it */
    "  pop edi\n  pop esi\n  pop ebx\n  pop ebp\n"
    "  ret\n"
    ".att_syntax\n");

/* Inventory of what translated code still calls natively, each target logged once: outside USM.exe that is the
   shim surface a standalone/Vita build must provide (module!export, or module+offset for COM methods); inside
   USM.exe it is code nothing translated yet. */
static const char* export_name(HMODULE m, uint32_t a) {
    uint8_t* b = (uint8_t*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress) return NULL;
    IMAGE_EXPORT_DIRECTORY* e = (IMAGE_EXPORT_DIRECTORY*)(b + d.VirtualAddress);
    uint32_t* fn = (uint32_t*)(b + e->AddressOfFunctions);
    uint32_t* nm = (uint32_t*)(b + e->AddressOfNames);
    uint16_t* ord = (uint16_t*)(b + e->AddressOfNameOrdinals);
    for (uint32_t i = 0; i < e->NumberOfNames; i++)
        if ((uint32_t)(uintptr_t)b + fn[ord[i]] == a) return (const char*)(b + nm[i]);
    return NULL;
}

/* name of a native call target: module!export, module+offset (COM methods), or exe address */
static void native_name(uint32_t target, char* out, size_t n) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)target, &m);
    if (m) GetModuleFileNameA(m, path, MAX_PATH);
    const char* mod = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    const char* x = m ? export_name(m, target) : NULL;
    if (m == GetModuleHandleA(NULL)) snprintf(out, n, "exe %06x", target);
    else if (x) snprintf(out, n, "%s!%s", mod, x);
    else if (m) snprintf(out, n, "%s+%x", mod, target - (uint32_t)(uintptr_t)m);
    else snprintf(out, n, "unknown %08x", target);
}

static volatile LONG native_seen[8192], native_calls[8192];

static void note_native(uint32_t target) {
    uint32_t h = (target * 2654435761u) >> 19;
    for (int n = 0; n < 8192; n++, h = (h + 1) & 8191) {
        LONG v = native_seen[h];
        if ((uint32_t)v == target) { native_calls[h]++; return; }
        if (v) continue;
        if (InterlockedCompareExchange(&native_seen[h], (LONG)target, 0) != 0) { h = (h - 1) & 8191; continue; }  /* lost the race: recheck this slot */
        DWORD err = GetLastError();
        char name[300];
        native_name(target, name, sizeof name);
        rc_log("native %s %s\n", strncmp(name, "exe ", 4) ? "import" : "exe", strncmp(name, "exe ", 4) ? name : name + 4);
        native_calls[h] = 1;
        SetLastError(err);
        return;
    }
}

/* the most called native targets so far, for the profiler (debug builds only) */
void rc_dump_native(FILE* f, unsigned top) {
    for (unsigned k = 0; k < top; k++) {
        int best = -1;
        for (int h = 0; h < 8192; h++)
            if (native_seen[h] && native_calls[h] > 0 && (best < 0 || native_calls[h] > native_calls[best])) best = h;
        if (best < 0) return;
        char name[300];
        native_name((uint32_t)native_seen[best], name, sizeof name);
        fprintf(f, "  %10ld  %s\n", native_calls[best], name);
        native_calls[best] = -native_calls[best];  /* taken; restored below */
    }
    for (int h = 0; h < 8192; h++) if (native_calls[h] < 0) native_calls[h] = -native_calls[h];
}

void rc_native(Ctx* c, uint32_t target) {
#ifndef RC_RELEASE
    note_native(target);
#endif
    unsigned n = c->nout;
    if (n >= sizeof c->out / sizeof *c->out) rc_trap(c, target, "native calls nested too deep");
    fpu_to_real(c);
    c->out[n].gsp = c->esp, c->out[n].hsp = c->host_sp;
    c->nout = n + 1;
    rc_native_raw(c, target, (uint32_t)(uintptr_t)(rc_resume_stubs + 10 * n), &c->out[n].frame);
    c->nout = n;
    fpu_from_real(c);
}

/* Calls through pointers (vtables) are hot: a direct-mapped cache in front of the binary search. Entries are single
   32-bit words so threads never see half of one (a 64-bit atomic is a locked cmpxchg8b on x86): the table index
   + 1 of a translated function, checked against rc_addr, or the address itself for original code.
   rc_on only changes before the game runs, so cached answers stay right. */
static uint32_t cache_idx[8192], cache_native[8192];

static unsigned index_slow(uint32_t a) {
    unsigned lo = 0, hi = rc_count;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (rc_addr[m] < a) lo = m + 1;
        else hi = m;
    }
#ifdef RC_RELEASE
    return lo < rc_count && rc_addr[lo] == a ? lo + 1 : 0;
#else
    return lo < rc_count && rc_addr[lo] == a && rc_on[lo] ? lo + 1 : 0;
#endif
}

static rc_fn lookup(uint32_t a) {
    unsigned h = (a >> 4 ^ a >> 17) & 8191;
    uint32_t k = __atomic_load_n(&cache_idx[h], __ATOMIC_RELAXED);
    if (k && rc_addr[k - 1] == a) return rc_impl[k - 1];
    if (__atomic_load_n(&cache_native[h], __ATOMIC_RELAXED) == a && a) return NULL;
    k = index_slow(a);
    if (k) __atomic_store_n(&cache_idx[h], k, __ATOMIC_RELAXED);
    else __atomic_store_n(&cache_native[h], a, __ATOMIC_RELAXED);
    return k ? rc_impl[k - 1] : NULL;
}

void rc_call(Ctx* c, uint32_t target, uint32_t ret) {
    rc_fn f = lookup(target);
    if (!f) { rc_native(c, target); return; }
    c->esp -= 4;
    ST32(c->esp, ret);
    f(c);
#ifndef RC_RELEASE
    if (c->target != ret) rc_bad_return(c, target, ret);
#endif
}

void rc_tail(Ctx* c, uint32_t target) {
    rc_fn f = lookup(target);
    if (f) { f(c); return; }
    uint32_t ra = LD32(c->esp);
    c->esp += 4;
    rc_native(c, target);
    c->target = ra;
}

/* An indirect jmp that is not a switch case is a tail call ([esp] is the return address). Targets that Ghidra
   does not know as a function are code only reachable through a pointer: they run as original code here, and
   are logged (once each) because a standalone build has to translate them too. */
void rc_jmp(Ctx* c, uint32_t target, uint32_t at) {
    unsigned lo = 0, hi = rc_nentries;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (rc_entries[m] < target) lo = m + 1;
        else hi = m;
    }
    int inside = target >= rc_entries[0] && target <= rc_entries[rc_nentries - 1] + 0x10000;
    if (inside && !(lo < rc_nentries && rc_entries[lo] == target)) {
        static uint32_t seen[64];
        static volatile long nseen;
        int known = 0;
        for (long i = 0; i < nseen && i < 64; i++) known |= seen[i] == target;
        if (!known && nseen < 64) seen[InterlockedIncrement(&nseen) - 1] = target, rc_log("missed function %08x (jumped to from %06x)\n", target, at);
    }
    rc_tail(c, target);
}

void (*rc_on_trap)(uint32_t at);

void rc_bad_return(Ctx* c, uint32_t at, uint32_t expected) {
    rc_log("bad return: call to %06x came back to %08x, expected %08x\n", at, c->target, expected);
    if (rc_on_trap) rc_on_trap(at);
    __builtin_trap();
}

void rc_trap(Ctx* c, uint32_t at, const char* why) {
    rc_log("trap at %06x: %s\n", at, why);
    if (rc_on_trap) rc_on_trap(at);
    __builtin_trap();
}

#ifndef RC_PORTABLE
/* ---- x87 helpers: the real instructions, so results match the original bit for bit ---- */
void rc_fldcw(Ctx* c, uint16_t cw) {
    c->fcw = cw;
    __asm__ volatile("fldcw %0" : : "m"(cw));
}
int16_t rc_fist16(f80 v) { int16_t r; __asm__("fistps %0" : "=m"(r) : "t"(v) : "st"); return r; }
int32_t rc_fist32(f80 v) { int32_t r; __asm__("fistpl %0" : "=m"(r) : "t"(v) : "st"); return r; }
int64_t rc_fist64(f80 v) { int64_t r; __asm__("fistpll %0" : "=m"(r) : "t"(v) : "st"); return r; }
f80 rc_frndint(f80 v) { __asm__("frndint" : "+t"(v)); return v; }
f80 rc_fsin(f80 v) { __asm__("fsin" : "+t"(v)); return v; }
f80 rc_fcos(f80 v) { __asm__("fcos" : "+t"(v)); return v; }
f80 rc_fsqrt(f80 v) { __asm__("fsqrt" : "+t"(v)); return v; }
f80 rc_fptan(f80 v) { __asm__("fptan\n\tfstp %%st(0)" : "+t"(v)); return v; }
f80 rc_fpatan(f80 y, f80 x) { __asm__("fpatan" : "+t"(x) : "u"(y) : "st(1)"); return x; }
f80 rc_fyl2x(f80 y, f80 x) { __asm__("fyl2x" : "+t"(x) : "u"(y) : "st(1)"); return x; }
f80 rc_fconst(int which) {
    f80 v;
    switch (which) {
    case 0: __asm__("fld1" : "=t"(v)); break;
    case 1: __asm__("fldz" : "=t"(v)); break;
    case 2: __asm__("fldpi" : "=t"(v)); break;
    case 3: __asm__("fldl2e" : "=t"(v)); break;
    case 4: __asm__("fldl2t" : "=t"(v)); break;
    case 5: __asm__("fldlg2" : "=t"(v)); break;
    default: __asm__("fldln2" : "=t"(v)); break;
    }
    return v;
}

/* cpuid as a CPU without MMX, SSE or 3DNow!: the game then never chooses its SIMD code paths, which the
   translator leaves out (they would need a SIMD emulation, and the Vita has none of these). */
void rc_cpuid(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    uint32_t leaf = *a, sub = *c;
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
    if (leaf == 1) {
        *d &= ~((1u << 23) | (1u << 25) | (1u << 26));                                  /* MMX, SSE, SSE2 */
        *c &= ~((1u << 0) | (1u << 9) | (1u << 19) | (1u << 20) | (1u << 28));          /* SSE3..4.2, AVX */
    } else if (leaf == 0x80000001) {
        *d &= ~((1u << 22) | (1u << 23) | (1u << 30) | (1u << 31));                    /* MMX ext, MMX, 3DNow! ext, 3DNow! */
    }
}

#endif

/* ---- setjmp/longjmp (see rt.h) ---- */
#define NJMP 32

rc_jmprec* rc_setjmp_rec(Ctx* c, uint32_t jb) {
    if (!c->jmp) c->jmp = (rc_jmprec*)VirtualAlloc(NULL, sizeof(rc_jmprec) * NJMP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    rc_jmprec* free_ = NULL;
    for (int i = 0; i < NJMP; i++) {
        if (c->jmp[i].jb == jb) return &c->jmp[i];
        if (!c->jmp[i].jb && !free_) free_ = &c->jmp[i];
    }
    if (!free_) free_ = &c->jmp[jb % NJMP];  /* full: reuse one (a stale buffer nobody longjmps to any more) */
    free_->jb = jb;
    return free_;
}

void rc_setjmp_save(Ctx* c, rc_jmprec* r) {
    memcpy(r->regs, &c->eax, 32);
    r->host_sp = c->host_sp, r->nout = c->nout, r->nent = c->nent;
    r->fs0 = __readfsdword(0);
}

int rc_longjmp(uint32_t jb, int v) {
    Ctx* c = rc_ctx();
    if (!c->jmp) return 0;
    for (int i = 0; i < NJMP; i++) {
        rc_jmprec* r = &c->jmp[i];
        if (r->jb != jb) continue;
        memcpy(&c->eax, r->regs, 32);
        c->eax = v ? (uint32_t)v : 1;
        c->host_sp = r->host_sp, c->nout = r->nout, c->nent = r->nent;
        c->ftop = 0, c->fdepth = 0;
        __writefsdword(0, r->fs0);  /* the SEH frames registered after setjmp are gone with their frames */
        __builtin_longjmp(r->buf, 1);
    }
    return 0;
}
