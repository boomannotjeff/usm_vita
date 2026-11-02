/* Shim slots: each import and COM method is a fake guest address SHIM_BASE + 4 * slot. A slot without a C function
   is survey mode: the first call logs its name, then it returns 0 with the right stdcall pop, so one run lists
   every shim the game still needs. */
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "vita.h"

#define MAX_SLOTS 8192

typedef struct {
    const char* name;
    shim_fn fn;
    int pops;
    uint8_t noted, quiet, kind;
#ifdef RC_PROFILE
    uint32_t calls, same;  /* the scan: calls, and calls that set what was already set (shim_same) */
#endif
} Slot;
/* kind, from the name once (the pipeline checks it on every call): */
enum { K_D3D = 1, K_DRAW = 2, K_SETSTREAM = 4 };

/* C runtime helpers called thousands of times (ctype, strings, memory, math): left out of the call trace */
static int quiet_name(const char* dll, const char* n) {
    if (!strncasecmp(dll, "winmm", 5)) return !strncmp(n, "time", 4);  /* timeGetTime, timeBeginPeriod */
    if (strncasecmp(dll, "msvcr", 5)) return 0;
    static const char* pre[] = {"is", "to", "str", "_str", "mem", "_CI", "_ftol", "rand", "sqrt", "floor", "ceil", "fabs",
                                "atoi", "atof", "_ismbb", "malloc", "free", "_aligned", "??", "_msize", "realloc", "calloc",
                                "qsort"};
    for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++)
        if (!strncmp(n, pre[i], strlen(pre[i]))) return 1;
    return 0;
}
static Slot g_slot[MAX_SLOTS];
static unsigned g_nslots;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* per-DLL tables, defined next to their shims */
extern const ShimDef shims_kernel32[], shims_msvcr71[], shims_user32[], shims_d3d9[], shims_dsound[], shims_dinput8[], shims_bink[], shims_misc[];
static const struct { const char* dll; const ShimDef* t; } g_tables[] = {
    {"kernel32.dll", shims_kernel32},
    {"msvcr71.dll", shims_msvcr71},
    {"user32.dll", shims_user32},
    {"d3d9.dll", shims_d3d9},
    {"dsound.dll", shims_dsound},
    {"dinput8.dll", shims_dinput8},
    {"binkw32.dll", shims_bink},
    {NULL, shims_misc},  /* any DLL: advapi32, winmm, ole32, d3d9, dsound, dinput8 ... */
};

uint32_t shim_new(const char* name, shim_fn fn, int pops) {
    pthread_mutex_lock(&g_lock);
    unsigned k = g_nslots < MAX_SLOTS ? g_nslots++ : MAX_SLOTS - 1;
    g_slot[k] = (Slot){name, fn, pops, 0};
    if (!strncmp(name, "IDirect3D", 9)) g_slot[k].kind = K_D3D;
    if (!strncmp(name, "IDirect3DDevice9::Draw", 22)) g_slot[k].kind |= K_DRAW;
    if (!strcmp(name, "IDirect3DDevice9::SetStreamSource")) g_slot[k].kind |= K_SETSTREAM;
    pthread_mutex_unlock(&g_lock);
    return SHIM_BASE + 4 * k;
}

uint32_t shim_addr(const char* dll, const char* name, int pops) {
    shim_fn fn = NULL;
    for (unsigned i = 0; i < sizeof g_tables / sizeof *g_tables && !fn; i++) {
        if (g_tables[i].dll && strcasecmp(g_tables[i].dll, dll)) continue;
        for (const ShimDef* d = g_tables[i].t; d->name; d++)
            if (!strcmp(d->name, name)) { fn = d->fn; break; }
    }
    char* full = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(full, "%s!%s", dll, name);
    uint32_t a = shim_new(full, fn, pops);
    g_slot[(a - SHIM_BASE) / 4].quiet = (uint8_t)quiet_name(dll, name);
    return a;
}

/* the last 64 shims called (slot numbers, newest at g_trace_n - 1): in .bss, so a crash dump shows them */
volatile uint16_t g_trace[64];
volatile uint32_t g_trace_n;
int g_shim_verbose;  /* every shim call to the log: on when ux0:data/usm/trace.txt exists (main.c) */

#ifdef RC_PROFILE
static Slot* volatile g_cur;  /* the shim running (racy with the pipeline's worker: counts only) */
void shim_same(void) { if (g_cur) g_cur->same++; }
static int by_calls(const void* a, const void* b) {
    uint32_t x = g_slot[*(const uint16_t*)a].calls, y = g_slot[*(const uint16_t*)b].calls;
    return x < y ? 1 : x > y ? -1 : 0;
}
/* the scan (profiler thread, every 10 s): the most called shims per frame and how many of those calls changed nothing */
void shim_dump(uint32_t frames) {
    static uint16_t ix[MAX_SLOTS];
    unsigned n = 0;
    for (unsigned i = 0; i < g_nslots; i++)
        if (g_slot[i].calls) ix[n++] = (uint16_t)i;
    qsort(ix, n, sizeof *ix, by_calls);
    if (!frames) frames = 1;
    vlog("shim calls over %u frames (per frame, %% that set what was already set):\n", frames);
    for (unsigned i = 0; i < n && i < 40; i++) {
        Slot* s = &g_slot[ix[i]];
        vlog("  %8.1f %5.1f%%  %s\n", (double)s->calls / frames, s->calls ? 100.0 * s->same / s->calls : 0.0, s->name);
    }
    for (unsigned i = 0; i < g_nslots; i++) g_slot[i].calls = g_slot[i].same = 0;
}
#endif

int shim_call(Ctx* c, uint32_t target) {
    if (target < SHIM_BASE || target >= SHIM_BASE + 4 * g_nslots || target & 3) return 0;
    Slot* s = &g_slot[(target - SHIM_BASE) / 4];
    g_trace[g_trace_n++ & 63] = (uint16_t)((target - SHIM_BASE) / 4);
#ifdef RC_WTRACE
    if (rc_wt_on) rc_wt_shim(s->name);
#endif
    if (g_shim_verbose && !s->quiet)
        vlog("%x> %s(%08x, %08x, %08x, %08x)\n", sceKernelGetThreadId() & 0xfff, s->name, ARG(0), ARG(1), ARG(2), ARG(3));
#ifndef RC_WTRACE
    /* render pipeline (native.c): D3D is single-threaded, so a D3D call waits while the worker draws */
    extern volatile int g_wbusy;
    void pipe_fence_d3d(Ctx* c, const char* name);
    if (g_wbusy && s->kind) {
        /* except the world frame tail's unbind of stream 0, which the worker does itself (d3d9_null.c skips main's) */
        if (!(s->kind & K_SETSTREAM && ARG(1) == 0 && ARG(2) == 0)) pipe_fence_d3d(c, s->name);
        void pipe_draw_delay(Ctx* c);
        if (s->kind & K_DRAW) pipe_draw_delay(c);
    }
#endif
#ifdef RC_PROFILE
    s->calls++, g_cur = s;
#endif
    if (s->fn) {
        s->fn(c);
    } else {
        if (!s->noted) s->noted = 1, vlog("missing shim %s (returns 0)\n", s->name);
        c->eax = 0;
    }
    if (s->pops > 0) c->esp += s->pops;
    return 1;
}

void shim_log_recent(int n) {
    uint32_t end = g_trace_n;
    vlog("  last shims (oldest first):");
    for (uint32_t i = end > (uint32_t)n ? end - n : 0; i < end; i++) vlog(" %s", g_slot[g_trace[i & 63]].name);
    vlog("\n");
}
