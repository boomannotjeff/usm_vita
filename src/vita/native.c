/* Native versions of the game's hottest math helpers (row-vector 4x4 matrices, translated chains of tiny calls that
   cost ~15-20% of a Vita frame). tools/recomp/native.txt lists them; recomp.py then emits their translation as
   fo_XXXX and these f_XXXX take their place for every caller.
   They copy the original x87 code exactly: precision control is 53-bit, so a product kept on the x87 stack is a
   double, one stored to a float temp first is rounded (F()). They also leave eax/ecx/edx as the original does
   (MSVC LTCG callers may rely on what a callee leaves in them).
   Check mode (env USM_NATIVE_CHECK=1, vpc): every call also runs the original on a copy and compares the outputs and
   registers; logs "native check" lines. */
#include <math.h>
#include <stdlib.h>

#include "recomp.h"
#include "vita.h"

static inline float F(double x) { return (float)x; }
static inline uint32_t B(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static inline uint32_t arg(Ctx* c, int i) { return LD32(c->esp + 4 * i); }
static inline void ret(Ctx* c, uint32_t pop) { c->target = LD32(c->esp); c->esp += 4 + pop; }
static inline void ldv(float* d, uint32_t a) { for (int i = 0; i < 4; i++) d[i] = LDF32(a + 4 * i); }
static inline void stv(uint32_t a, const float* s) { for (int i = 0; i < 4; i++) STF32(a + 4 * i, s[i]); }

/* Copy-on-write for entity matrices (render pipeline, below). While the worker draws frame N, the next logic tick
   recomputes entity world matrices (4db590 -> 415a30) that the worker reads (414360): the hazard tracer's only heap
   hazard. Main saves a matrix's old 64 bytes before 415a30 overwrites it; the worker's 414360 reads the saved copy, so
   frame N is drawn from frame N's matrices (otherwise Spider-Man's parts mix two frames). One writer (main), one
   reader (worker); a generation per hand-off empties the table. */
#define FORK_N 4096
typedef struct { uint32_t key; volatile uint32_t gen; uint32_t m[16]; } Fork;
static Fork g_fork[FORK_N];
static volatile uint32_t g_fgen;  /* 0: no frame being drawn */
static uint32_t g_fsaved, g_fhits;  /* matrices saved by main / reads the worker got from a saved copy (the log) */
static void fork_save(uint32_t a) {
    uint32_t g = g_fgen;
    for (uint32_t i = (a * 2654435761u) >> 20, n = 0; n < 64; i = (i + 1) & (FORK_N - 1), n++) {
        Fork* f = &g_fork[i];
        uint32_t fg = __atomic_load_n(&f->gen, __ATOMIC_ACQUIRE);
        if (fg == g && f->key == a) return;  /* already saved this frame: keep the oldest */
        if (fg != g) {
            memcpy(f->m, (void*)RC_P(a), 64), f->key = a, g_fsaved++;
            __atomic_store_n(&f->gen, g, __ATOMIC_RELEASE);  /* published after the copy */
            return;
        }
    }
}
static const uint32_t* fork_find(uint32_t a) {
    uint32_t g = g_fgen;
    for (uint32_t i = (a * 2654435761u) >> 20, n = 0; n < 64; i = (i + 1) & (FORK_N - 1), n++) {
        Fork* f = &g_fork[i];
        if (__atomic_load_n(&f->gen, __ATOMIC_ACQUIRE) != g) return NULL;
        if (f->key == a) return f->m;
    }
    return NULL;
}
static Ctx* g_wc;  /* the render pipeline's worker context (below) */
/* The worker's own world matrices: 4db590 on the worker (w_4db590) computes into these guest slots, keyed by the
   entity's world-matrix address, instead of into the entity, so the worker never writes what main's next tick writes.
   Worker-only; a generation per hand-off empties it. */
#define WOV_N 4096
static uint32_t g_wov_key[WOV_N], g_wov_gen[WOV_N], g_wov_mem;
static uint32_t g_wbones, g_wstale;  /* the log: matrices w_4db590 computed, and those from a local still to rebuild */
static uint32_t wov_find(uint32_t a, int add) {
    for (uint32_t i = (a * 2654435761u) >> 20, n = 0; n < 64; i = (i + 1) & (WOV_N - 1), n++) {
        if (g_wov_gen[i] == g_fgen && g_wov_key[i] == a) return g_wov_mem + 64 * i;
        if (g_wov_gen[i] != g_fgen) {
            if (!add) return 0;
            g_wov_gen[i] = g_fgen, g_wov_key[i] = a;
            return g_wov_mem + 64 * i;
        }
    }
    return 0;
}
/* a 64-byte matrix as frame N saw it: the worker's own copy, else the saved copy if main has rewritten it since the
   hand-off. Read live, then look again: main publishes the copy before it writes, so a read that raced a write finds
   the copy the second time */
static void read_mat(Ctx* c, uint32_t mp, float* m, int n) {
    uint32_t o = c == g_wc && g_fgen && g_wov_mem ? wov_find(mp, 0) : 0;
    if (o) {
        for (int i = 0; i < n; i++) m[i] = LDF32(o + 4 * i);
        return;
    }
    const uint32_t* s = c == g_wc && g_fgen ? fork_find(mp) : NULL;
    if (!s) {
        for (int i = 0; i < n; i++) m[i] = LDF32(mp + 4 * i);
        if (c == g_wc && g_fgen) __atomic_thread_fence(__ATOMIC_ACQUIRE), s = fork_find(mp);
    }
    if (s) memcpy(m, s, 4 * n), g_fhits++;
}
/* n floats at p as frame N saw them, p anywhere in a saved or worker-computed matrix (rows of it: the vector
   helpers take row pointers). Worker only; elsewhere a plain load */
static void ldw(Ctx* c, float* d, uint32_t p, int n) {
    if (c == g_wc && g_fgen)
        for (uint32_t k = 0; k + 4 * n <= 64; k += 16) {
            uint32_t o = g_wov_mem ? wov_find(p - k, 0) : 0;
            if (o) {
                for (int i = 0; i < n; i++) d[i] = LDF32(o + k + 4 * i);
                return;
            }
            const uint32_t* s = fork_find(p - k);
            if (s) {
                memcpy(d, (const uint8_t*)s + k, 4 * n), g_fhits++;
                return;
            }
        }
    for (int i = 0; i < n; i++) d[i] = LDF32(p + 4 * i);
}
static float ldf(Ctx* c, uint32_t p) { float f; ldw(c, &f, p, 1); return f; }

/* 411a50: C += v * s, s a scalar (504270 uses v[i] * p[i]): products 0 and 1 stay on the stack, 2 and 3 are stored */
static void madd_late(float* C, const float* v, const float* s) {
    double t0 = (double)v[0] * s[0], t1 = (double)v[1] * s[1];
    float t2 = F((double)v[2] * s[2]), t3 = F((double)v[3] * s[3]);
    C[0] = F(t0 + C[0]), C[1] = F(t1 + C[1]), C[2] = F((double)t2 + C[2]), C[3] = F((double)t3 + C[3]);
}
/* 413530: C += v * x: only product 0 stays on the stack */
static void madd_x(float* C, const float* v, float x) {
    double t0 = (double)x * v[0];
    float t1 = F((double)x * v[1]), t2 = F((double)x * v[2]), t3 = F((double)x * v[3]);
    C[0] = F(t0 + C[0]), C[1] = F((double)t1 + C[1]), C[2] = F((double)t2 + C[2]), C[3] = F((double)t3 + C[3]);
}
/* the shared tail of 4126e0/413e90: out = L + r * z */
static void tail(float* out, const float* L, const float* r, float z) {
    double a = (double)r[0] * z, b = (double)r[1] * z;
    float c2 = F((double)r[2] * z), c3 = F((double)r[3] * z);
    out[0] = F(a + L[0]), out[1] = F((double)L[1] + b), out[2] = F((double)L[2] + c2), out[3] = F((double)L[3] + c3);
}
static void splat(float* d, float s) { d[0] = d[1] = d[2] = d[3] = s; }

/* 4126e0 core: out = v.x * r0 + v.y * r1 + v.z * r2 (a direction through the 3x4 rows) */
static void dir3(float* out, const float* r0, const float* r1, const float* r2, const float* v) {
    float L[4], s[4];
    for (int i = 0; i < 4; i++) L[i] = F((double)v[0] * r0[i]);
    splat(s, v[1]), madd_late(L, r1, s);
    tail(out, L, r2, v[2]);
}
/* 413e90 core: out = r3 + v.x * r0 + v.y * r1 + v.z * r2 (a point) */
static void pt3(float* out, const float* r0, const float* r1, const float* r2, const float* r3, const float* v) {
    float L[4], s[4];
    memcpy(L, r3, 16);
    madd_x(L, r0, v[0]);
    splat(s, v[1]), madd_late(L, r1, s);
    tail(out, L, r2, v[2]);
}

static void n_411a50(Ctx* c) {  /* thiscall (C, v, p): C += v * p.y; ret 8 */
    float C[4], v[4], p[4], s[4];
    uint32_t cp = c->ecx;
    ldv(C, cp), ldw(c, v, arg(c, 1), 4), ldw(c, p, arg(c, 2), 4);
    splat(s, p[1]), madd_late(C, v, s), stv(cp, C);
    c->eax = B(C[2]), c->edx = B(C[3]);
    ret(c, 8);
}
static void n_504270(Ctx* c) {  /* thiscall (C, v, p): C += v * p elementwise; ret 8 */
    float C[4], v[4], p[4];
    uint32_t cp = c->ecx;
    ldv(C, cp), ldw(c, v, arg(c, 1), 4), ldw(c, p, arg(c, 2), 4);
    madd_late(C, v, p), stv(cp, C);
    c->eax = B(C[2]), c->edx = B(C[3]);
    ret(c, 8);
}
static void n_413530(Ctx* c) {  /* thiscall (C, v, p): C += v * p.x; ret 8 */
    float C[4], v[4];
    uint32_t cp = c->ecx;
    ldv(C, cp), ldw(c, v, arg(c, 1), 4);
    madd_x(C, v, ldf(c, arg(c, 2))), stv(cp, C);
    c->edx = B(C[2]), c->eax = B(C[3]);
    ret(c, 8);
}
static void n_4126e0(Ctx* c) {  /* cdecl (out, r0, v, r1, v, r2, v): v read through args 3, 5 and 7 */
    float r0[4], r1[4], r2[4], L[4], s[4], o[4];
    uint32_t out = arg(c, 1);
    ldw(c, r0, arg(c, 2), 4), ldw(c, r1, arg(c, 4), 4), ldw(c, r2, arg(c, 6), 4);
    float x = ldf(c, arg(c, 3)), y = ldf(c, arg(c, 5) + 4), z = ldf(c, arg(c, 7) + 8);
    for (int i = 0; i < 4; i++) L[i] = F((double)x * r0[i]);
    splat(s, y), madd_late(L, r1, s);
    tail(o, L, r2, z), stv(out, o);
    c->eax = out, c->ecx = arg(c, 6), c->edx = B(L[3]);
    ret(c, 0);
}
static void n_413e90(Ctx* c) {  /* cdecl (out, r0, v, r1, v, r2, v, r3) */
    float r0[4], r1[4], r2[4], L[4], s[4], o[4];
    uint32_t out = arg(c, 1);
    ldw(c, L, arg(c, 8), 4), ldw(c, r0, arg(c, 2), 4), ldw(c, r1, arg(c, 4), 4), ldw(c, r2, arg(c, 6), 4);
    madd_x(L, r0, ldf(c, arg(c, 3)));
    splat(s, ldf(c, arg(c, 5) + 4)), madd_late(L, r1, s);
    tail(o, L, r2, ldf(c, arg(c, 7) + 8)), stv(out, o);
    c->eax = out, c->ecx = arg(c, 6), c->edx = B(L[3]);
    ret(c, 0);
}
static void n_4139a0(Ctx* c) {  /* cdecl (out, v, M): out = direction v through M; returns out */
    float m[12], v[4], o[4];
    uint32_t out = arg(c, 1), mp = arg(c, 3);
    ldw(c, m, mp, 12);
    ldw(c, v, arg(c, 2), 4);
    dir3(o, m, m + 4, m + 8, v), stv(out, o);
    c->eax = c->ecx = c->edx = out;
    ret(c, 0);
}
static void n_414360(Ctx* c) {  /* cdecl (out, v, M): out = point v through M; returns out */
    float m[16], v[4], o[4];
    uint32_t out = arg(c, 1), mp = arg(c, 3);
    read_mat(c, mp, m, 16);
    ldw(c, v, arg(c, 2), 4);
    pt3(o, m, m + 4, m + 8, m + 12, v), stv(out, o);
    c->eax = c->ecx = c->edx = out;
    ret(c, 0);
}
static void n_48e900(Ctx* c) {  /* thiscall ({A*, B*}, o0, o1, o2, o3): rows of A*B; B is reread per row (an
                                    output may alias it); ret 0x10 */
    float a[16], b[16], o[4];
    uint32_t th = c->ecx, ap = LD32(th), bp;
    ldw(c, a, ap, 16);
    for (int r = 0; r < 4; r++) {
        bp = LD32(th + 4);
        ldw(c, b, bp, 16);
        if (r < 3) dir3(o, b, b + 4, b + 8, a + 4 * r);
        else pt3(o, b, b + 4, b + 8, b + 12, a + 12);
        stv(arg(c, 1 + r), o);
    }
    c->ecx = arg(c, 4), c->edx = B(o[2]), c->eax = B(o[3]);
    ret(c, 0x10);
}
/* 507130: cdecl (out, {A*, B*}) -> out = the transpose of 48e900's rows (render-list build, via 770360). The original
   is 48e900 into locals, a transpose (4013c0), a row shuffle and a copy (4118f0): only moves after the product, so
   this is exact. Leaves eax = out, ecx = out's last word, edx = out + 0x30, as 4118f0 does */
static void n_507130(Ctx* c) {
    float a[16], b[16], r[16], t[4];
    uint32_t out = arg(c, 1), th = arg(c, 2);
    ldw(c, a, LD32(th), 16);
    for (int i = 0; i < 4; i++) {
        ldw(c, b, LD32(th + 4), 16);
        if (i < 3) dir3(r + 4 * i, b, b + 4, b + 8, a + 4 * i);
        else pt3(r + 4 * i, b, b + 4, b + 8, b + 12, a + 12);
    }
    for (int i = 0; i < 4; i++) t[0] = r[i], t[1] = r[4 + i], t[2] = r[8 + i], t[3] = r[12 + i], stv(out + 16 * i, t);
    c->eax = out, c->ecx = B(r[15]), c->edx = out + 0x30;
    ret(c, 0);
}

/* 822160: the CRT's _ftol2, st0 truncated to a 64-bit integer in edx:eax, popped (whatever the rounding mode: it
   rounds with fistp, then corrects by the sign of the difference). Out of range and NaN give the x87's "integer
   indefinite". ecx is left as scratch (callers treat _ftol as clobbering it). */
static void n_822160(Ctx* c) {
    f80 v = ST(0);
    FPOP();
    int64_t r = v != v || v >= 9223372036854775808.0 || v <= -9223372036854775809.0 ? INT64_MIN : (int64_t)v;
    c->eax = (uint32_t)r, c->edx = (uint32_t)((uint64_t)r >> 32);
    ret(c, 0);
}

/* ---- check mode (vpc only: the Vita build compiles it out of the hot math helpers) ---- */
#ifdef USM_DEBUG
#define USM_CHECKS 1
#else
#define USM_CHECKS 0
#endif
static int g_check = -1;
typedef struct { const char* name; uint32_t calls, bad; } Stat;
static void check(Ctx* c, void (*nat)(Ctx*), void (*orig)(Ctx*), Stat* st, const uint32_t* outs, int n, int ecx_free) {
    Ctx t;  /* on the stack: the original of 414360 calls the checked native 413e90, which nests a check */
    uint8_t pre[4][16], ref[4][16];
    for (int i = 0; i < n; i++) memcpy(pre[i], (void*)RC_P(outs[i]), 16);
    t = *c;
    orig(&t);
    for (int i = 0; i < n; i++) memcpy(ref[i], (void*)RC_P(outs[i]), 16), memcpy((void*)RC_P(outs[i]), pre[i], 16);
    nat(c);
    st->calls++;
    int bad = c->eax != t.eax || (!ecx_free && c->ecx != t.ecx) || c->edx != t.edx || c->esp != t.esp ||
              c->target != t.target || c->ftop != t.ftop || c->fdepth != t.fdepth;
    for (int i = 0; i < n; i++) bad |= memcmp(ref[i], (void*)RC_P(outs[i]), 16) != 0;
    if (bad && st->bad++ < 8) {
        vlog("native check %s: MISMATCH (call %u) eax %08x/%08x ecx %08x/%08x edx %08x/%08x esp %08x/%08x\n", st->name,
             st->calls, c->eax, t.eax, c->ecx, t.ecx, c->edx, t.edx, c->esp, t.esp);
        for (int i = 0; i < n; i++) {
            const uint32_t* r = (const uint32_t*)ref[i];
            vlog("  out %d native %08x %08x %08x %08x  original %08x %08x %08x %08x\n", i, LD32(outs[i]), LD32(outs[i] + 4),
                 LD32(outs[i] + 8), LD32(outs[i] + 12), r[0], r[1], r[2], r[3]);
        }
    }
    if ((st->calls & 0xfffff) == 0) vlog("native check %s: %u calls, %u mismatches\n", st->name, st->calls, st->bad);
}
#define NATIVE_(addr, ecx_free, n, ...)                                                           \
    void f_##addr(Ctx* c) {                                                              \
        if (USM_CHECKS && __builtin_expect(g_check, 0)) {                                        \
            if (g_check < 0) g_check = getenv("USM_NATIVE_CHECK") != NULL;               \
            if (g_check) {                                                               \
                static Stat st = {#addr};                                                \
                uint32_t o[4] = {__VA_ARGS__};                                           \
                check(c, n_##addr, fo_##addr, &st, o, n, ecx_free);                              \
                return;                                                                  \
            }                                                                            \
        }                                                                                \
        n_##addr(c);                                                                     \
    }
#define NATIVE(addr, n, ...) NATIVE_(addr, 0, n, __VA_ARGS__)
#define A(i) arg(c, i)
NATIVE(411a50, 1, c->ecx)
NATIVE(504270, 1, c->ecx)
NATIVE(413530, 1, c->ecx)
NATIVE(4126e0, 1, A(1))
NATIVE(413e90, 1, A(1))
NATIVE(4139a0, 1, A(1))
NATIVE(414360, 1, A(1))
NATIVE(48e900, 4, A(1), A(2), A(3), A(4))
NATIVE(507130, 4, A(1), A(1) + 16, A(1) + 32, A(1) + 48)
NATIVE_(822160, 1, 0, 0)

/* ---- Render pipeline ----
   The world frame (73ea70) is: 76e050 begin, 73e710 build the render lists, 76ea10 EXEC (sort them, every draw call,
   Present: ~28% of the Vita's game thread), then a tail (unbind stream 0, 775170 reset textures). With the pipeline on,
   EXEC and the tail run on a worker thread (core 2) while the main thread goes on with the next logic tick; main waits
   for it (the fence) where its next render frame starts (73ea70, 76e050), before any other EXEC, and before any D3D
   call (shims.c). The hazard tracer (wtrace.c, vpc -DRC_WTRACE) found what the two sides share in that window: no
   globals in free roam; per-thread globals go in tools/recomp/tls.txt. Known: entity matrices (4db590) can be one
   logic tick newer than the rest of a frame for objects that moved (a jitter, not a crash).
   Mode (ux0:data/usm/pipe.txt, or env USM_PIPE; a digit): 0 off, 1 the worker does it but main waits at once (no
   overlap: checks the plumbing), 2 overlapped (default). Checked on vpc: free roam, New Game, the first fight. */
#ifdef RC_WTRACE
void rc_wt_exec_begin(Ctx* c);
void rc_wt_exec_end(void);
void rc_wt_fence(void);
int pipe_skip_unbind(void) { return 0; }
#else
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
void d3d_frame_fixups(void);
float density_cap(void);  /* d3d9_null.c */
void d3d_unbind_stream0(void);
void gl_set_recorder(void);
static int g_pipe = -1, g_wdelay;
static uint32_t g_wsp, g_warg;    /* its guest stack top; EXEC's argument */
static uint32_t g_wtls[16];       /* its private copies of the per-thread globals */
static uint32_t g_wscratch;       /* its scratch arena (0x971f0c) */
static uint16_t g_wfcw;
static SceUID g_wjob, g_wdone;
volatile int g_wbusy;
static uint64_t g_wus, g_waitus;  /* worker busy, main waiting at fences (us), for the log */
static uint64_t g_t_handoff, g_t_first, g_tick_us, g_first_us;  /* where the first entity matrix lands in the tick */
static uint64_t g_t_logic;  /* 55d780 (the logic tick) start, while the worker runs */
static uint32_t g_wframes;
static int pipe_mode(void) {
    if (g_pipe < 0) {
        const char* e = getenv("USM_PIPE");
        g_pipe = e ? atoi(e) : 2;  /* on unless pipe.txt / USM_PIPE says otherwise (0 = off) */
#ifdef USM_DEBUG
        if ((e = getenv("USM_PIPE_DELAY"))) g_wdelay = atoi(e);
#endif  /* vpc: us per worker draw, to be as slow as the Vita */
        FILE* f = fopen(USM_DIR "pipe.txt", "r");
        if (f) {
            int m;
            if (fscanf(f, "%d", &m) == 1) g_pipe = m;
            fclose(f);
        }
        if (g_pipe) vlog("pipeline: mode %d (%u per-thread globals)\n", g_pipe, rc_tls_n);
    }
    return g_pipe;
}
static void call0(Ctx* w, void (*f)(Ctx*), uint32_t arg) {  /* a cdecl guest function with one argument, from C */
    w->esp = g_wsp - 8;
    ST32(w->esp + 4, arg), ST32(w->esp, 0);
    f(w);
}
static void* worker_main(void* arg) {
    (void)arg;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x40000);  /* core 2, its own (game threads and the mixer: core 1) */
    g_wc = ctx_new(0x100000);
    g_wsp = g_wc->esp;
#ifdef RC_PROFILE
    void rc_prof_worker(Ctx* c);
    rc_prof_worker(g_wc);  /* the sampler profiles this thread too */
#endif
    for (unsigned k = 0; k < rc_tls_n; k++) g_wc->tls[k] = g_wtls[k];
    sceKernelSignalSema(g_wdone, 1);
    for (;;) {
        sceKernelWaitSema(g_wjob, 1, NULL);
        uint64_t t = sceKernelGetProcessTimeWide();
        rc_fldcw(g_wc, g_wfcw);  /* the rounding mode is per thread (FPSCR) */
        gl_set_recorder();       /* the renderer's command buffers have one writer at a time: now this thread */
        call0(g_wc, fo_76ea10, g_warg);
        d3d_unbind_stream0();    /* 73ea70's tail, which main skips */
        call0(g_wc, fo_775170, 0);
        g_wus += sceKernelGetProcessTimeWide() - t;
        sceKernelSignalSema(g_wdone, 1);
    }
    return NULL;
}
static void pipe_start(void) {
    for (unsigned k = 0; k < rc_tls_n; k++) g_wtls[k] = G(guest_alloc(rc_tls_size[k] + 16));
    g_wjob = sceKernelCreateSema("usm_pipe_job", 0, 0, 1, NULL);
    g_wdone = sceKernelCreateSema("usm_pipe_done", 0, 0, 1, NULL);
    /* started the way the game's own threads are (shims_kernel32.c CreateThread): translated code nests deep on the
       host stack, and vitaGL's shader compiler may run on top of it */
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);
    pthread_t t;
    if (g_wjob < 0 || g_wdone < 0 || pthread_create(&t, &a, worker_main, NULL)) {
        vlog("pipeline: could not start the worker (semaphores %x %x), running without it\n", g_wjob, g_wdone);
        g_pipe = 0;
        return;
    }
    sceKernelWaitSema(g_wdone, 1, NULL);  /* its context is ready */
    vlog("pipeline: worker started\n");
}
/* wait for the worker, then hand back what it produced. Only the thread that hands EXEC over takes the worker's
   "done" signal; any other thread just waits for it to finish */
static const char* g_why, *g_waited;  /* the fence being passed; the last one that made main wait (for the log) */
static Ctx* g_mc;
void pipe_fence(void) {
    if (!g_wbusy) return;
    if (rc_ctx() != g_mc) {
        while (g_wbusy) sceKernelDelayThread(100);
        return;
    }
    g_waited = g_why;
    uint64_t t = sceKernelGetProcessTimeWide();
    sceKernelWaitSema(g_wdone, 1, NULL);
    g_waitus += sceKernelGetProcessTimeWide() - t;
    g_wbusy = 0, g_fgen = 0;
    for (unsigned k = 0; k < rc_tls_n; k++)
        if (rc_tls_flag[k] & 2) memcpy((void*)RC_P(rc_tls_addr0[k]), (void*)RC_P(g_wtls[k]), rc_tls_size[k]);
    gl_set_recorder();
}
/* shims.c, before a D3D call: main (or any other thread) waits while the worker is drawing */
void pipe_fence_d3d(Ctx* c, const char* name) {
    if (g_wbusy && c != g_wc) g_why = name, pipe_fence();
}
/* shims.c, every Draw* call: USM_PIPE_DELAY (us) on the worker spreads EXEC out like on the Vita (vpc testing) */
void pipe_draw_delay(Ctx* c) {
    if (g_wdelay && g_wbusy && c == g_wc)  /* a busy wait: vpc's DelayThread sleeps whole milliseconds */
        for (uint64_t t = sceKernelGetProcessTimeWide(); sceKernelGetProcessTimeWide() - t < (uint64_t)g_wdelay;) {}
}
/* d3d9_null.c: main's own copy of the tail's unbind is skipped while the worker does it */
int pipe_skip_unbind(void) { return g_wbusy && rc_ctx() != g_wc; }
static void pipe_handoff(Ctx* c) {
    if (!g_wc) pipe_start();
    if (!g_pipe) {  /* the worker could not start */
        fo_76ea10(c);
        return;
    }
    g_mc = c;
    pipe_fence();
    Ctx* w = g_wc;
    w->eax = c->eax, w->ecx = c->ecx, w->edx = c->edx, w->ebx = c->ebx, w->ebp = c->ebp, w->esi = c->esi, w->edi = c->edi;
    w->fsw = c->fsw, w->ftop = c->ftop, w->fdepth = c->fdepth, memcpy(w->st, c->st, sizeof w->st);
    g_wfcw = c->fcw, g_warg = LD32(c->esp + 4);
    uint32_t arena = LD32(0x971f10);
    if (!g_wscratch && arena) g_wscratch = G(guest_alloc(arena));
    for (unsigned k = 0; k < rc_tls_n; k++) {
        if (rc_tls_flag[k] & 1) memcpy((void*)RC_P(g_wtls[k]), (void*)RC_P(rc_tls_addr0[k]), rc_tls_size[k]);
        if (rc_tls_flag[k] & 4) ST32(g_wtls[k], g_wscratch);  /* the worker's own scratch arena */
        if (rc_tls_flag[k] & 8) {  /* a {size, alignment, base, top} stack: the worker's own block, empty */
            static uint32_t blk[16];
            if (!blk[k]) blk[k] = G(guest_alloc(LD32(g_wtls[k]) + 64));
            ST32(g_wtls[k] + 8, blk[k]), ST32(g_wtls[k] + 12, blk[k]);
        }
    }
    static uint32_t fseq;
    g_fgen = ++fseq ? fseq : ++fseq;  /* a new, empty copy-on-write table for this frame */
    g_wbusy = 1;
    if (log_on()) g_t_handoff = sceKernelGetProcessTimeWide(), g_t_first = 0;  /* only for the log line below */
    sceKernelSignalSema(g_wjob, 1);
    if (g_pipe == 1) pipe_fence();
    if (++g_wframes % 120 == 0)
        vlog("pipeline: worker %.1f ms/frame (EXEC, now off the main thread), main waited %.1f ms/frame for it (last at %s); "
             "logic until its first entity matrix: %.0f%% of the tick; matrices kept for the worker %.1f/frame, used %.1f; "
             "bones computed by the worker %.1f/frame (%.1f from a local still to rebuild)\n",
             g_wus / 1000.0 / 120, g_waitus / 1000.0 / 120, g_waited ? g_waited : "-",
             g_tick_us ? 100.0 * g_first_us / g_tick_us : 0.0, g_fsaved / 120.0, g_fhits / 120.0, g_wbones / 120.0,
             g_wstale / 120.0),
            g_wus = g_waitus = g_first_us = g_tick_us = 0, g_fsaved = g_fhits = g_wbones = g_wstale = 0;
    ret(c, 0);
}
#endif
/* 73ea70: the world render frame; the pipeline's fence */
#ifdef RC_WTRACE
static int g_wt_build;  /* WT_BUILD set: the tracer's EXEC phase is the whole world frame (build + EXEC) */
#endif
void f_73ea70(Ctx* c) {
#ifdef RC_WTRACE
    rc_wt_fence();
    if (getenv("WT_BUILD")) {
        g_wt_build = 1, rc_wt_exec_begin(c);
        fo_73ea70(c);
        rc_wt_exec_end(), g_wt_build = 0;
        return;
    }
#else
    if (pipe_mode()) g_why = "73ea70 (next world frame)", pipe_fence();
    d3d_frame_fixups();
#endif
    fo_73ea70(c);
}
/* 76e050: begin a render frame (resets the scratch arena the worker's lists live in) */
void f_76e050(Ctx* c) {
#ifndef RC_WTRACE
    if (g_pipe > 0) g_why = "76e050 (a render frame begins)", pipe_fence();
#endif
    fo_76e050(c);
}
/* 55a680 (a streamed city chunk is parsed) and 509d70 (it spawns an entity through the game allocator 4c1fa0, then
   calls its vtable): the worker must not be inside EXEC meanwhile. Two Vita crashes in exactly this path (4c1fa0 on
   10-28; a vtable slot of -1 called from 509d70 under 55a680 with fast4) while the worker was drawing; the tracer
   only ever covered free roam, not streaming. Streaming is rare per frame, so the wait costs little. */
void f_55a680(Ctx* c) {
#ifndef RC_WTRACE
    if (g_pipe > 0) g_why = "55a680 (streaming a city chunk)", pipe_fence();
#endif
    fo_55a680(c);
}
void f_509d70(Ctx* c) {
#ifndef RC_WTRACE
    if (g_pipe > 0) g_why = "509d70 (streaming spawns an entity)", pipe_fence();
#endif
    fo_509d70(c);
}
/* 775170: the world frame's tail resets textures; the worker does it after EXEC */
void f_775170(Ctx* c) {
#ifndef RC_WTRACE
    if (g_wbusy && LD32(c->esp) == 0x73eb33 && rc_ctx() != g_wc) {
        ret(c, 0);
        return;
    }
#endif
    fo_775170(c);
}
void f_76ea10(Ctx* c) {
    if (LD32(c->esp) != 0x73eb15) {  /* only the world frame's EXEC (from 73ea70) is deferred */
#ifndef RC_WTRACE
        if (g_pipe > 0) g_why = "76ea10 (another EXEC)", pipe_fence();
#endif
        fo_76ea10(c);
        return;
    }
#ifdef RC_WTRACE
    if (g_wt_build) { fo_76ea10(c); return; }
    rc_wt_exec_begin(c);
    fo_76ea10(c);
    rc_wt_exec_end();
#else
    if (pipe_mode() > 0) pipe_handoff(c);
    else fo_76ea10(c);
#endif
}

/* 820890: the display name of a binding (device, code), used by prompts and the Controls menu. On PC a pad shows
   "Button 5", "LX+"; the Vita's are named after its controls (input.c: buttons 1..8 = Cross Circle Square Triangle L
   R Select Start, left stick on X/Y, right stick on Z/Rz, D-pad as the PoV). Keyboard and mouse keep the original. */
void f_820890(Ctx* c) {  /* thiscall (input manager, device, code), returns a string; ret 8 */
    /* face buttons and L/R are icon characters (f_7791a0), the rest words */
    static const char* const btn[] = {"\x80", "\x81", "\x8d", "\x8f", "\x90", "\x9d", "SELECT", "START", "REAR TOUCH"};
    static const char* const ax[] = {"LEFT STICK RIGHT", "LEFT STICK LEFT", "LEFT STICK DOWN", "LEFT STICK UP",
                                     "RIGHT STICK RIGHT", "RIGHT STICK LEFT", 0, 0, 0, 0, "RIGHT STICK DOWN", "RIGHT STICK UP"};
    static const char* const pov[] = {"D-PAD RIGHT", "D-PAD LEFT", "D-PAD DOWN", "D-PAD UP"};
    uint32_t dev = arg(c, 1), code = arg(c, 2), from = LD32(c->esp);
    const char* s = NULL;
    /* The tutorial token loop (0x5953a3: "~square" -> the name of Punch's binding) takes the first of an action's
       three bindings that has a name: keyboard ones come first, and the Vita's fake keyboard has no key names (an
       empty string, "PRESS "" TO PUNCH"). No name for keyboard/mouse there, so it falls through to the pad slot. */
    if (dev <= 2 && (from == 0x595664 || from == 0x59567b || from == 0x595692)) {
        c->eax = 0;
        ret(c, 8);
        return;
    }
    if (dev >= 3 && dev <= 12) {
        if (code > 0x14 && code - 0x15 < sizeof btn / sizeof *btn) s = btn[code - 0x15];
        else if (code >= 1 && code <= 12) s = ax[code - 1];
        else if (code >= 0x11 && code <= 0x14) s = pov[code - 0x11];
    }
    if (!s) {
        fo_820890(c);
        return;
    }
    uint32_t buf = 0x987a20;  /* the original's own 80-byte result buffer for pad buttons */
    memset((void*)RC_P(buf), 0, 80);
    strcpy((char*)RC_P(buf), s);
    c->eax = buf;
    ret(c, 8);
}

/* Button icons in text, like the PS2 version's prompts. 6 characters no English text uses stand for the icons
   (slot k of the glyphs.h atlas: cross, circle, square, triangle, L, R). */
static const uint8_t icon_char[6] = {0x80, 0x81, 0x8d, 0x8f, 0x90, 0x9d};
static int is_icon_char(uint8_t ch) { return memchr(icon_char, ch, sizeof icon_char) != NULL; }

/* 7791a0: makes a font from its descriptor text. Font: +0x24 texture, +0x28 glyphs (7 ints: x, y, glyph w, glyph h,
   offset x, offset y, advance), +0x30 cell height, +0x38 first char, +0x3c count, +0x4c texcoords {u0 v0 u1 v1}.
   Each icon character becomes a square glyph the size of a capital, centred on the capitals, with texcoords at
   u 1000+k, v -1000 (no real texture goes there; gl.c icon_quad swaps in the icon atlas). The tables grow if the font stops before 0x9d. */
void f_7791a0(Ctx* c) {  /* cdecl (name, descriptor text), returns the font */
    fo_7791a0(c);
    uint32_t f = c->eax;
    if (!f) return;
    uint32_t first = LD32(f + 0x38), num = LD32(f + 0x3c), gl = LD32(f + 0x28), uv = LD32(f + 0x4c);
    if (!gl || !uv || first > 'A' || first + num <= 'A') return;
    if (first + num <= 0x9d) {
        uint32_t n = 0x9e - first, g2 = G(guest_alloc(n * 0x1c)), u2 = G(guest_alloc(n * 16));
        memcpy(H(g2), H(gl), num * 0x1c), memcpy(H(u2), H(uv), num * 16);
        for (uint32_t i = num; i < n; i++)  /* new chars draw as a space, as the renderer did with them before */
            memcpy(H(g2 + i * 0x1c), H(gl + (' ' - first) * 0x1c), 0x1c), memcpy(H(u2 + i * 16), H(uv + (' ' - first) * 16), 16);
        ST32(f + 0x28, gl = g2), ST32(f + 0x4c, uv = u2), ST32(f + 0x3c, num = n);
    }
    uint32_t a = gl + ('A' - first) * 0x1c;
    int32_t ay = (int32_t)LD32(a + 20), ah = (int32_t)LD32(a + 12), s = ah * 5 / 4;
    for (int k = 0; k < 6; k++) {
        uint32_t g = gl + (icon_char[k] - first) * 0x1c, t = uv + (icon_char[k] - first) * 16;
        ST32(g + 8, (uint32_t)s), ST32(g + 12, (uint32_t)s), ST32(g + 16, 0), ST32(g + 20, (uint32_t)(ay + ah / 2 - s / 2));
        ST32(g + 24, (uint32_t)(s - s / 16));  /* the icon's disc has a transparent edge: this spaces it like a letter */
        STF32(t, 1000.f + k), STF32(t + 4, -1000.f), STF32(t + 8, 1001.f + k), STF32(t + 12, -999.f);
    }
}

/* 779570: lays out a text string for drawing. Prompt texts quote their button token (PRESS "~square" TO PUNCH); with an
   icon the quotes go, as on the PS2. The layout keeps pointers into the string until the frame is drawn, so the
   unquoted copy lives in a small cache keyed by the original text (the same strings come back every frame). */
void f_779570(Ctx* c) {  /* cdecl (font, out, x, y, sx, sy, color, text, count) */
    uint32_t sp = c->esp, text = arg(c, 8);
    const uint8_t* s = (const uint8_t*)H(text);
    int has = 0;
    for (const uint8_t* p = s; p && *p; p++)
        if (*p == '"' && (is_icon_char(p[1]) || (p > s && is_icon_char(p[-1])))) { has = 1; break; }
    if (!has || strlen((const char*)s) >= 256) { fo_779570(c); return; }
    static struct { char key[256]; uint32_t copy; } cache[32];
    static unsigned next;
    int k = -1;
    for (int i = 0; i < 32; i++)
        if (cache[i].copy && !strcmp(cache[i].key, (const char*)s)) { k = i; break; }
    if (k < 0) {
        k = (int)(next++ % 32);
        strcpy(cache[k].key, (const char*)s);
        if (!cache[k].copy) cache[k].copy = G(guest_alloc(256));
        char* o = (char*)H(cache[k].copy);
        for (const uint8_t* p = s; *p; p++)
            if (!(*p == '"' && (is_icon_char(p[1]) || (p > s && is_icon_char(p[-1]))))) *o++ = (char)*p;
        *o = 0;
    }
    ST32(sp + 4 * 8, cache[k].copy);
    fo_779570(c);
    ST32(sp + 4 * 8, text);  /* the caller's argument slot as it was */
}

/* 81c580: the PC text table lookup (table, key) -> string. RESUME is hardcoded as PRESS "SPACE" TO RESUME (Data/
   usm_lte.usm); on the Vita, Start pauses and resumes. */
void f_81c580(Ctx* c) {  /* cdecl */
    static uint32_t resume;
    const char* key = (const char*)RC_P(arg(c, 2));
    if (strcmp(key, "RESUME") == 0) {
        if (!resume) resume = G(guest_alloc(32)), strcpy((char*)RC_P(resume), "PRESS \"START\" TO RESUME");
        c->eax = resume;
        ret(c, 0);
        return;
    }
    fo_81c580(c);
}

/* dst = a * b with the game's own 415a30, a and b as frame N saw them (worker) */
static void w_mul(Ctx* c, uint32_t dst, uint32_t a, uint32_t b) {
    static uint32_t buf;  /* guest: A, B, {A*, B*} */
    if (!buf) buf = G(guest_alloc(64 * 2 + 8));
    float m[16];
    read_mat(c, a, m, 16);
    for (int i = 0; i < 16; i++) STF32(buf + 4 * i, m[i]);
    read_mat(c, b, m, 16);
    for (int i = 0; i < 16; i++) STF32(buf + 64 + 4 * i, m[i]);
    ST32(buf + 128, buf), ST32(buf + 132, buf + 64);
    uint32_t sp = c->esp, ecx = c->ecx;
    c->esp -= 8, ST32(c->esp + 4, buf + 128), ST32(c->esp, 0), c->ecx = dst;
    fo_415a30(c);
    c->esp = sp, c->ecx = ecx;
}
/* 4db590 on the worker. Body-part (bone) world matrices are computed lazily while drawing, i.e. on the worker, from
   local matrices main's next tick is rewriting: mixing two frames is the "moogly" animation, and skipping it left
   parts a frame or more late. Same walk as 4db590 (up the dirty parents, then down, children unless arg), but every
   matrix read is frame N's (read_mat) and results go to the worker's own slots (wov), never into the entity; no dirty
   bits cleared, and a local still to rebuild (4d6050, flag bit 27) is used as is (counted: one frame late). */
static void w_4db590(Ctx* c) {
    if (!g_wov_mem) g_wov_mem = G(guest_alloc(WOV_N * 64));
    uint32_t e = c->ecx, up = LD8(c->esp + 4), st[512];
    int n = 0;
    if (LD32(e + 0x14) != LD32(e + 0xc))
        for (uint32_t p; (p = LD32(e + 0x20)) && (LD32(p + 8) >> 28 & 1);) {
            if (up && n < 511) st[n++] = e;
            e = p;
            if (LD32(e + 0x14) == LD32(e + 0xc)) break;
        }
    st[n++] = e;
    while (n) {
        uint32_t x = st[--n], w = LD32(x + 0x14), l = LD32(x + 0xc), f = LD32(x + 8), dst;
        if (w != l && (dst = wov_find(w, 1))) {
            uint32_t par = LD32(x + 0x20);
            g_wbones++;
            if (!par) {
                float m[16];
                if (f >> 27 & 1) g_wstale++;
                read_mat(c, l, m, 16);
                for (int i = 0; i < 16; i++) STF32(dst + 4 * i, m[i]);
            } else {
                uint32_t a = 0, owner = par;
                if ((int8_t)LD8(x + 5) < 0 && f >> 27 & 1 && !(f >> 8 & 1)) {  /* a skeleton bone: its palette entry */
                    uint32_t o = LD32(x + 0x34), k = LD8(x + 0x42);
                    if (k < LD16(o + 0xe4)) a = LD32(o + 0xe0) + ((k - 1) & 0xffff) * 64, owner = o;
                }
                if (!a) a = l, g_wstale += f >> 27 & 1;
                w_mul(c, dst, a, LD32(owner + 0x14));
            }
        }
        if (!up)
            for (uint32_t ch = LD32(x + 0x24); ch && n < 512; ch = LD32(ch + 0x28)) st[n++] = ch;
    }
    ret(c, 4);
}

/* 4db590: an entity's world matrix (computed and cached; written through 415a30). Only measured: the first one comes
   ~9% into the logic tick (vpc), too early for a fence, hence the copy-on-write in 415a30 */
void f_4db590(Ctx* c) {
#ifndef RC_WTRACE
    if (g_t_logic && !g_t_first && rc_ctx() == g_mc) g_t_first = sceKernelGetProcessTimeWide();
    if (g_wc && c == g_wc) { w_4db590(c); return; }
#endif
    fo_4db590(c);
}

/* 55d780: the game's logic tick (game states). Only measured: how far into it the first entity matrix is computed,
   which is how much of the tick can overlap the pipeline's worker before the 4db590 fence */
void f_55d780(Ctx* c) {
#ifndef RC_WTRACE
    if (g_t_handoff && rc_ctx() == g_mc) {
        g_t_logic = sceKernelGetProcessTimeWide(), g_t_first = 0;
        fo_55d780(c);
        uint64_t now = sceKernelGetProcessTimeWide();
        g_tick_us += now - g_t_logic, g_first_us += (g_t_first ? g_t_first : now) - g_t_logic, g_t_logic = g_t_handoff = 0;
        return;
    }
#endif
    fo_55d780(c);
}

/* 415a30: this = A * B (a 4x4 matrix, written whole): main saves the old one first while the worker draws */
void f_415a30(Ctx* c) {
#ifndef RC_WTRACE
    uint32_t dst = c->ecx;
    if (g_fgen && c == g_mc && dst - (c->esp - 0x100000) >= 0x200000) fork_save(dst);  /* not main's own stack */
    if (g_fgen && c == g_wc) {  /* the worker multiplies frame N's A and B (private copies), not what main rewrites */
        static uint32_t buf;
        if (!buf) buf = G(guest_alloc(64 * 2 + 8));
        uint32_t pr = arg(c, 1);
        float m[16];
        ldw(c, m, LD32(pr), 16);
        for (int i = 0; i < 16; i++) STF32(buf + 4 * i, m[i]);
        ldw(c, m, LD32(pr + 4), 16);
        for (int i = 0; i < 16; i++) STF32(buf + 64 + 4 * i, m[i]);
        ST32(buf + 128, buf), ST32(buf + 132, buf + 64), ST32(c->esp + 4, buf + 128);
    }
#endif
    fo_415a30(c);
}

/* 6c2e10: sets the pedestrian density (0x937ff0). The traffic manager 6d8610 recomputes the traffic density
   (0x937ff8, 0.2..1, cars allowed = 30 x it) every logic tick and passes the same value here, so a cap applied once
   per frame (d3d9_null.c cap_density) was undone before the spawners read it. Both are capped here, at the source. */
void f_6c2e10(Ctx* c) {  /* cdecl (float) */
    float cap = density_cap(), d;
    uint32_t a = c->esp + 4;
    memcpy(&d, (void*)RC_P(a), 4);
    if (d > cap) STF32(a, cap);
    if (LDF32(0x937ff8) > cap) STF32(0x937ff8, cap);
    fo_6c2e10(c);
}

/* 4a6110: one character's animation step (thiscall: animator, dt, flag; ret 8): advance its clips by dt (4b06a0),
   build the pose (4b0860), apply it. 537170 calls it for every animated character every logic tick: ~21% of the
   Vita's main thread in a fight-heavy interior (prof6). Characters farther than ux0:data/usm/anim_half.txt metres
   (default 12; 0 = off) from Spider-Man (the hero, 0x95c770 + 0x230) step every other tick, half of them each tick,
   with the time they skipped added, so their animations keep their speed. */
static float anim_half_dist(void) {
    static float d = -1;
    if (d < 0) {
        d = 12;
        FILE* f = fopen(USM_DIR "anim_half.txt", "r");
        if (f) {
            if (fscanf(f, "%f", &d) != 1 || d < 0) d = 12;
            fclose(f);
        }
        if (getenv("VPC_ANIM_HALF")) d = (float)atof(getenv("VPC_ANIM_HALF"));  /* vpc tests */
        vlog("animation: characters past %.0f m step every other tick (anim_half.txt; 0 = off)\n", d);
    }
    return d;
}
void f_4a6110(Ctx* c) {
    float far = anim_half_dist();
    uint32_t self = c->ecx, ent = LD32(self + 4), world = LD32(0x95c770), hero = world ? LD32(world + 0x230) : 0;
    if (far > 0 && ent && hero && ent != hero) {
        uint32_t m = LD32(ent + 0x14), hm = LD32(hero + 0x14);  /* world matrices: position at +0x30 */
        if (m && hm) {
            float dx = LDF32(m + 0x30) - LDF32(hm + 0x30), dy = LDF32(m + 0x34) - LDF32(hm + 0x34),
                  dz = LDF32(m + 0x38) - LDF32(hm + 0x38);
            if (dx * dx + dy * dy + dz * dz > far * far) {
                static struct { uint32_t key; float acc; } slot[1024];
                extern uint32_t d3d_frame_count(void);
                uint32_t i = ((self >> 4) * 2654435761u) >> 22;
                float dt = LDF32(c->esp + 4);
                if (slot[i].key != self) {  /* first time seen (a new character has no pose yet): always step it */
                    slot[i].key = self, slot[i].acc = 0;
                    fo_4a6110(c);
                    return;
                }
                static int said;
                if (!said) said = 1, vlog("animation: first half-rate character %x, %.0f m from the hero %x\n", ent,
                                          sqrtf(dx * dx + dy * dy + dz * dz), hero);
                if (((self >> 4) ^ d3d_frame_count()) & 1) {  /* this tick: skipped, its time kept */
                    /* through the function's own early exit (byte +0x50 set = "don't step"), not a bare return: it
                       is called through a vtable, and the caller relies on the translation's register write-back */
                    slot[i].acc += dt;
                    uint8_t was = LD8(self + 0x50);
                    ST8(self + 0x50, 1);
                    fo_4a6110(c);
                    ST8(self + 0x50, was);
                    return;
                }
                STF32(c->esp + 4, dt + slot[i].acc), slot[i].acc = 0;
            }
        }
    }
    fo_4a6110(c);
}
