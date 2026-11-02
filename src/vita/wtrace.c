/* Hazard tracer (vpc analysis builds: -DRC_WTRACE -DRC_PROFILE), for running the render list execution (76ea10:
   sort + every item's draw call, ~28% of the Vita's game thread) on another core, one frame behind the rest of the game.
   On the game thread, every guest load/store is reported in two phases: EXEC (inside f_76ea10) and MAIN (everything
   else, i.e. game logic and render list building for the next frame). A pipelined EXEC of frame N overlaps MAIN of
   frame N+1, so the hazards are:
   - MAIN writes a word EXEC read in the previous frame (EXEC would see the new value, or a torn one);
   - MAIN reads a word EXEC wrote in the previous frame (MAIN would see the old value).
   Words EXEC both writes and reads itself, and the game thread's stack, are ignored. Every 300 frames hazards.txt
   (next to log.txt) is rewritten: kind, word address, MAIN function, EXEC function, count. Also the shims EXEC calls. */
#ifdef RC_WTRACE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"
#include "vita.h"

__thread int rc_wt_on;
static Ctx* g_c;
static uint32_t g_frame, g_stack_lo, g_stack_hi;
static int g_phase;  /* 1 MAIN, 2 EXEC */

typedef struct { uint32_t a, frame, fn; uint8_t rd, wr; } Word;  /* what EXEC did to a word, in which frame */
#define NW (1u << 21)
static Word g_w[NW];
typedef struct { uint32_t a, main_fn, exec_fn, kind, n, up1, up2, up3; } Haz;  /* up: MAIN callers */  /* kind 0: MAIN writes what EXEC read, 1: MAIN reads what EXEC wrote */
#define NHZ 65536
static Haz g_haz[NHZ];
static struct { const char* name; uint32_t n; } g_shim[256];

static uint32_t fn_at(uint32_t up) {  /* the function `up` levels above the innermost on the shadow stack */
    uint32_t d = g_c ? g_c->pdepth : 0;
    if (d > 256) d = 256;
    if (d <= up) return 0;
    uint32_t k = g_c->pstack[d - 1 - up];
    return k < rc_count ? rc_addr[k] : k;
}
static uint32_t cur_fn(void) { return fn_at(0); }
static Word* word(uint32_t a, int add) {
    for (uint32_t i = (a * 2654435761u) >> 11, n = 0; n < 256; i = (i + 1) & (NW - 1), n++) {
        Word* w = &g_w[i];
        if (w->a == a && w->frame == g_frame) return w;
        if (!w->a || w->frame != g_frame) {  /* empty, or from an earlier EXEC: reusable */
            if (!add) return NULL;
            memset(w, 0, sizeof *w), w->a = a, w->frame = g_frame;
            return w;
        }
    }
    return NULL;
}
static void hazard(uint32_t a, uint32_t main_fn, uint32_t exec_fn, uint32_t kind) {
    for (uint32_t i = ((a ^ main_fn * 31 ^ kind) * 2654435761u) >> 16, n = 0; n < NHZ; i = (i + 1) & (NHZ - 1), n++) {
        Haz* h = &g_haz[i];
        if (!h->n) h->a = a, h->main_fn = main_fn, h->exec_fn = exec_fn, h->kind = kind, h->up1 = fn_at(1), h->up2 = fn_at(2), h->up3 = fn_at(3);
        if (h->a == a && h->main_fn == main_fn && h->kind == kind) { h->n++; return; }
    }
}
static void trace_access(uint32_t a, uint32_t size, int write) {
    if (a >= g_stack_lo && a < g_stack_hi) return;
    /* the per-frame scratch arena (base 0x971f08, size 0x971f10): solved by giving each frame its own */
    uint32_t base = *(rc_u32*)RC_P(0x971f08), n = *(rc_u32*)RC_P(0x971f10);
    if (a - base < n) return;
    for (uint32_t w = a & ~3u; w < a + size; w += 4) {
        if (g_phase == 2) {
            Word* x = word(w, 1);
            if (!x) continue;
            if (x->frame != g_frame) x->frame = g_frame, x->rd = x->wr = 0;
            if (write) x->wr = 1; else if (!x->wr) x->rd = 1;  /* a read after EXEC's own write is EXEC-internal */
            x->fn = cur_fn();
        } else {
            Word* x = word(w, 0);  /* EXEC's record from the EXEC that just ran (same g_frame) */
            if (!x) continue;
            if (write && x->rd) hazard(w, cur_fn(), x->fn, 0);
            if (!write && x->wr) hazard(w, cur_fn(), x->fn, 1);
        }
    }
}
void rc_wt(uint32_t a, uint32_t size) { trace_access(a, size, 1); }
void rc_rd(uint32_t a, uint32_t size) { trace_access(a, size, 0); }
static struct { const char* name; uint32_t n, fn; } g_mshim[256];  /* shims MAIN calls (D3D from two threads?) */
void rc_wt_shim(const char* name) {
    if (g_phase != 2) {
        for (int i = 0; i < 256; i++) {
            if (!g_mshim[i].name) g_mshim[i].name = name, g_mshim[i].fn = cur_fn();
            if (g_mshim[i].name == name) { g_mshim[i].n++; return; }
        }
        return;
    }
    for (int i = 0; i < 256; i++) {
        if (!g_shim[i].name) g_shim[i].name = name;
        if (g_shim[i].name == name) { g_shim[i].n++; return; }
    }
}
static int by_n(const void* a, const void* b) { return (int)((const Haz*)b)->n - (int)((const Haz*)a)->n; }
static void dump(void) {
    static Haz s[NHZ];
    int n = 0;
    for (int i = 0; i < NHZ; i++)
        if (g_haz[i].n) s[n++] = g_haz[i];
    qsort(s, n, sizeof *s, by_n);
    FILE* f = fopen(USM_DIR "hazards.txt", "w");
    if (!f) return;
    fprintf(f, "# %u frames: kind word main-function exec-function count\n", g_frame);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s %08x %06x %06x %u  main callers %06x %06x %06x\n", s[i].kind ? "main-reads-exec-write" : "main-writes-exec-read",
                s[i].a, s[i].main_fn, s[i].exec_fn, s[i].n, s[i].up1, s[i].up2, s[i].up3);
    for (int i = 0; i < 256 && g_shim[i].name; i++) fprintf(f, "exec-shim %s %u\n", g_shim[i].name, g_shim[i].n);
    for (int i = 0; i < 256 && g_mshim[i].name; i++)
        fprintf(f, "main-shim %s %u (first from %06x)\n", g_mshim[i].name, g_mshim[i].n, g_mshim[i].fn);
    fclose(f);
}
/* f_76ea10 (native.c) brackets EXEC; MAIN is the game thread the rest of the time, from the first EXEC on */
void rc_wt_exec_begin(Ctx* c) {
    g_c = c;
    g_stack_lo = c->esp - 0x200000, g_stack_hi = c->esp + 0x200000;  /* the game thread's guest stack */
    g_frame++, g_phase = 2, rc_wt_on = 1;
}
/* f_73ea70 (the render frame: begin, build, EXEC) is where a pipelined main thread waits for the previous EXEC, so
   MAIN only overlaps EXEC from the hand-off to here (the logic tick): stop tracing MAIN at the fence */
void rc_wt_fence(void) { if (g_phase == 1) g_phase = 0, rc_wt_on = 0; }
void rc_wt_exec_end(void) {
    g_phase = 1;  /* rc_wt_on stays set: MAIN is traced from here to the fence */
    if (g_frame % 300 == 0) dump(), vlog("hazards: %u frames, hazards.txt written\n", g_frame);
}
#endif
