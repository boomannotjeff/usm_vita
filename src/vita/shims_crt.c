/* MSVCR71 (cdecl, so no pops). Guest pointers become host pointers with H(); host memory handed to the game goes back
   through G(). The heap is newlib's malloc: its blocks are valid guest addresses (see vita.h). */
#include <ctype.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "vita.h"
#include "vita_util.h"

#define S(i) ((char*)H(ARG(i)))

/* ---- startup ---- */
static uint32_t g_fmode, g_commode, g_cw = 0x9001f;  /* _controlfp's view: 53-bit, round to nearest, all masked */
static void __set_app_type(Ctx* c) { (void)c; }
static void __p__fmode(Ctx* c) { if (!g_fmode) g_fmode = G(guest_alloc(4)); c->eax = g_fmode; }
static void __p__commode(Ctx* c) { if (!g_commode) g_commode = G(guest_alloc(4)); c->eax = g_commode; }

/* _controlfp(new, mask): only rounding (MCW_RC 0x300) reaches the FPU; precision control isn't applied (fpu_port.c) */
static void _controlfp(Ctx* c) {
    uint32_t nw = ARG(0), mask = ARG(1);
    g_cw = (g_cw & ~mask) | (nw & mask);
    static const uint16_t rc[4] = {0x000, 0x400, 0x800, 0xc00};  /* near, down, up, chop: _RC_* -> x87 RC */
    rc_fldcw(c, (uint16_t)((c->fcw & ~0xc00) | rc[(g_cw >> 8) & 3]));
    c->eax = g_cw;
}

static void _initterm(Ctx* c) {
    for (uint32_t p = ARG(0), end = ARG(1); p < end; p += 4)
        if (LD32(p)) guest_call(LD32(p), 0, NULL);
}

static uint32_t g_onexit[512];
static unsigned g_nonexit;
static void _onexit(Ctx* c) {
    if (g_nonexit < 512) g_onexit[g_nonexit++] = ARG(0);
    c->eax = ARG(0);
}
void crt_run_onexit(void) {
    while (g_nonexit) guest_call(g_onexit[--g_nonexit], 0, NULL);
}

static void __getmainargs(Ctx* c) {
    uint32_t* argv = guest_alloc(16);
    argv[0] = G(gstrdup("C:\\USM\\USM.exe"));
    ST32(ARG(0), 1);
    ST32(ARG(1), G(argv));
    ST32(ARG(2), G(argv + 2));  /* empty environment */
    c->eax = 0;
}

static void exit_(Ctx* c) {
    vlog("exit(%d)\n", (int)ARG(0));
    crt_run_onexit();
    exit((int)ARG(0));
}

/* ---- strings ---- */
static void _strcmpi(Ctx* c) { c->eax = (uint32_t)strcasecmp(S(0), S(1)); }
static void _strnicmp(Ctx* c) { c->eax = (uint32_t)strncasecmp(S(0), S(1), ARG(2)); }
static void strcmp_(Ctx* c) { c->eax = (uint32_t)strcmp(S(0), S(1)); }
static void strncmp_(Ctx* c) { c->eax = (uint32_t)strncmp(S(0), S(1), ARG(2)); }
static void strncpy_(Ctx* c) { strncpy(S(0), S(1), ARG(2)); c->eax = ARG(0); }
static void strncat_(Ctx* c) { strncat(S(0), S(1), ARG(2)); c->eax = ARG(0); }
static void strstr_(Ctx* c) { char* r = strstr(S(0), S(1)); c->eax = r ? G(r) : 0; }
static void strchr_(Ctx* c) { char* r = strchr(S(0), (int)ARG(1)); c->eax = r ? G(r) : 0; }
static void strrchr_(Ctx* c) { char* r = strrchr(S(0), (int)ARG(1)); c->eax = r ? G(r) : 0; }
static void _strlwr(Ctx* c) { for (char* p = S(0); *p; p++) *p = (char)tolower((unsigned char)*p); c->eax = ARG(0); }
static void _strupr(Ctx* c) { for (char* p = S(0); *p; p++) *p = (char)toupper((unsigned char)*p); c->eax = ARG(0); }
static void _strdup(Ctx* c) { c->eax = G(gstrdup(S(0))); }
static void strtok_(Ctx* c) {  /* the CRT keeps its position per thread; the game only tokenizes on one at a time */
    static char* pos;
    char* r = strtok_r(ARG(0) ? S(0) : NULL, S(1), &pos);
    c->eax = r ? G(r) : 0;
}
static void atoi_(Ctx* c) { c->eax = (uint32_t)atoi(S(0)); }
static void atol_(Ctx* c) { c->eax = (uint32_t)atol(S(0)); }
static void atof_(Ctx* c) { FPUSH(atof(S(0))); }
static void strtol_(Ctx* c) {
    char* end;
    c->eax = (uint32_t)strtol(S(0), ARG(1) ? &end : NULL, (int)ARG(2));
    if (ARG(1)) ST32(ARG(1), G(end));
}
static void strtod_(Ctx* c) {
    char* end;
    FPUSH(strtod(S(0), &end));
    if (ARG(1)) ST32(ARG(1), G(end));
}
static void isalpha_(Ctx* c) { c->eax = isalpha((int)ARG(0)) != 0; }
static void isdigit_(Ctx* c) { c->eax = isdigit((int)ARG(0)) != 0; }
static void isspace_(Ctx* c) { c->eax = isspace((int)ARG(0)) != 0; }
static void isalnum_(Ctx* c) { c->eax = isalnum((int)ARG(0)) != 0; }
static void isupper_(Ctx* c) { c->eax = isupper((int)ARG(0)) != 0; }
static void islower_(Ctx* c) { c->eax = islower((int)ARG(0)) != 0; }
static void isprint_(Ctx* c) { c->eax = isprint((int)ARG(0)) != 0; }
static void tolower_(Ctx* c) { c->eax = (uint32_t)tolower((int)ARG(0)); }
static void toupper_(Ctx* c) { c->eax = (uint32_t)toupper((int)ARG(0)); }
static void _ismbblead(Ctx* c) { c->eax = 0; }

static void sprintf_(Ctx* c) { c->eax = (uint32_t)gformat(S(0), 1 << 20, S(1), c->esp + 8); }
static void _snprintf_(Ctx* c) { c->eax = (uint32_t)gformat(S(0), ARG(1), S(2), c->esp + 12); }
static void vsprintf_(Ctx* c) { c->eax = (uint32_t)gformat(S(0), 1 << 20, S(1), ARG(2)); }
static void _vsnprintf_(Ctx* c) { c->eax = (uint32_t)gformat(S(0), ARG(1), S(2), ARG(3)); }
static void printf_(Ctx* c) {
    char b[1024];
    gformat(b, sizeof b, S(0), c->esp + 4);
    vlog("[game] %s", b);
}

/* ---- memory ---- */
static void malloc_(Ctx* c) { c->eax = GA(game_mem(malloc(ARG(0) ? ARG(0) : 1))); }
static void calloc_(Ctx* c) { c->eax = GA(game_mem(calloc(ARG(0), ARG(1) ? ARG(1) : 1))); }
static void realloc_(Ctx* c) { c->eax = GA(game_realloc(H(ARG(0)), ARG(1))); }
static void free_(Ctx* c) { game_free(H(ARG(0))); }
static void _msize(Ctx* c) { c->eax = (uint32_t)malloc_usable_size(H(ARG(0))); }
/* msvcr71's layout: the malloc block's address sits just below the aligned pointer. The game's operator new reads it
   back ((p & ~3) - 4) and hands it to _msize. */
static void _aligned_malloc(Ctx* c) {
    uint32_t align = ARG(1) < 4 ? 4 : ARG(1);
    uint8_t* raw = game_mem(malloc(ARG(0) + align - 1 + 4));
    if (!raw) { c->eax = 0; return; }
    uint32_t p = (GA(raw) + 4 + align - 1) & ~(align - 1);
    ST32(p - 4, G(raw));
    c->eax = p;
}
static void _aligned_free(Ctx* c) { if (ARG(0)) game_free(H(LD32((ARG(0) & ~3u) - 4))); }
static void memset_(Ctx* c) { RC_WT(ARG(0), ARG(2)) memset(H(ARG(0)), (int)ARG(1), ARG(2)); c->eax = ARG(0); }
static void memcpy_(Ctx* c) { RC_WT(ARG(0), ARG(2)) RC_RD(ARG(1), ARG(2)) memcpy(H(ARG(0)), H(ARG(1)), ARG(2)); c->eax = ARG(0); }
static void memmove_(Ctx* c) { RC_WT(ARG(0), ARG(2)) RC_RD(ARG(1), ARG(2)) memmove(H(ARG(0)), H(ARG(1)), ARG(2)); c->eax = ARG(0); }
static void memcmp_(Ctx* c) { c->eax = (uint32_t)memcmp(H(ARG(0)), H(ARG(1)), ARG(2)); }

/* ---- math: the _CI* helpers take and return on the x87 stack ---- */
static void _CIpow(Ctx* c) { f80 y = ST(0); FPOP(); f80 x = ST(0); FPOP(); FPUSH(pow(x, y)); }
static void _CIfmod(Ctx* c) { f80 y = ST(0); FPOP(); f80 x = ST(0); FPOP(); FPUSH(fmod(x, y)); }
static void _CIacos(Ctx* c) { ST(0) = acos(ST(0)); }
static void _CIasin(Ctx* c) { ST(0) = asin(ST(0)); }
static void _CIatan2(Ctx* c) { f80 x = ST(0); FPOP(); ST(0) = atan2(ST(0), x); }
static void _CIexp(Ctx* c) { ST(0) = exp(ST(0)); }
static void _CIlog(Ctx* c) { ST(0) = log(ST(0)); }
static void _CIlog10(Ctx* c) { ST(0) = log10(ST(0)); }
static void _CIsinh(Ctx* c) { ST(0) = sinh(ST(0)); }
static void _CIcosh(Ctx* c) { ST(0) = cosh(ST(0)); }
static void _CItanh(Ctx* c) { ST(0) = tanh(ST(0)); }
static void _ftol(Ctx* c) {  /* truncate st0 to a 64-bit integer in edx:eax, popping it */
    int64_t v = (int64_t)ST(0);
    FPOP();
    c->eax = (uint32_t)v, c->edx = (uint32_t)(v >> 32);
}
static double argd(Ctx* c, int i) { uint64_t b = LD32(c->esp + 4 * i) | (uint64_t)LD32(c->esp + 4 * i + 4) << 32; double d; memcpy(&d, &b, 8); return d; }
static void sqrt_(Ctx* c) { FPUSH(sqrt(argd(c, 0))); }
static void floor_(Ctx* c) { FPUSH(floor(argd(c, 0))); }
static void ceil_(Ctx* c) { FPUSH(ceil(argd(c, 0))); }
static void fabs_(Ctx* c) { FPUSH(fabs(argd(c, 0))); }
static void modf_(Ctx* c) {
    double ip, r = modf(argd(c, 0), &ip);
    STF64(ARG(2), ip);
    FPUSH(r);
}

/* MSVC's rand: same sequence as the PC game */
static uint32_t g_seed = 1;
static void srand_(Ctx* c) { g_seed = ARG(0); }
static void rand_(Ctx* c) { g_seed = g_seed * 214013u + 2531011u; c->eax = (g_seed >> 16) & 0x7fff; }
static void time_(Ctx* c) {
    uint32_t t = (uint32_t)time(NULL);
    if (ARG(0)) ST32(ARG(0), t);
    c->eax = t;
}
static void clock_(Ctx* c) { c->eax = (uint32_t)(clock() * 1000ull / CLOCKS_PER_SEC); }

static void qsort_(Ctx* c);
static void bsearch_(Ctx* c);

/* ---- files: a guest FILE* is the host FILE* (the game only passes it back to these) ---- */
static void fopen_(Ctx* c) {
    char p[512];
    map_path(S(0), p, sizeof p);
    FILE* f = fopen(p, S(1));
    if (!f) vlog("fopen %s (%s) failed\n", p, S(1));
    c->eax = G(f);
}
/* The C runtime's variables: imported as data, so their IAT slots must point at real guest memory. _iob is
   stdin/stdout/stderr (3 MSVC FILEs of 32 bytes), which crt_file() maps to the host's. */
static uint32_t g_iob, g_acmdln, g_adjust_fdiv;
uint32_t crt_data_import(const char* name) {
    if (!strcmp(name, "_iob")) return g_iob ? g_iob : (g_iob = G(guest_alloc(3 * 32)));
    if (!strcmp(name, "_acmdln")) {
        if (!g_acmdln) g_acmdln = G(guest_alloc(4)), ST32(g_acmdln, G(gstrdup("\"" GAME_CWD "\\USM.exe\"")));
        return g_acmdln;
    }
    if (!strcmp(name, "_adjust_fdiv")) return g_adjust_fdiv ? g_adjust_fdiv : (g_adjust_fdiv = G(guest_alloc(4)));
    return 0;
}
void* crt_file(uint32_t f) {
    if (g_iob && f >= g_iob && f < g_iob + 96) return (f - g_iob) / 32 == 0 ? stdin : (f - g_iob) / 32 == 1 ? stdout : stderr;
    return H(f);
}
#define F(i) ((FILE*)crt_file(ARG(i)))
static void fclose_(Ctx* c) { c->eax = (uint32_t)fclose(F(0)); }
static void fread_(Ctx* c) { c->eax = (uint32_t)fread(H(ARG(0)), ARG(1), ARG(2), F(3)); }
static void fwrite_(Ctx* c) { c->eax = (uint32_t)fwrite(H(ARG(0)), ARG(1), ARG(2), F(3)); }
static void fseek_(Ctx* c) { c->eax = (uint32_t)fseek(F(0), (long)(int32_t)ARG(1), (int)ARG(2)); }
static void ftell_(Ctx* c) { c->eax = (uint32_t)ftell(F(0)); }
static void feof_(Ctx* c) { c->eax = (uint32_t)feof(F(0)); }
static void fflush_(Ctx* c) { c->eax = ARG(0) ? (uint32_t)fflush(F(0)) : 0; }
static void fgets_(Ctx* c) { char* r = fgets(S(0), (int)ARG(1), F(2)); c->eax = r ? ARG(0) : 0; }
static void fputs_(Ctx* c) { c->eax = (uint32_t)fputs(S(0), F(1)); }
static void fgetc_(Ctx* c) { c->eax = (uint32_t)fgetc(F(0)); }
static void fprintf_(Ctx* c) {
    char b[4096];
    int n = gformat(b, sizeof b, S(1), c->esp + 8);
    FILE* f = F(0);
    if (f == stdout || f == stderr) vlog("[game] %s", b);
    else c->eax = (uint32_t)fwrite(b, 1, (size_t)n, f);
}
static void vfprintf_(Ctx* c) {
    char b[4096];
    int n = gformat(b, sizeof b, S(1), ARG(2));
    FILE* f = F(0);
    if (f == stdout || f == stderr) vlog("[game] %s", b);
    else c->eax = (uint32_t)fwrite(b, 1, (size_t)n, f);
}
static void tmpfile_(Ctx* c) { c->eax = G(fopen(USM_DIR "tmpfile.bin", "w+b")); }
static void _fstat_(Ctx* c) { c->eax = 0xffffffffu; }  /* no fd-level files are handed out */
static void _flushall_(Ctx* c) { c->eax = 0; }
static void _chdir_(Ctx* c) { c->eax = 0; }

/* ---- the rest of what the game imports ---- */
static void longjmp_(Ctx* c) {  /* back into the C frame of the translated setjmp that recorded this jmp_buf */
    rc_longjmp(ARG(0), (int)ARG(1));
    rc_trap(c, ARG(0), "longjmp to a jmp_buf no translated setjmp recorded");
}
static void _setjmp3_(Ctx* c) { rc_trap(c, ARG(0), "_setjmp3 called directly (setjmp callers are translated inline)"); }
static void isxdigit_(Ctx* c) { c->eax = isxdigit((int)ARG(0)) != 0; }
static void setlocale_(Ctx* c) { static uint32_t s; if (!s) s = G(gstrdup("C")); c->eax = s; }
static void _mbsdup_(Ctx* c) { c->eax = G(gstrdup(S(0))); }
static void ldexp_(Ctx* c) { FPUSH(ldexp(argd(c, 0), (int)ARG(2))); }
static void frexp_(Ctx* c) { int e; FPUSH(frexp(argd(c, 0), &e)); ST32(ARG(2), (uint32_t)e); }
static void _finite_(Ctx* c) { c->eax = isfinite(argd(c, 0)) != 0; }
static void strtoul_(Ctx* c) {
    char* end;
    c->eax = (uint32_t)strtoul(S(0), &end, (int)ARG(2));
    if (ARG(1)) ST32(ARG(1), G(end));
}
static void strcspn_(Ctx* c) { c->eax = (uint32_t)strcspn(S(0), S(1)); }
static void strspn_(Ctx* c) { c->eax = (uint32_t)strspn(S(0), S(1)); }
static void _itoa_(Ctx* c) {  /* (value, buffer, radix) */
    char tmp[40], *p = tmp + sizeof tmp - 1;
    uint32_t v = ARG(0), radix = ARG(2) ? ARG(2) : 10;
    int neg = radix == 10 && (int32_t)v < 0;
    if (neg) v = 0u - v;
    *p = 0;
    do *--p = "0123456789abcdefghijklmnopqrstuvwxyz"[v % radix], v /= radix; while (v);
    if (neg) *--p = '-';
    strcpy(S(1), p);
    c->eax = ARG(1);
}
static void _strtime_(Ctx* c) { time_t t = time(NULL); strftime(S(0), 9, "%H:%M:%S", localtime(&t)); c->eax = ARG(0); }
static void _strdate_(Ctx* c) { time_t t = time(NULL); strftime(S(0), 9, "%m/%d/%y", localtime(&t)); c->eax = ARG(0); }
static void Nop_(Ctx* c) { c->eax = 0; }
static void ThisRet_(Ctx* c) { c->eax = c->ecx; }  /* thiscall constructors return this */
static void exception_ctor_(Ctx* c) { memset(H(c->ecx + 4), 0, 8); c->eax = c->ecx; }
static void _XcptFilter_(Ctx* c) { c->eax = 0; }  /* EXCEPTION_CONTINUE_SEARCH */
static void __CxxFrameHandler_(Ctx* c) { c->eax = 1; }  /* ExceptionContinueSearch */
static void fatal_(Ctx* c, const char* what) {
    vlog("fatal: %s\n", what);
    rc_trap(c, LD32(c->esp - 4), what);
}
static void _purecall_(Ctx* c) { fatal_(c, "pure virtual function call"); }
static void _CxxThrowException_(Ctx* c) { fatal_(c, "C++ exception thrown (never seen on the PC)"); }
static void terminate_(Ctx* c) { fatal_(c, "std::terminate"); }
static void _amsg_exit_(Ctx* c) { vlog("_amsg_exit(%d)\n", (int)ARG(0)); exit(255); }
static void __security_error_handler_(Ctx* c) { fatal_(c, "/GS buffer overrun check failed (a shim popped the wrong stack size?)"); }

/* sscanf for the game: results go to guest pointers. %d %i %u %x %o %c %s %[..] %f %e %g %n, with *, width, h/l. */
static void sscanf_(Ctx* c) {
    const char *s = S(0), *f = S(1), *s0 = s;
    uint32_t ap = c->esp + 8;
    int n = 0;
    while (*f) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; f++; continue; }
        if (*f != '%' || f[1] == '%') { if (*f == '%') f++; if (*s != *f) break; s++, f++; continue; }
        f++;
        int skip = *f == '*';
        if (skip) f++;
        int width = 0;
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        int lng = 0;
        while (*f == 'l' || *f == 'h' || *f == 'L') lng = *f++ == 'l' ? lng + 1 : lng;
        char conv = *f++;
        if (conv != 'c' && conv != '[' && conv != 'n') while (isspace((unsigned char)*s)) s++;
        if (!*s && conv != 'n') break;
        uint32_t dst = skip ? 0 : LD32(ap);
        if (!skip) ap += 4;
        char buf[512];
        int k = 0, max = width ? width : (int)sizeof buf - 1;
        if (conv == 'n') { if (dst) ST32(dst, (uint32_t)(s - s0)); continue; }
        if (strchr("diuxXo", conv)) {
            while (*s && k < max && (isxdigit((unsigned char)*s) || ((*s == '-' || *s == '+') && !k) || ((*s == 'x' || *s == 'X') && k == 1))) buf[k++] = *s++;
            buf[k] = 0;
            if (!k) break;
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            if (base == 10) { int j = 0; while (buf[j] == '-' || buf[j] == '+' || isdigit((unsigned char)buf[j])) j++; s -= k - j; buf[j] = 0; }
            long long v = strtoll(buf, NULL, base);
            if (dst) { if (lng >= 2) ST32(dst, (uint32_t)v), ST32(dst + 4, (uint32_t)(v >> 32)); else ST32(dst, (uint32_t)v); }
        } else if (strchr("eEfgG", conv)) {
            char* end;
            double v = strtod(s, &end);
            if (end == s) break;
            if (width && end - s > width) end = (char*)s + width, v = strtod(strncpy(buf, s, (size_t)width), NULL), buf[width] = 0;
            s = end;
            if (dst) { if (lng) STF64(dst, v); else STF32(dst, (float)v); }
        } else if (conv == 's') {
            while (*s && !isspace((unsigned char)*s) && k < max) { if (dst) ST8(dst + k, *s); k++, s++; }
            if (dst) ST8(dst + k, 0);
        } else if (conv == 'c') {
            for (int w = width ? width : 1; w-- && *s; s++, k++) if (dst) ST8(dst + k, *s);
        } else if (conv == '[') {
            int neg = *f == '^';
            if (neg) f++;
            const char* set = f;
            if (*f == ']') f++;
            while (*f && *f != ']') f++;
            int setn = (int)(f - set);
            if (*f) f++;
            while (*s && k < max) {
                int in = 0;
                for (int j = 0; j < setn; j++) in |= set[j] == *s;
                if (in == neg) break;
                if (dst) ST8(dst + k, *s);
                k++, s++;
            }
            if (!k) break;
            if (dst) ST8(dst + k, 0);
        } else break;
        if (!skip) n++;
    }
    c->eax = (uint32_t)n;
}
static void _getcwd(Ctx* c) { strncpy(S(0), GAME_CWD, ARG(1)); c->eax = ARG(0); }
static void _mkdir_(Ctx* c) {
    char p[512];
    map_path(S(0), p, sizeof p);
    c->eax = (uint32_t)mkdir(p, 0777);
}
static void remove_(Ctx* c) {
    char p[512];
    map_path(S(0), p, sizeof p);
    c->eax = (uint32_t)remove(p);
}

/* ---- callbacks into the game ---- */
static uint32_t g_cmp;
static int cmp_tramp(const void* a, const void* b) {
    uint32_t args[2] = {G(a), G(b)};
    return (int)guest_call(g_cmp, 2, args);
}
static void qsort_(Ctx* c) {
    uint32_t saved = g_cmp;  /* comparators can sort too */
    g_cmp = ARG(3);
    qsort(H(ARG(0)), ARG(1), ARG(2), cmp_tramp);
    g_cmp = saved;
}
static void bsearch_(Ctx* c) {
    uint32_t saved = g_cmp;
    g_cmp = ARG(4);
    void* r = bsearch(H(ARG(0)), H(ARG(1)), ARG(2), ARG(3), cmp_tramp);
    g_cmp = saved;
    c->eax = G(r);
}

const ShimDef shims_msvcr71[] = {
    {"__set_app_type", __set_app_type}, {"__p__fmode", __p__fmode}, {"__p__commode", __p__commode},
    {"_controlfp", _controlfp}, {"_initterm", _initterm}, {"_onexit", _onexit}, {"__dllonexit", _onexit},
    {"__getmainargs", __getmainargs}, {"exit", exit_}, {"_exit", exit_}, {"_cexit", Nop_}, {"_c_exit", Nop_},
    {"vfprintf", vfprintf_}, {"tmpfile", tmpfile_}, {"_fstat", _fstat_}, {"_flushall", _flushall_}, {"_chdir", _chdir_},
    {"longjmp", longjmp_}, {"_setjmp3", _setjmp3_}, {"isxdigit", isxdigit_}, {"setlocale", setlocale_},
    {"_mbsdup", _mbsdup_}, {"ldexp", ldexp_}, {"frexp", frexp_}, {"_finite", _finite_}, {"strtoul", strtoul_},
    {"strcspn", strcspn_}, {"strspn", strspn_}, {"_itoa", _itoa_}, {"_strtime", _strtime_}, {"_strdate", _strdate_},
    {"__setusermatherr", Nop_}, {"_callnewh", Nop_}, {"??1type_info@@UAE@XZ", Nop_}, {"_XcptFilter", _XcptFilter_},
    {"??0exception@@QAE@XZ", exception_ctor_}, {"??0exception@@QAE@ABV0@@Z", exception_ctor_},
    {"??1exception@@UAE@XZ", Nop_}, {"__CxxFrameHandler", __CxxFrameHandler_}, {"_purecall", _purecall_},
    {"_CxxThrowException", _CxxThrowException_}, {"?terminate@@YAXXZ", terminate_}, {"_amsg_exit", _amsg_exit_},
    {"__security_error_handler", __security_error_handler_}, {"sscanf", sscanf_},
    {"_strcmpi", _strcmpi}, {"_stricmp", _strcmpi}, {"_strnicmp", _strnicmp}, {"strcmp", strcmp_}, {"strncmp", strncmp_},
    {"strncpy", strncpy_}, {"strncat", strncat_}, {"strstr", strstr_}, {"strchr", strchr_}, {"strrchr", strrchr_},
    {"_strlwr", _strlwr}, {"_strupr", _strupr}, {"_strdup", _strdup}, {"strtok", strtok_}, {"atoi", atoi_},
    {"atol", atol_}, {"atof", atof_}, {"strtol", strtol_}, {"strtod", strtod_}, {"isalpha", isalpha_},
    {"isdigit", isdigit_}, {"isspace", isspace_}, {"isalnum", isalnum_}, {"isupper", isupper_}, {"islower", islower_},
    {"isprint", isprint_}, {"tolower", tolower_}, {"toupper", toupper_}, {"_ismbblead", _ismbblead},
    {"sprintf", sprintf_}, {"_snprintf", _snprintf_}, {"vsprintf", vsprintf_}, {"_vsnprintf", _vsnprintf_},
    {"printf", printf_},
    {"malloc", malloc_}, {"calloc", calloc_}, {"realloc", realloc_}, {"free", free_}, {"_msize", _msize},
    {"_aligned_malloc", _aligned_malloc}, {"_aligned_free", _aligned_free}, {"??2@YAPAXI@Z", malloc_},
    {"??3@YAXPAX@Z", free_}, {"??_U@YAPAXI@Z", malloc_}, {"??_V@YAXPAX@Z", free_},
    {"memset", memset_}, {"memcpy", memcpy_}, {"memmove", memmove_}, {"memcmp", memcmp_},
    {"_CIpow", _CIpow}, {"_CIfmod", _CIfmod}, {"_CIacos", _CIacos}, {"_CIasin", _CIasin}, {"_CIatan2", _CIatan2},
    {"_CIexp", _CIexp}, {"_CIlog", _CIlog}, {"_CIlog10", _CIlog10}, {"_CIsinh", _CIsinh}, {"_CIcosh", _CIcosh},
    {"_CItanh", _CItanh}, {"_ftol", _ftol}, {"sqrt", sqrt_}, {"floor", floor_}, {"ceil", ceil_}, {"fabs", fabs_},
    {"modf", modf_},
    {"srand", srand_}, {"rand", rand_}, {"time", time_}, {"clock", clock_}, {"qsort", qsort_}, {"bsearch", bsearch_},
    {"fopen", fopen_}, {"fclose", fclose_}, {"fread", fread_}, {"fwrite", fwrite_}, {"fseek", fseek_},
    {"ftell", ftell_}, {"feof", feof_}, {"fflush", fflush_}, {"fgets", fgets_}, {"fputs", fputs_}, {"fgetc", fgetc_},
    {"fprintf", fprintf_}, {"_getcwd", _getcwd}, {"_mkdir", _mkdir_}, {"remove", remove_},
    {NULL, NULL},
};
