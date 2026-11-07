/* Native versions of the game's hottest math helpers (row-vector 4x4 matrices, translated chains of tiny calls that
   cost ~15-20% of a Vita frame). tools/recomp/native.txt lists them; recomp.py then emits their translation as
   fo_XXXX and these f_XXXX take their place for every caller.
   They do what the original x87 code does, in float: the game ran the x87 at 24 bits, doubles here only cost time.
   They leave eax/ecx/edx as the original does (MSVC LTCG callers may rely on what a callee leaves in them).
   Check mode (env USM_NATIVE_CHECK=1, vpc): every call also runs the original on a copy and compares the outputs and
   registers; logs "native check" lines. */
#include <math.h>
#include <stdlib.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "recomp.h"
#include "vita.h"
#include "d3d9.h"

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
/* One bit per hashed matrix row that has a saved or worker-computed copy this generation, one table per writer (main,
   worker), both emptied at the hand-off: the worker's vector helpers read thousands of rows that have none, and two
   hash probes per row of each were ~25% of its time (scan1) */
static uint32_t g_rows_m[1024], g_rows_w[1024];
static inline uint32_t row_hash(uint32_t p) { return (p * 2654435761u) >> 17; }
static inline void rows_mark(uint32_t* t, uint32_t a) {
    for (uint32_t k = 0; k < 64; k += 16) {
        uint32_t h = row_hash(a + k);
        t[h >> 5] |= 1u << (h & 31);
    }
}
static inline int row_kept(uint32_t p) {
    uint32_t h = row_hash(p), b = 1u << (h & 31);
    return ((g_rows_m[h >> 5] | g_rows_w[h >> 5]) & b) != 0;
}
static void fork_save(uint32_t a) {
    uint32_t g = g_fgen;
    rows_mark(g_rows_m, a);  /* before the copy is published and the matrix rewritten: see read_mat */
    __atomic_thread_fence(__ATOMIC_RELEASE);
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
Ctx* g_wc;  /* the render pipeline's worker context (below) */
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
            rows_mark(g_rows_w, a);
            return g_wov_mem + 64 * i;
        }
    }
    return 0;
}
/* a 64-byte matrix as frame N saw it: the worker's own copy, else the saved copy if main has rewritten it since the
   hand-off. Read live, then look again: main publishes the copy before it writes, so a read that raced a write finds
   the copy the second time */
__attribute__((hot)) static void read_mat(Ctx* c, uint32_t mp, float* m, int n) {
    if (c != g_wc || !g_fgen) {
        for (int i = 0; i < n; i++) m[i] = LDF32(mp + 4 * i);
        return;
    }
    if (!row_kept(mp)) {
        for (int i = 0; i < n; i++) m[i] = LDF32(mp + 4 * i);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!row_kept(mp)) return;
    }
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
__attribute__((hot)) static void ldw(Ctx* c, float* d, uint32_t p, int n) {
    if (c == g_wc && g_fgen && row_kept(p))
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

/* The vector core, in float: the game never set its FPU to 53 bits, so the x87 ran these at 24 and doubles here only cost
   time (a VFP double multiply is the slowest thing on the Cortex-A9). NEON does the 4 lanes in one go on the Vita. */
static inline void madd_late(float* C, const float* v, const float* s) {  /* C += v * s, by lane */
#ifdef __ARM_NEON
    vst1q_f32(C, vmlaq_f32(vld1q_f32(C), vld1q_f32(v), vld1q_f32(s)));
#else
    for (int i = 0; i < 4; i++) C[i] += v[i] * s[i];
#endif
}
static inline void madd_x(float* C, const float* v, float x) {  /* C += v * x */
#ifdef __ARM_NEON
    vst1q_f32(C, vmlaq_n_f32(vld1q_f32(C), vld1q_f32(v), x));
#else
    for (int i = 0; i < 4; i++) C[i] += v[i] * x;
#endif
}
static inline void tail(float* out, const float* L, const float* r, float z) {  /* out = L + r * z */
#ifdef __ARM_NEON
    vst1q_f32(out, vmlaq_n_f32(vld1q_f32(L), vld1q_f32(r), z));
#else
    for (int i = 0; i < 4; i++) out[i] = L[i] + r[i] * z;
#endif
}
static inline void mul_x(float* L, const float* r, float x) {  /* L = r * x */
#ifdef __ARM_NEON
    vst1q_f32(L, vmulq_n_f32(vld1q_f32(r), x));
#else
    for (int i = 0; i < 4; i++) L[i] = r[i] * x;
#endif
}

/* 4126e0 core: out = v.x * r0 + v.y * r1 + v.z * r2 (a direction through the 3x4 rows) */
static inline void dir3(float* out, const float* r0, const float* r1, const float* r2, const float* v) {
    float L[4];
    mul_x(L, r0, v[0]);
    madd_x(L, r1, v[1]);
    tail(out, L, r2, v[2]);
}
/* 413e90 core: out = r3 + v.x * r0 + v.y * r1 + v.z * r2 (a point) */
static inline void pt3(float* out, const float* r0, const float* r1, const float* r2, const float* r3, const float* v) {
    float L[4];
    memcpy(L, r3, 16);
    madd_x(L, r0, v[0]);
    madd_x(L, r1, v[1]);
    tail(out, L, r2, v[2]);
}

static void n_411a50(Ctx* c) {  /* thiscall (C, v, p): C += v * p.y; ret 8 */
    float C[4], v[4], p[4];
    uint32_t cp = c->ecx;
    ldv(C, cp), ldw(c, v, arg(c, 1), 4), ldw(c, p, arg(c, 2), 4);
    madd_x(C, v, p[1]), stv(cp, C);
    c->eax = B(C[2]), c->edx = B(C[3]);
    ret(c, 8);
}
static void n_411ac0(Ctx* c) {  /* thiscall (C, v, p): C += v * p.z; ret 8 */
    float C[4], v[4], p[4];
    uint32_t cp = c->ecx;
    ldv(C, cp), ldw(c, v, arg(c, 1), 4), ldw(c, p, arg(c, 2), 4);
    madd_x(C, v, p[2]), stv(cp, C);
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
    float r0[4], r1[4], r2[4], L[4], o[4];
    uint32_t out = arg(c, 1);
    ldw(c, r0, arg(c, 2), 4), ldw(c, r1, arg(c, 4), 4), ldw(c, r2, arg(c, 6), 4);
    float x = ldf(c, arg(c, 3)), y = ldf(c, arg(c, 5) + 4), z = ldf(c, arg(c, 7) + 8);
    mul_x(L, r0, x);
    madd_x(L, r1, y);
    tail(o, L, r2, z), stv(out, o);
    c->eax = out, c->ecx = arg(c, 6), c->edx = B(L[3]);
    ret(c, 0);
}
static void n_413e90(Ctx* c) {  /* cdecl (out, r0, v, r1, v, r2, v, r3) */
    float r0[4], r1[4], r2[4], L[4], o[4];
    uint32_t out = arg(c, 1);
    ldw(c, L, arg(c, 8), 4), ldw(c, r0, arg(c, 2), 4), ldw(c, r1, arg(c, 4), 4), ldw(c, r2, arg(c, 6), 4);
    madd_x(L, r0, ldf(c, arg(c, 3)));
    madd_x(L, r1, ldf(c, arg(c, 5) + 4));
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
    __attribute__((hot)) void f_##addr(Ctx* c) {                                         \
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
NATIVE(411ac0, 1, c->ecx)
NATIVE(504270, 1, c->ecx)
NATIVE(413530, 1, c->ecx)
NATIVE(4126e0, 1, A(1))
NATIVE(413e90, 1, A(1))
NATIVE(4139a0, 1, A(1))
NATIVE(414360, 1, A(1))
NATIVE(48e900, 4, A(1), A(2), A(3), A(4))
NATIVE(507130, 4, A(1), A(1) + 16, A(1) + 32, A(1) + 48)
NATIVE_(822160, 1, 0, 0)

/* ---- Particle helpers: the per-sprite calls of the 8 particle renderers 75c1e0..7627c0 (see f_75af90), and the vector sine, which
   also went through the ceil import 4 times a call: 2,860 calls a frame in a burning street, the game's most called import ---- */
static inline int64_t tl(float v) {  /* _ftol */
    return v != v || v >= 9223372036854775808.0f || v <= -9223372036854775808.0f ? INT64_MIN : (int64_t)v;
}
static void n_75b9e0(Ctx* c) {  /* thiscall (position, u, v): a packed vertex: 3 truncated shorts, then v << 4 | u & 15; ret 12 */
    uint32_t o = c->ecx, p = arg(c, 1), u = arg(c, 2) & 255, v = arg(c, 3);
    int64_t x = tl(LDF32(p)), y = tl(LDF32(p + 4)), z = tl(LDF32(p + 8));
    ST16(o, (uint16_t)x), ST16(o + 2, (uint16_t)y), ST16(o + 4, (uint16_t)z);
    c->eax = v << 4 | (u & 15);
    ST16(o + 6, (uint16_t)c->eax);
    c->ecx = u & 15, c->edx = (uint32_t)((uint64_t)z >> 32);
    ret(c, 12);
}
static void n_75b950(Ctx* c) {  /* thiscall (colour): 4 floats to bytes, x255 and truncated, r and b swapped; ret 4 */
    uint32_t o = c->ecx, p = arg(c, 1);
    float k = LDF32(0x871978);
    int64_t a = tl(LDF32(p + 8) * k), b = tl(LDF32(p + 4) * k), d = tl(LDF32(p) * k), e = tl(LDF32(p + 12) * k);
    ST8(o, (uint8_t)a), ST8(o + 1, (uint8_t)b), ST8(o + 2, (uint8_t)d), ST8(o + 3, (uint8_t)e);
    c->eax = o, c->edx = (uint32_t)((uint64_t)e >> 32);
    ret(c, 4);
}
static void n_75bf10(Ctx* c) {  /* cdecl (out, range): a random vector in +-range; the w term reads a word left on the stack, as the original */
    if ((LD32(0x971638) & 3) != 3) {  /* the first call sets its constants up */
        fo_75bf10(c);
        return;
    }
    uint32_t out = arg(c, 1), p = arg(c, 2), r = LD32(0x93ad40), v[3];
    for (int i = 0; i < 3; i++) r = r * 0x41c64e6du + 0x3039, v[i] = r >> 16;
    ST32(0x93ad40, r);
    float k = LDF32(0x8b5544), x = (float)(int32_t)v[0] * k, y = (float)(int32_t)v[1] * k, z = (float)(int32_t)v[2] * k;
    float s0 = LDF32(0x971618) * x - LDF32(0x971628), s1 = LDF32(0x97161c) * y - LDF32(0x97162c);
    float s2 = LDF32(0x971620) * z - LDF32(0x971630), s3 = LDF32(c->esp - 4) * LDF32(0x971624) - LDF32(0x971634);
    float w = s3 * LDF32(p + 12), t = s2 * LDF32(p + 8), u = s1 * LDF32(p + 4), q = s0 * LDF32(p);
    STF32(out, q), STF32(out + 4, u), STF32(out + 8, t), STF32(out + 12, w);
    c->eax = out, c->ecx = r, c->edx = 0;
    ret(c, 0);
}
static void n_75b870(Ctx* c) {  /* cdecl (out, table, t): interpolate a 16-row table of vectors, t clamped to 0..0.99 */
    uint32_t out = arg(c, 1), tb = arg(c, 2);
    float t = LDF32(c->esp + 12), hi = LDF32(0x87eb44), lo = LDF32(0x871780), x;
    if (hi < t) x = hi;
    else x = lo > t ? lo : t;
    x *= LDF32(0x8a5a68);
    int64_t i = tl(x);
    uint32_t a = tb + (uint32_t)i * 16;
    float f = x - (float)(int32_t)i;
    STF32(c->esp + 12, f);  /* the original keeps it in its argument slot */
    float r[4];
    for (int k = 0; k < 4; k++) r[k] = (LDF32(a + 16 + 4 * k) - LDF32(a + 4 * k)) * f + LDF32(a + 4 * k);
    for (int k = 0; k < 4; k++) STF32(out + 4 * k, r[k]);
    c->eax = out, c->ecx = tb, c->edx = (uint32_t)((uint64_t)i >> 32);
    ret(c, 0);
}
static void n_5044f0(Ctx* c) {  /* cdecl (out, angle, 1 / 2pi): sine of each component, a polynomial on the phase */
    uint32_t out = arg(c, 1), in = arg(c, 2), p = arg(c, 3);
    float half = LDF32(0x870d44), quarter = LDF32(0x87e6f0), k1 = LDF32(0x883528), k9 = LDF32(0x88352c), r[4];
    for (int i = 0; i < 4; i++) {
        float a = -fabsf(LDF32(in + 4 * i)), m = a * LDF32(p + 4 * i), f = ceilf(m) - m;
        float y = fabsf(f - half) - quarter, y2 = y * y, y3 = y2 * y, y4 = y2 * y2, y5 = y4 * y, y7 = y3 * y4, y9 = y5 * y4;
        float q = y9 * k9;
        q += y7 * LDF32(0x882578 + 4 * i), q += y5 * LDF32(0x882568 + 4 * i), q += y3 * LDF32(0x882558 + 4 * i);
        r[i] = y * k1 + q;
    }
    for (int i = 0; i < 4; i++) STF32(out + 4 * i, r[i]);
    c->eax = out;
    ret(c, 0);
}
NATIVE_(75b9e0, 0, 1, c->ecx)
NATIVE_(75b950, 0, 1, c->ecx)
NATIVE_(75bf10, 1, 2, A(1), 0x93ad40)
NATIVE_(75b870, 0, 2, A(1), c->esp + 12)
NATIVE_(5044f0, 1, 1, A(1))

/* ---- Render pipeline ----
   The world frame (73ea70) is: 76e050 begin, 73e710 build the render lists, 76ea10 EXEC (sort them, every draw call,
   Present: ~28% of the Vita's game thread), then a tail (unbind stream 0, 775170 reset textures). With the pipeline on,
   EXEC and the tail run on a worker thread (core 2) while the main thread goes on with the next logic tick; main waits
   for it (the fence) where its next render frame starts (73ea70, 76e050), before any other EXEC, and before any D3D
   call (shims.c). The hazard tracer (wtrace.c, vpc -DRC_WTRACE) found what the two sides share in that window: no
   globals in free roam; per-thread globals go in tools/recomp/tls.txt. Known: entity matrices (4db590) can be one
   logic tick newer than the rest of a frame for objects that moved (a jitter, not a crash).
   Mode 3 also overlaps the next render frame's prologue and list build: a frame then takes max(main, worker, render)
   instead of max(logic, worker) + build. The game already keeps two of what the two sides fill (the dynamic VB/IB pair,
   the second allocator, flipped by EXEC), the scratch arena gets a second block (arena_flip), and the small globals both
   touch are per-thread (tls.txt, "in"): the worker works on frame N's copies, and pipe_after_handoff does on the real
   ones what EXEC would have done to them by the time main gets there.
   Mode (ux0:data/usm/pipe.txt, or env USM_PIPE; a digit): 0 off, 1 the worker does it but main waits at once (no
   overlap: checks the plumbing), 2 overlaps the logic tick only, 3 overlaps the build too (default). */
/* EXEC's effects on the globals main keeps using (76ea10, 76e980): flip the dynamic VB/IB pair and restart its cursors,
   back to the root view, flip the second allocator, count the Present and the draw stamp, clear the statistics */
void pipe_after_handoff(void) {
    ST32(0x972a5c, LD32(0x972a5c) ^ 1);
    ST32(0x972a3c, 0), ST32(0x972a40, 0), ST32(0x972a44, 0), ST32(0x972a48, 0);
    ST32(0x971f00, LD32(0x971f04));
    uint32_t a = LD32(0x975308);
    if (LD32(0x975314) == a) a = LD32(0x97530c);
    ST32(0x975314, a), ST32(0x975310, a);
    ST32(0x972908, LD32(0x972908) + 1), ST32(0x972904, LD32(0x972904) + 1);
    ST32(0x9758d0, 0), ST32(0x9758d4, 0);
}
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
volatile uint32_t g_free_gen;  /* anything the game freed: rt_vita.c game_free, f_59dca0 */
static uint64_t g_t_handoff, g_t_first, g_tick_us, g_first_us;  /* where the first entity matrix lands in the tick */
static uint64_t g_t_logic;  /* 55d780 (the logic tick) start, while the worker runs */
static uint32_t g_wframes;
static int pipe_mode(void) {
    if (g_pipe < 0) {
        const char* e = getenv("USM_PIPE");
        g_pipe = e ? atoi(e) : 3;  /* on unless pipe.txt / USM_PIPE says otherwise (0 = off) */
#ifdef USM_DEBUG
        if ((e = getenv("USM_PIPE_DELAY"))) g_wdelay = atoi(e);
#endif  /* vpc: us per worker draw, to be as slow as the Vita */
        FILE* f = fopen(USM_DIR "pipe.txt", "r");
        if (f) {
            int m;
            if (fscanf(f, "%d", &m) == 1) g_pipe = m;
            fclose(f);
        }
        /* pipe.txt 2 is what the Settings menu has always saved for "on": mode 3 unless overlap.txt says 0 */
        if (g_pipe == 2) g_pipe = 3;
        if ((f = fopen(USM_DIR "overlap.txt", "r"))) {
            int m = 1;
            if (fscanf(f, "%d", &m) == 1 && !m && g_pipe == 3) g_pipe = 2;
            fclose(f);
        }
#ifdef USM_DEBUG
        if ((e = getenv("USM_PIPE"))) g_pipe = atoi(e);  /* vpc: exactly this mode */
#endif
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
       host stack. EXEC only records commands (shaders compile on the render thread), so a smaller stack still works
       when memory is short; running without the worker costs about 10% of the frame rate (fast5 vs fast6) */
    pthread_t t;
    int err = g_wjob < 0 || g_wdone < 0 ? -1 : 1;
    for (int mb = 8; mb >= 2 && err > 0; mb /= 2) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, (unsigned)mb << 20);
        err = pthread_create(&t, &a, worker_main, NULL);
        if (!err) vlog("pipeline: worker has a %d MB stack\n", mb);
    }
    if (err) {
        vlog("pipeline: could not start the worker (semaphores %x %x, error %d), running without it\n", g_wjob, g_wdone, err);
        g_pipe = 0;
        return;
    }
    sceKernelWaitSema(g_wdone, 1, NULL);  /* its context is ready */
    vlog("pipeline: worker started\n");
}
/* main.c, before the game starts: vitaGL later takes all RAM but its reserve, and the game's threads and movies use
   that up, so a worker started at the first frame only sometimes got its stack (fast5 did, fast6 did not) */
void pipe_start_early(void) {
    if (pipe_mode() > 0 && !g_wc) pipe_start();
}
/* wait for the worker, then hand back what it produced. Only the thread that hands EXEC over takes the worker's
   "done" signal; any other thread just waits for it to finish */
static const char* g_why, *g_waited;  /* the fence being passed; the last one that made main wait (for the log) */
static Ctx* g_mc;
static struct { const char* why; uint32_t n, us; } g_fw[12];
/* shims.c: main's Release of a resource waited for the worker, 18-41 ms each and the game drops several at once (77b2f0, streaming):
   it waits here for the next fence instead, and until then the object simply stays alive. Off: release_later.txt containing 0 */
static uint32_t g_rel[256], g_reln;
int pipe_defer_release(Ctx* c, uint32_t o) {
    static int on = -1;
    if (on < 0) {
        FILE* f = fopen(USM_DIR "release_later.txt", "r");
        on = 1;
        if (f) {
            if (fscanf(f, "%d", &on) != 1) on = 1;
            fclose(f);
        }
    }
    if (!on || c != g_mc || g_reln == 256) return 0;
    g_rel[g_reln++] = o;
    return 1;
}
void pipe_fence(void) {
    if (!g_wbusy || rc_ctx() == g_wc) return;  /* the worker never waits for itself */
    if (rc_ctx() != g_mc) {
        while (g_wbusy) sceKernelDelayThread(100);
        return;
    }
    g_waited = g_why;
    uint64_t t = sceKernelGetProcessTimeWide();
    sceKernelWaitSema(g_wdone, 1, NULL);
    t = sceKernelGetProcessTimeWide() - t;
    g_waitus += t;
    for (int i = 0; i < 12; i++)  /* the log: what main waited at */
        if (!g_fw[i].why || g_fw[i].why == g_why) { g_fw[i].why = g_why, g_fw[i].n++, g_fw[i].us += (uint32_t)t; break; }
    g_wbusy = 0, g_fgen = 0;
    void game_free_late(void);
    game_free_late();
    void slab_free_late_(void);
    slab_free_late_();
    for (unsigned k = 0; k < rc_tls_n; k++)
        if (rc_tls_flag[k] & 2) memcpy((void*)RC_P(rc_tls_addr0[k]), (void*)RC_P(g_wtls[k]), rc_tls_size[k]);
    gl_set_recorder();
    void d3d_release_later(uint32_t o);
    for (uint32_t i = 0; i < g_reln; i++) d3d_release_later(g_rel[i]);
    g_reln = 0;
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
    pipe_after_handoff();
    memset(g_rows_m, 0, sizeof g_rows_m), memset(g_rows_w, 0, sizeof g_rows_w);
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
    if (g_wframes % 120 == 0 && log_on()) {
        static uint64_t last;
        uint64_t now = sceKernelGetProcessTimeWide();
        double wall = last ? (double)(now - last) / 1000.0 / 120 : 0, waited = 0;
        for (int i = 0; i < 12 && g_fw[i].why; i++) waited += g_fw[i].us / 1000.0 / 120;
        if (last) {
            vlog("main thread: %.1f ms/frame busy of %.1f (waits at:", wall - waited, wall);
            for (int i = 0; i < 12 && g_fw[i].why; i++) vlog(" %s %u x %.1f ms", g_fw[i].why, g_fw[i].n, g_fw[i].us / 1000.0 / g_fw[i].n);
            vlog(")\n");
        }
        last = now;
        memset(g_fw, 0, sizeof g_fw);
    }
    ret(c, 0);
}
#endif
/* 76e050 resets the scratch arena the render lists live in (base 0x971f08, size 0x971f10) while the worker may still
   be reading last frame's: each world frame takes the other of two blocks. Frame N's block is free again once EXEC(N) is
   done, which main waits for at the hand-off of frame N+1, before it gets back here */
#ifndef RC_WTRACE
static void arena_flip(void) {
    static uint32_t a, b;
    uint32_t base = LD32(0x971f08);
    if (base != a && base != b) {  /* first time, or the game made a new arena */
        a = base, b = G(guest_alloc(LD32(0x971f10)));
        vlog("pipeline: second scratch arena, %u KB\n", LD32(0x971f10) >> 10);
    }
    ST32(0x971f08, base == a ? b : a);
}
#endif
/* 73ea70: the world render frame; the pipeline's fence */
#ifdef RC_WTRACE
static int g_wt_build;  /* WT_BUILD set: the tracer's EXEC phase is the whole world frame (build + EXEC) */
#endif
void f_73ea70(Ctx* c) {
#ifdef RC_WTRACE
    if (!getenv("WT_OVERLAP")) rc_wt_fence();  /* WT_OVERLAP: trace EXEC against the next build too (fence moved to EXEC) */
    if (getenv("WT_BUILD")) {
        g_wt_build = 1, rc_wt_exec_begin(c);
        fo_73ea70(c);
        rc_wt_exec_end(), g_wt_build = 0;
        return;
    }
#else
    if (pipe_mode() > 0 && g_pipe < 3) g_why = "73ea70 (next world frame)", pipe_fence();
    d3d_frame_fixups();
#endif
    fo_73ea70(c);
}
/* 76e050: begin a render frame (resets the scratch arena the worker's lists live in) */
void f_76e050(Ctx* c) {
#ifndef RC_WTRACE
    if (g_pipe == 3 && LD32(c->esp) == 0x73eaad) arena_flip();  /* the world frame: EXEC may still read the last lists */
    else if (g_pipe > 0) g_why = "76e050 (a render frame begins)", pipe_fence();
#endif
    fo_76e050(c);
}
/* 52b250, near the end of the build: 619690 under it rewrites the geometry pointers of a set the worker may be drawing
   (406350). The hand-off's own fence comes soon after, so moving this one could only gain that short gap */
void f_52b250(Ctx* c) {
#ifndef RC_WTRACE
    if (g_pipe > 0) g_why = "52b250 (end of the build)", pipe_fence();  /* main here before the worker ends: worker-bound */
#endif
    fo_52b250(c);
}
/* 59dca0 (block, its page or 0): the game's small-block pool free, 417 callers. A freed slot goes back on the list at once
   and the next allocation of that size takes it, so an object the logic tick deletes (crowds, gang fights, mission ends)
   turned into something else while the worker drew frame N from it: a timer read as a texture pointer, 0x4b. malloc
   never sees these, so game_free's queue doesn't help. Frees wait here for the fence (GitHub issue 5). */
#ifndef RC_WTRACE
#define SLAB_LATE 16384
static uint32_t g_slab[SLAB_LATE][2];
static unsigned g_nslab;
static pthread_mutex_t g_slab_m = PTHREAD_MUTEX_INITIALIZER;
void f_59dca0(Ctx* c) {
    g_free_gen++;
    if (g_wbusy && arg(c, 1)) {
        pthread_mutex_lock(&g_slab_m);
        int kept = g_nslab < SLAB_LATE;
        if (kept) g_slab[g_nslab][0] = arg(c, 1), g_slab[g_nslab][1] = arg(c, 2), g_nslab++;
        pthread_mutex_unlock(&g_slab_m);
        if (kept) { ret(c, 0); return; }
        g_why = "59dca0 (pool free queue full)", pipe_fence();
    }
    fo_59dca0(c);
}
void slab_free_late_(void) {  /* pipe_fence: main, the worker idle */
    pthread_mutex_lock(&g_slab_m);
    unsigned n = g_nslab;
    g_nslab = 0;
    pthread_mutex_unlock(&g_slab_m);
    for (unsigned i = 0; i < n; i++) guest_call(0x59dca0, 2, g_slab[i]);
}
#else
void f_59dca0(Ctx* c) { g_free_gen++; fo_59dca0(c); }
#endif
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
/* Quit: the teardown from 5ad5a9 frees the whole world and never got done on the Vita (the old mixer watchdog closed the
   app 10 s later). Its first step deletes the game object: leave there, once the quit flag is up (a deletion without it
   runs as usual), and at 4c2a80, the next step, in case that was skipped. Nothing here saves anything: save games are
   written when saved, settings by reg_flush */
static void quit_now(void) {
    void reg_flush(void);
    reg_flush();
    vlog("quit: leaving\n");
    sceKernelExitProcess(0);
}
void f_5e99d0(Ctx* c) {
    /* only from the teardown's delete: 57b8b3 sets this flag outside quitting too */
    if (LD8(0x95c178) && LD32(c->esp) == 0x5ad5bb) quit_now();
    fo_5e99d0(c);
}
void f_4c2a80(Ctx* c) { (void)c; quit_now(); }
/* D3DX's pixel blit: every image the game loads from a file (the Options menu has 41: 11.8 s frozen on the Vita, 75% of it
   here), through a float per channel per pixel. 32-bit to 32-bit, same size or stretched up, is all the menus need:
   bilinear in fixed point, ~100x faster. dst/src: {bits, format, pitch, slice pitch, box left top right bottom front back,
   rect, ...}. Anything else (DXT sources, shrinking) stays with the game's code */
static uint32_t blit_mix(uint32_t a, uint32_t b, uint32_t f) {  /* f: 0..256 */
    uint32_t g = 256 - f;
    return (((a & 0xff00ffu) * g + (b & 0xff00ffu) * f) >> 8 & 0xff00ffu) | (((a >> 8 & 0xff00ffu) * g + (b >> 8 & 0xff00ffu) * f) & 0xff00ff00u);
}
static int blit_fast(uint32_t dst, uint32_t src, uint32_t flags, uint32_t* out, uint32_t pitch) {
    enum { PITCH = 2, L = 4, T = 5, R = 6, B = 7 };
    #define W_(d, i) LD32((d) + 4 * (i))
    static uint16_t xi[4096];
    static uint16_t xf[4096];
    uint32_t dw = W_(dst, R), dh = W_(dst, B), sw = W_(src, R), sh = W_(src, B), sp = W_(src, PITCH), sb = W_(src, 0);
    if (W_(dst, 1) != 21 || W_(src, 1) != 21 || (flags & 0xffff) != 3 || flags >> 16 || W_(dst, L) | W_(dst, T) | W_(src, L) | W_(src, T)) return 0;
    if (!dw || !dh || !sw || !sh || dw > 4096 || sw > dw || sh > dh || W_(dst, 3) | W_(src, 3)) return 0;
    for (uint32_t x = 0; x < dw; x++) {  /* source position of the pixel's centre */
        int32_t u = (int32_t)(((uint64_t)(2 * x + 1) * sw << 8) / (2 * dw)) - 128;
        if (u < 0) u = 0;
        xi[x] = (uint16_t)(u >> 8), xf[x] = (uint16_t)(u & 255);
        if (xi[x] >= sw - 1) xi[x] = (uint16_t)(sw - 1), xf[x] = 0;
    }
    for (uint32_t y = 0; y < dh; y++) {
        int32_t v = (int32_t)(((uint64_t)(2 * y + 1) * sh << 8) / (2 * dh)) - 128;
        if (v < 0) v = 0;
        uint32_t r0 = (uint32_t)v >> 8, fy = (uint32_t)v & 255;
        if (r0 >= sh - 1) r0 = sh - 1, fy = 0;
        const uint32_t* a = (const uint32_t*)RC_P(sb + r0 * sp), *b = (const uint32_t*)RC_P(sb + (fy ? r0 + 1 : r0) * sp);
        uint32_t* o = (uint32_t*)((uint8_t*)out + y * pitch);
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t i = xi[x], j = xf[x] ? i + 1 : i, f = xf[x];
            o[x] = blit_mix(blit_mix(a[i], a[j], f), blit_mix(b[i], b[j], f), fy);
        }
    }
    #undef W_
    return 1;
}
void f_7d36f3(Ctx* c) {
    uint32_t dst = LD32(c->esp + 4), src = LD32(c->esp + 8), flags = LD32(c->esp + 12), self = c->ecx;
    static int off = -1;
    if (off < 0) {
        FILE* f = fopen(USM_DIR "fast_blit.txt", "r");
        int v = 1;
        if (f) {
            if (fscanf(f, "%d", &v) != 1) v = 1;
            fclose(f);
        }
        off = !v;
    }
#ifdef USM_DEBUG
    if (getenv("VPC_BLIT_CHECK")) {  /* the game's result against ours: pixels off by more than 16 in a channel */
        static uint32_t tmp[4096 * 1024];
        int did = blit_fast(dst, src, flags, tmp, 4096 * 4);
        fo_7d36f3(c);
        if (did) {
            uint32_t w = LD32(dst + 24), h = LD32(dst + 28), bad = 0, worst = 0;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t p = LD32(LD32(dst) + y * LD32(dst + 8) + 4 * x), q = tmp[y * 4096 + x], m = 0;
                    for (int k = 0; k < 32; k += 8) {
                        int e = abs((int)(p >> k & 255) - (int)(q >> k & 255));
                        m = (uint32_t)e > m ? (uint32_t)e : m;
                    }
                    bad += m > 16, worst = m > worst ? m : worst;
                }
            vlog("blit check %ux%u: %u pixels off by more than 16, worst %u\n", w, h, bad, worst);
        }
        return;
    }
#endif
    if (!off && LD32(dst + 8) >= 4 * LD32(dst + 24) && blit_fast(dst, src, flags, (uint32_t*)RC_P(LD32(dst)), LD32(dst + 8))) {
        ST32(self, 0), ST32(self + 4, 0), ST32(self + 8, flags);
        c->eax = 0;
        ret(c, 12);
        return;
    }
    fo_7d36f3(c);
}
/* Smoke and fire: 75af90 builds every particle of an emitter into quads each frame, on the pipeline worker: 30-50% of its samples
   in a burning street (760aa0 and its 7 variants, vector math per particle), and main then waits for it at 52b250. An emitter with
   4 or more particles gets a private copy here that walks every smoke.txt-th one (default 2, 1 = all): count / step at +0x74, stride
   x step in a copy of its layout (+0x78, stride at +4). The particles the game simulates are not touched. The copy stands in
   for the emitter, so the axes the renderer computes into it (+0x20..+0x70) go back afterwards */
void f_75af90(Ctx* c) {
    static int step = -1;
    static uint32_t em_copy, lay_copy;
    if (step < 0) {
        step = 2;
        FILE* f = fopen(USM_DIR "smoke.txt", "r");
        if (f) {
            if (fscanf(f, "%d", &step) != 1 || step < 1) step = 2;
            fclose(f);
        }
        vlog("smoke: every %d of large emitters' particles (smoke.txt)\n", step);
    }
    uint32_t em = LD32(c->esp + 4), n = LD32(em + 0x74);
    static uint32_t calls, total, big, most;  /* the log: how particles are spread over emitters */
    static uint64_t told;
    calls++, total += n, big += n >= 4, most = n > most ? n : most;
    if (log_on() && calls >= 2000) {
        uint64_t now = sceKernelGetProcessTimeWide();
        if (now >= told) vlog("smoke: %u emitters, %u particles, largest %u, %u emitters with 4 or more\n", calls, total, most, big), told = now + 2000000;
        calls = total = big = most = 0;
    }
    if (step < 2 || n < 4) {
        fo_75af90(c);
        return;
    }
    if (!em_copy) em_copy = G(guest_alloc(0x100)), lay_copy = G(guest_alloc(0x40));
    uint32_t lay = LD32(em + 0x78);
    memcpy((void*)RC_P(em_copy), (const void*)RC_P(em), 0x100);
    memcpy((void*)RC_P(lay_copy), (const void*)RC_P(lay), 0x40);
    ST32(em_copy + 0x74, n / (uint32_t)step);
    ST32(em_copy + 0x78, lay_copy);
    ST32(lay_copy + 4, LD32(lay + 4) * (uint32_t)step);
    ST32(c->esp + 4, em_copy);
    fo_75af90(c);
    memcpy((void*)RC_P(em + 0x20), (const void*)RC_P(em_copy + 0x20), 0x50);
}
/* 771e40 copies a mesh's master buffers over the next slot of its ring, every tick (4d2060: morphing faces and the like, 21% of the main
   thread in that profile). Its Locks waited for the worker, which draws from the previous slot, never this one */
static volatile int g_ring_copy;
int pipe_ring_copy(Ctx* c) { return g_ring_copy && c == g_mc; }
void f_771e40(Ctx* c) {
    g_ring_copy = c == g_mc;
    fo_771e40(c);
    g_ring_copy = 0;
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
static int g_slow;  /* logic ticks running long, 0..16: see f_4a6110 */
static uint32_t g_tick;  /* logic ticks */
/* 6bc150: the entity nearest the HERO, a search of the collision grid around him (522c40 -> 5229b0, 3.3% self, the hottest
   real game code on main). 6d9740 calls it once per car, every tick, with no car in it: every car but the first repeated
   the same search (5.3% of the mission profile, and in the 250 ms hitch stacks). The answer is a pointer, so a free
   since the search redoes it: a car may have deleted that entity. */
void f_6bc150(Ctx* c) {
    static uint32_t tick = ~0u, gen, r[3];
    if (tick == g_tick && gen == g_free_gen) {
        c->eax = r[0], c->ecx = r[1], c->edx = r[2];
        ret(c, 0);
        return;
    }
    fo_6bc150(c);
    tick = g_tick, gen = g_free_gen, r[0] = c->eax, r[1] = c->ecx, r[2] = c->edx;
}
void f_55d780(Ctx* c) {
    g_tick++;
#ifndef RC_WTRACE
    static uint64_t last;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    if (t0 - last > 50000) { if (g_slow < 16) g_slow++; } else if (g_slow) g_slow--;
    last = t0;
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
float density_cap(void);
void f_6c2e10(Ctx* c) {  /* cdecl (float) */
    float cap = density_cap(), d;
    uint32_t a = c->esp + 4;
    memcpy(&d, (void*)RC_P(a), 4);
    if (d > cap) STF32(a, cap);
    if (LDF32(0x937ff8) > cap) STF32(0x937ff8, cap);
    fo_6c2e10(c);
}

/* 81e430: stereo IMA ADPCM, a nibble per channel, called on the game thread for streamed sound (~2.4% of it): (out,
   in, samples, predictors[2], step indices[2]). The registers it leaves are nothing a cdecl caller may use */
static void n_81e430(Ctx* c) {
    uint32_t n = arg(c, 3);
    if ((int32_t)n > 0) {
        const uint8_t* in = (const uint8_t*)RC_P(arg(c, 2));
        uint8_t* out = (uint8_t*)RC_P(arg(c, 1));
        rc_u32* pred = (rc_u32*)RC_P(arg(c, 4)), *idx = (rc_u32*)RC_P(arg(c, 5));
        const rc_u32* steps = (const rc_u32*)RC_P(0x9551f0), *adj = (const rc_u32*)RC_P(0x9551d0);
        int32_t ch[2][2] = {{(int32_t)pred[0], (int32_t)idx[0]}, {(int32_t)pred[1], (int32_t)idx[1]}};
        for (uint32_t i = 0; i < n; i++, in++, out += 4)
            for (int k = 0; k < 2; k++) {
                int32_t nib = k ? *in >> 4 : *in & 15, m = nib & 7, step = (int32_t)steps[ch[k][1]];
                int32_t x = ch[k][1] + (int32_t)adj[m];
                ch[k][1] = x < 0 ? 0 : x > 0x58 ? 0x58 : x;
                int32_t d = step >> 3;
                if (m & 4) d += step;
                if (m & 2) d += step >> 1;
                if (m & 1) d += step >> 2;
                int32_t v = nib & 8 ? ch[k][0] - d : ch[k][0] + d;
                ch[k][0] = v > 0x7fff ? 0x7fff : v < -0x8000 ? -0x8000 : v;
                out[2 * k] = (uint8_t)ch[k][0], out[2 * k + 1] = (uint8_t)(ch[k][0] >> 8);
            }
        pred[0] = (uint32_t)ch[0][0], idx[0] = (uint32_t)ch[0][1], pred[1] = (uint32_t)ch[1][0], idx[1] = (uint32_t)ch[1][1];
    }
    ret(c, 0);
}
void f_81e430(Ctx* c) { n_81e430(c); }
/* 81e320: the same decode for one channel (out, in, n, pred, index) */
static void n_81e320(Ctx* c) {
    int32_t n = (int32_t)arg(c, 3);
    if (n > 0) {
        const uint8_t* in = (const uint8_t*)RC_P(arg(c, 2));
        uint8_t* out = (uint8_t*)RC_P(arg(c, 1));
        rc_u32* pp = (rc_u32*)RC_P(arg(c, 4)), *ip = (rc_u32*)RC_P(arg(c, 5));
        const rc_u32* steps = (const rc_u32*)RC_P(0x9551f0), *adj = (const rc_u32*)RC_P(0x9551d0);
        int32_t pred = (int32_t)*pp, idx = (int32_t)*ip;
        for (int32_t i = 0; i < n; i++, out += 2) {
            int32_t nib = i & 1 ? *in++ >> 4 : *in & 15, m = nib & 7, step = (int32_t)steps[idx];
            int32_t x = idx + (int32_t)adj[m];
            idx = x < 0 ? 0 : x > 0x58 ? 0x58 : x;
            int32_t d = step >> 3;
            if (m & 4) d += step;
            if (m & 2) d += step >> 1;
            if (m & 1) d += step >> 2;
            int32_t v = nib & 8 ? pred - d : pred + d;
            pred = v > 0x7fff ? 0x7fff : v < -0x8000 ? -0x8000 : v;
            out[0] = (uint8_t)pred, out[1] = (uint8_t)(pred >> 8);
        }
        *pp = (uint32_t)pred, *ip = (uint32_t)idx;
    }
    ret(c, 0);
}
void f_81e320(Ctx* c) { n_81e320(c); }

/* The game's state wrappers, ~2,700 calls a frame on the worker, 85% of them setting what is already set. Then nothing
   changes, so skip the wrapper's guest calls and the shim dispatch: same result, same registers left. A real change takes
   the original. Only with no second D3D user: main may have to wait for the worker first, which the shim does. */
void f_772250(Ctx* c) {  /* SetPixelShader(*arg) */
    uint32_t ps = LD32(arg(c, 1));
    if (st.ps != ps || (g_wbusy && c != g_wc)) { fo_772250(c); return; }
    c->eax = 0, c->ecx = LD32(LD32(0x971f94)), c->edx = ps;
    ret(c, 0);
}
void f_772270(Ctx* c) {  /* SetVertexDeclaration(arg[1]), SetVertexShader(arg[0]) */
    uint32_t a = arg(c, 1), vs = LD32(a), decl = LD32(a + 4);
    if (st.vs != vs || st.decl != decl || st.fvf || (g_wbusy && c != g_wc)) { fo_772270(c); return; }
    c->eax = 0, c->ecx = LD32(LD32(0x971f94)), c->edx = vs;
    ret(c, 0);
}
void f_76dc30(Ctx* c) {  /* SetSamplerState(stage, type, value) through the game's own cache at 0x971ff0 */
    uint32_t s = arg(c, 1), t = arg(c, 2), k = s * 14 + t;
    if (LD32(0x971ff0 + 4 * k) != arg(c, 3)) { fo_76dc30(c); return; }
    c->eax = k, c->ecx = s, c->edx = t;
    ret(c, 0);
}
/* 401110: cdecl (out, a, b): out = a + b, the 4 lanes */
void f_401110(Ctx* c) {
    uint32_t out = arg(c, 1), a = arg(c, 2), b = arg(c, 3);
    float x[4], y[4];
    ldv(x, a), ldv(y, b);
    for (int i = 0; i < 4; i++) x[i] += y[i];
    stv(out, x);
    c->eax = out, c->ecx = b;
    ret(c, 0);
}
#ifdef USM_DEBUG
/* vpc, USM_ADPCM_CHECK: random blocks through the translation and the native version: same samples and predictors */
void adpcm_selftest(void) {
    if (!getenv("USM_ADPCM_CHECK")) return;
    enum { N = 4096 };
    Ctx* c = rc_ctx();
    uint32_t in = G(guest_alloc(N)), out = G(guest_alloc(N * 4)), st = G(guest_alloc(16)), sp = c->esp, bad = 0, r[N + 4];
    for (int t = 0; t < 2000; t++) {
        uint32_t x = (uint32_t)rand() * 2654435761u, st0[4] = {(uint16_t)x - 32768u + 0u, (uint32_t)rand() % 65536 - 32768, rand() % 89u, rand() % 89u}, n = 1 + rand() % N;
        for (uint32_t i = 0; i < n; i++) ST8(in + i, rand());
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < 4; i++) ST32(st + 4 * i, st0[i]);
            memset((void*)RC_P(out), 0, N * 4);
            c->esp = sp - 32;
            uint32_t a[6] = {0, out, in, n, st, st + 8};
            for (int i = 0; i < 6; i++) ST32(c->esp + 4 * i, a[i]);
            if (t & 1) (pass ? n_81e320 : fo_81e320)(c);
            else (pass ? n_81e430 : fo_81e430)(c);
            if (!pass) { memcpy(r, (void*)RC_P(out), N * 4); memcpy(r + N, (void*)RC_P(st), 16); }
        }
        bad += memcmp(r, (void*)RC_P(out), N * 4) || memcmp(r + N, (void*)RC_P(st), 16);
    }
    c->esp = sp;
    vlog("adpcm selftest: 2000 blocks, %u differ\n", bad);
}
#endif

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
            float d2 = dx * dx + dy * dy + dz * dz;
            /* a crowd (gang fights, ~1.5 min after a mission: 6 fps with 21% of the frame here, prof6): while ticks run
               long, half rate from a third of the distance and quarter rate past it. Set off by g_slow, so a calm
               street stays as it was; the hero never steps late */
            int busy = g_slow >= 8;
            uint32_t mask = busy && d2 > far * far ? 3 : 1;
            if (busy ? d2 > far * far / 9 : d2 > far * far) {
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
                                          sqrtf(d2), hero);
                if (((self >> 4) + d3d_frame_count()) & mask) {  /* this tick: skipped, its time kept */
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
