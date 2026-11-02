/* Runtime for the translated code on the Vita (the rt.h interface, without native code to bridge to): per-thread
   contexts, calls through pointers, shim dispatch, host->guest calls, setjmp/longjmp, traps and the log. */
#define RC_GUEST_WRITER  /* sets rc_guest, see rt.h */
#include <malloc.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "vita.h"

uint8_t* rc_guest;
void (*rc_on_trap)(uint32_t at);

static FILE* g_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

/* log.txt (and the speed report) only when ux0:data/usm/debug.txt exists; vpc always logs */
int log_on(void) {
    static int on = -1;
    if (on < 0) {
#if defined(USM_DEBUG) || defined(RC_PROFILE)
        on = 1;
#else
        FILE* f = fopen(USM_DIR "debug.txt", "r");
        on = f != NULL;
        if (f) fclose(f);
#endif
    }
    return on;
}

void vlog(const char* fmt, ...) {
    if (!log_on()) return;
    pthread_mutex_lock(&g_log_lock);
    if (!g_log) g_log = fopen(USM_DIR "log.txt", "w");
    if (g_log) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_log_lock);
}

void rc_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vlog("%s", buf);
}

void negative_guest(uint32_t a) {
    static int told;
    if (!told) told = 1, vlog("warning: allocation below the image (guest address %08x)\n", a);
}

uint32_t g_game_bytes;
void* game_mem(void* p) {
    if (p) __atomic_add_fetch(&g_game_bytes, (uint32_t)malloc_usable_size(p), __ATOMIC_RELAXED);
    return p;
}
void* game_realloc(void* p, uint32_t n) {
    uint32_t old = p ? (uint32_t)malloc_usable_size(p) : 0;
    void* q = realloc(p, n);
    if (q || !n) __atomic_sub_fetch(&g_game_bytes, old, __ATOMIC_RELAXED);  /* failed: p is still held */
    return game_mem(q);
}
void game_free(void* p) {
    if (p) __atomic_sub_fetch(&g_game_bytes, (uint32_t)malloc_usable_size(p), __ATOMIC_RELAXED), free(p);
}

void* guest_alloc(uint32_t n) {
    void* p = memalign(16, n);
    if (!p) { vlog("out of memory (%u bytes)\n", n); abort(); }
    memset(p, 0, n);
    GA(p);
    return p;
}

/* ---- contexts ---- */
static pthread_key_t g_ctx_key;
static pthread_once_t g_ctx_once = PTHREAD_ONCE_INIT;
static void ctx_key(void) { pthread_key_create(&g_ctx_key, NULL); }

/* A thread's guest side: registers, a guest stack and a TEB for fs:. The C code runs on the thread's own stack. */
Ctx* ctx_new(uint32_t stack_size) {
    pthread_once(&g_ctx_once, ctx_key);
    Ctx* c = guest_alloc(sizeof(Ctx));
    uint8_t* stack = guest_alloc(stack_size);
    uint8_t* teb = guest_alloc(0x1000);
    uint8_t* peb = guest_alloc(0x1000);
    c->esp = G(stack + stack_size - 64);
    c->fsbase = G(teb);
    ST32(c->fsbase + 0x00, 0xffffffffu);            /* SEH chain: empty */
    ST32(c->fsbase + 0x04, G(stack + stack_size));  /* stack base */
    ST32(c->fsbase + 0x08, G(stack));               /* stack limit */
    ST32(c->fsbase + 0x18, c->fsbase);              /* self */
    ST32(c->fsbase + 0x30, G(peb));
    rc_fldcw(c, 0x27f);                             /* the Windows default control word */
    rc_tls_init(c);
    pthread_setspecific(g_ctx_key, c);
    return c;
}

Ctx* rc_ctx(void) {
    pthread_once(&g_ctx_once, ctx_key);
    Ctx* c = pthread_getspecific(g_ctx_key);
    return c ? c : ctx_new(0x100000);
}

#ifdef RC_RETCHK
/* recomp.py RC_RETCHK: a function changed a register its register flow says it keeps (each one logged once) */
void rc_retbad(uint32_t fn, const char* reg, uint32_t local, uint32_t ctx) {
    static uint32_t seen[256];
    static unsigned n;
    for (unsigned i = 0; i < n; i++)
        if (seen[i] == (fn ^ (uint32_t)reg[1] << 24)) return;
    if (n < 256) seen[n++] = fn ^ (uint32_t)reg[1] << 24;
    vlog("register flow: %06x changed %s (%08x, Ctx %08x)\n", fn, reg, local, ctx);
}
#endif

/* ---- traps ---- */
void rc_trap(Ctx* c, uint32_t at, const char* why) {
    vlog("trap at %06x: %s\n  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n  stack:", at, why,
         c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    for (int i = 0; i < 32; i++) {
        uint32_t v = LD32(c->esp + 4 * i);
        if (v >= 0x401000 && v < 0x870000) vlog(" %06x", v);
    }
    vlog("\n");
    shim_log_recent(8);
    if (rc_on_trap) rc_on_trap(at);
    abort();
}

void rc_bad_return(Ctx* c, uint32_t at, uint32_t expected) {
    vlog("bad return from %06x: expected %06x, got %06x\n", at, expected, c->target);
}

/* ---- calls ---- */
static unsigned index_of(uint32_t a) {
    unsigned lo = 0, hi = rc_count;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (rc_addr[m] < a) lo = m + 1;
        else hi = m;
    }
    return lo < rc_count && rc_addr[lo] == a ? lo + 1 : 0;
}

/* calls through pointers are hot: a direct-mapped cache in front of the binary search. One word per entry (table
   index + 1, checked against rc_addr), so threads never see half of an update. */
static uint32_t cache_idx[8192];

int gl_rl_before(uint32_t entry, uint32_t fn, uint32_t* mark);
void gl_rl_after(uint32_t entry, uint32_t fn, const uint32_t* mark);

static rc_fn lookup(uint32_t a) {
    if (a >= SHIM_BASE) return NULL;  /* imports and COM methods: never cached, so they skipped to the binary search */
    unsigned h = (a >> 4 ^ a >> 17) & 8191;
    uint32_t k = __atomic_load_n(&cache_idx[h], __ATOMIC_RELAXED);
    if (k && rc_addr[k - 1] == a) return rc_impl[k - 1];
    k = index_of(a);
    if (!k) return NULL;
    __atomic_store_n(&cache_idx[h], k, __ATOMIC_RELAXED);
    return rc_impl[k - 1];
}

/* Everything outside the translation is a shim; anything else is code nobody translated, and stops the game. */
void rc_native(Ctx* c, uint32_t target) {
    if (!shim_call(c, target)) rc_trap(c, target, "call to code that is neither translated nor a shim");
}

void rc_call(Ctx* c, uint32_t target, uint32_t ret) {
    rc_fn f = lookup(target);
    if (!f) { rc_native(c, target); return; }
#ifdef USM_VITAGL
    /* the render lists' entry callbacks (77e190 / 77e220, ecx = the entry): gl.c's early cull */
    int rl = ret == 0x77e206u || ret == 0x77e296u;
    uint32_t mark[4], entry = c->ecx;
    if (rl && gl_rl_before(entry, target, mark)) return;
#endif
    c->esp -= 4;
    ST32(c->esp, ret);
#ifdef RC_PROFILE
    rc_prof_push(c, target);
    f(c);
    rc_prof_pop(c);
#else
    f(c);
#endif
#ifdef RC_RETCHK
    if ((c->eax & 0xffffff00u) == 0x0bad0000u) {  /* the callee left eax as the caller's poison: it never wrote eax back */
        static uint32_t seen[64];
        static unsigned n;
        unsigned i = 0;
        while (i < n && seen[i] != target) i++;
        if (i == n && n < 64) seen[n++] = target, vlog("register flow: %06x (called through a pointer from %06x) left eax unwritten\n", target, ret);
    }
#endif
#ifdef USM_VITAGL
    if (rl) gl_rl_after(entry, target, mark);
#endif
}

#ifdef RC_PROFILE
/* The sampler (profile builds): 1000 times a second it reads the game loop thread's shadow call stack, counting the
   innermost function (self) and every function on it once (inclusive), and every 10 s logs the top of both. */
typedef struct {
    uint32_t key, self, incl, stamp;
} ProfEnt;
#define PROF_N 16384
static ProfEnt g_prof[PROF_N], g_wprof[PROF_N];  /* the game loop thread; the render pipeline's worker (native.c) */
static Ctx* g_prof_ctx;
static Ctx* volatile g_prof_wctx;
void rc_prof_worker(Ctx* c) { g_prof_wctx = c; }
static ProfEnt* prof_ent_in(ProfEnt* t, uint32_t key) {
    for (uint32_t i = (key * 2654435761u) >> 18, n = 0; n < PROF_N; i = (i + 1) & (PROF_N - 1), n++)
        if (t[i].key == key || !t[i].key) return t[i].key = key, &t[i];
    return NULL;
}
static ProfEnt* prof_ent(uint32_t key) { return prof_ent_in(g_prof, key); }
static uint32_t prof_addr(uint32_t key) { return key < rc_count ? rc_addr[key] : key; }
/* caller -> callee edges (inclusive samples), so the call tree can be rebuilt offline: profile_edges.txt */
typedef struct { uint32_t from, to, incl, stamp; } ProfEdge;
static ProfEdge g_edge[PROF_N];
static ProfEdge* prof_edge(uint32_t from, uint32_t to) {
    for (uint32_t i = ((from * 2654435761u) ^ (to * 40503u)) >> 18, n = 0; n < PROF_N; i = (i + 1) & (PROF_N - 1), n++)
        if ((g_edge[i].from == from && g_edge[i].to == to) || !g_edge[i].incl)
            return g_edge[i].from = from, g_edge[i].to = to, &g_edge[i];
    return NULL;
}
static int by_self(const void* a, const void* b) { return (int)((const ProfEnt*)b)->self - (int)((const ProfEnt*)a)->self; }
static int by_incl(const void* a, const void* b) { return (int)((const ProfEnt*)b)->incl - (int)((const ProfEnt*)a)->incl; }
/* the worker's samples (while it has a call stack): its own top 25 and profile_worker.txt */
static void prof_dump_worker(uint32_t samples, uint32_t busy) {
    static ProfEnt s[PROF_N];
    uint32_t n = 0;
    for (int i = 0; i < PROF_N; i++)
        if (g_wprof[i].key) s[n++] = g_wprof[i];
    if (!busy) return;
    vlog("profile worker: busy in %.1f%% of %u samples\n", 100.0 * busy / samples, samples);
    qsort(s, n, sizeof *s, by_self);
    for (uint32_t i = 0; i < n && i < 25; i++)
        vlog("profile worker self %5.1f%%  incl %5.1f%%  %06x\n", 100.0 * s[i].self / busy, 100.0 * s[i].incl / busy, prof_addr(s[i].key));
    FILE* f = fopen(USM_DIR "profile_worker.txt", "w");
    if (f) {
        fprintf(f, "# %u busy samples: incl%% self%% address\n", busy);
        for (uint32_t i = 0; i < n; i++)
            fprintf(f, "%6.2f %6.2f %06x\n", 100.0 * s[i].incl / busy, 100.0 * s[i].self / busy, prof_addr(s[i].key));
        fclose(f);
    }
}
static void prof_dump(uint32_t samples, uint32_t outside) {
    static ProfEnt s[PROF_N];
    uint32_t n = 0;
    for (int i = 0; i < PROF_N; i++)
        if (g_prof[i].key) s[n++] = g_prof[i];
    vlog("profile: %u samples, %.1f%% outside any call\n", samples, 100.0 * outside / samples);
    qsort(s, n, sizeof *s, by_self);
    for (uint32_t i = 0; i < n && i < 25; i++)
        vlog("profile self %5.1f%%  incl %5.1f%%  %06x\n", 100.0 * s[i].self / samples, 100.0 * s[i].incl / samples, prof_addr(s[i].key));
    qsort(s, n, sizeof *s, by_incl);
    for (uint32_t i = 0; i < n && i < 25; i++)
        vlog("profile incl %5.1f%%  self %5.1f%%  %06x\n", 100.0 * s[i].incl / samples, 100.0 * s[i].self / samples, prof_addr(s[i].key));
    FILE* f = fopen(USM_DIR "profile.txt", "w");  /* every function seen, for tools/recomp/prof_names.py */
    if (f) {
        fprintf(f, "# %u samples: incl%% self%% address\n", samples);
        for (uint32_t i = 0; i < n; i++)
            fprintf(f, "%6.2f %6.2f %06x\n", 100.0 * s[i].incl / samples, 100.0 * s[i].self / samples, prof_addr(s[i].key));
        fclose(f);
    }
    if ((f = fopen(USM_DIR "profile_edges.txt", "w"))) {
        fprintf(f, "# %u samples: incl%% caller callee\n", samples);
        for (int i = 0; i < PROF_N; i++)
            if (g_edge[i].incl)
                fprintf(f, "%6.2f %06x %06x\n", 100.0 * g_edge[i].incl / samples, prof_addr(g_edge[i].from), prof_addr(g_edge[i].to));
        fclose(f);
    }
}
static int prof_main(SceSize args, void* argp) {
    (void)args, (void)argp;
    uint32_t samples = 0, outside = 0, wbusy = 0;
    for (;;) {
        sceKernelDelayThread(1000);
        Ctx* w = g_prof_wctx;
        uint32_t wd = w ? w->pdepth : 0;
        if (wd > 256) wd = 256;
        if (wd) wbusy++;
        for (uint32_t i = 0; i < wd; i++) {
            ProfEnt* e = prof_ent_in(g_wprof, w->pstack[i] ? w->pstack[i] : 0xffffffffu);
            if (!e) continue;
            if (e->stamp != samples + 1) e->stamp = samples + 1, e->incl++;
            if (i == wd - 1) e->self++;
        }
        Ctx* c = g_prof_ctx;
        uint32_t d = c->pdepth;
        if (d > 256) d = 256;
        samples++;
        if (!d) outside++;
        for (uint32_t i = 0; i < d; i++) {
            ProfEnt* e = prof_ent(c->pstack[i] ? c->pstack[i] : 0xffffffffu);
            if (!e) continue;
            if (e->stamp != samples) e->stamp = samples, e->incl++;
            if (i == d - 1) e->self++;
            if (i) {
                ProfEdge* g = prof_edge(c->pstack[i - 1], c->pstack[i]);
                if (g && g->stamp != samples) g->stamp = samples, g->incl++;
            }
        }
        {   /* a hitch: no frame for 250 ms. Log where the game loop thread is, every 250 ms of it, up to 4 times */
            extern uint32_t d3d_frame_count(void);
            static uint32_t last_frame, still;
            uint32_t fr = d3d_frame_count();
            if (fr != last_frame) last_frame = fr, still = 0;
            else if (++still % 250 == 0 && still <= 1000 && fr > 100) {
                char line[400];
                int o = snprintf(line, sizeof line, "hitch %u ms at frame %u, innermost first:", still, fr);
                for (int i = (int)d - 1; i >= 0 && i >= (int)d - 14 && o < (int)sizeof line - 8; i--)
                    o += snprintf(line + o, sizeof line - o, " %06x", prof_addr(c->pstack[i]));
                vlog("%s\n", line);
            }
        }
        if (samples == 10000) {
            extern uint32_t d3d_frame_count(void);
            extern void shim_dump(uint32_t frames);
            static uint32_t frames0;
            uint32_t frames = d3d_frame_count();
            shim_dump(frames - frames0), frames0 = frames;
        }
        if (samples == 10000)
            prof_dump(samples, outside), prof_dump_worker(samples, wbusy), memset(g_prof, 0, sizeof g_prof),
                memset(g_wprof, 0, sizeof g_wprof), memset(g_edge, 0, sizeof g_edge), samples = outside = wbusy = 0;
    }
    return 0;
}
void rc_prof_start(Ctx* main_ctx) {
    g_prof_ctx = main_ctx;
    SceUID t = sceKernelCreateThread("usm_prof", prof_main, 0x10000100, 0x10000, 0, 0x80000, NULL);  /* core 3 */
    if (t < 0) t = sceKernelCreateThread("usm_prof", prof_main, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
    vlog("profiler on: sampling the game loop thread\n");
}
#endif

void rc_tail(Ctx* c, uint32_t target) {
    rc_fn f = lookup(target);
    if (f) { f(c); return; }
    uint32_t ra = LD32(c->esp);
    c->esp += 4;
    rc_native(c, target);
    c->target = ra;
}

void rc_jmp(Ctx* c, uint32_t target, uint32_t at) {
    (void)at;
    rc_tail(c, target);
}

#define SENTINEL 0xfffffff0u

uint32_t guest_call(uint32_t fn, int n, const uint32_t* args) {
    Ctx* c = rc_ctx();
    uint32_t saved[8];
    memcpy(saved, &c->eax, sizeof saved);
    for (int i = n - 1; i >= 0; i--) c->esp -= 4, ST32(c->esp, args[i]);
    rc_fn f = lookup(fn);
    if (f) {
        c->esp -= 4;
        ST32(c->esp, SENTINEL);
        f(c);
    } else if (!shim_call(c, fn)) {
        rc_trap(c, fn, "callback to code that is neither translated nor a shim");
    }
    uint32_t r = c->eax;
    memcpy(&c->eax, saved, sizeof saved);  /* esp too: cdecl or stdcall, the caller's stack is as it was */
    return r;
}

/* ---- setjmp/longjmp (see rt.h) ---- */
#define NJMP 32

rc_jmprec* rc_setjmp_rec(Ctx* c, uint32_t jb) {
    if (!c->jmp) c->jmp = guest_alloc(sizeof(rc_jmprec) * NJMP);
    rc_jmprec* free_ = NULL;
    for (int i = 0; i < NJMP; i++) {
        if (c->jmp[i].jb == jb) return &c->jmp[i];
        if (!c->jmp[i].jb && !free_) free_ = &c->jmp[i];
    }
    if (!free_) free_ = &c->jmp[jb % NJMP];
    free_->jb = jb;
    return free_;
}

void rc_setjmp_save(Ctx* c, rc_jmprec* r) {
    memcpy(r->regs, &c->eax, 32);
    r->fs0 = LD32(c->fsbase);
}

int rc_longjmp(uint32_t jb, int v) {
    Ctx* c = rc_ctx();
    if (!c->jmp) return 0;
    for (int i = 0; i < NJMP; i++) {
        rc_jmprec* r = &c->jmp[i];
        if (r->jb != jb || r->regs[4] < c->esp) continue;  /* below esp: its setjmp frame has returned */
        memcpy(&c->eax, r->regs, 32);
        c->eax = v ? (uint32_t)v : 1;
        c->ftop = 0, c->fdepth = 0;
        ST32(c->fsbase, r->fs0);
        __builtin_longjmp(r->buf, 1);
    }
    return 0;
}
