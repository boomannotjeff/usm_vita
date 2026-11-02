/* Differential test for the recompiler: each original USM.exe function vs. its translated twin, from the same
   random registers, stack and memory. Compares every register, the x87 stack, .data, the world buffers and the
   caller's stack. USM.exe is mapped by hand at 0x400000; imports are not resolved (calling one is "untestable").
   usage: difftest.exe [first index] [count]          prints PASS/FAIL/UNTEST lines */
#include <stdio.h>
#include <process.h>
#include <stdlib.h>
#include <windows.h>
#include <intrin.h>

#include "recomp.h"

extern const uint8_t rc_resume_stubs[];  /* stub 0: where an outermost native call returns */
#define rc_resume rc_resume_stubs
Ctx* rc_ctx(void);

#define BUF 512
#define NW 16
/* The world is one 64 KB region of NROWS rows; random pointers aim at the NW rows in the middle, so large or
   negative offsets from them still land in memory that is snapshotted and compared (not the tester's own). */
#define NROWS (0x10000 / BUF)
#define WSZ (NROWS * BUF)
static uint8_t (*W)[BUF], (*W0)[BUF], (*Wo)[BUF];
#define STK 0x4000
static uint8_t S[STK], S0[STK], So[STK];  /* guest stack; the test frame sits at S + STK/2 */
static uint32_t data_lo, data_hi;
static uint8_t *data0, *datao;

static void* jb[5];
static volatile int guard;
static volatile DWORD t_start;
static volatile unsigned last_code, last_at, last_va;
static uint32_t fs0;

static LONG CALLBACK veh(EXCEPTION_POINTERS* e) {
    if (!guard) return EXCEPTION_CONTINUE_SEARCH;
    guard = 0;
    last_code = e->ExceptionRecord->ExceptionCode, last_at = (unsigned)e->ExceptionRecord->ExceptionAddress, last_va = (unsigned)e->ExceptionRecord->ExceptionInformation[1];
    __builtin_longjmp(jb, 1);
}
static void import_called(void) { last_code = 0x1e; guard = 0; __builtin_longjmp(jb, 1); }
static void timed_out(void) { last_code = 0x70; guard = 0; __builtin_longjmp(jb, 1); }

static HANDLE main_thread;
static uint8_t landing_stack[4096];
static DWORD WINAPI watchdog(void* p) {
    for (;;) {
        Sleep(50);
        if (!guard || GetTickCount() - t_start < 200) continue;
        SuspendThread(main_thread);
        if (guard) {
            CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
            GetThreadContext(main_thread, &c);
            c.Eip = (DWORD)timed_out;
            c.Esp = (DWORD)(landing_stack + sizeof landing_stack - 64);
            SetThreadContext(main_thread, &c);
        }
        ResumeThread(main_thread);
    }
}

static unsigned rs = 12345;
static unsigned rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static uint32_t rval(void) {
    unsigned r = rnd() % 8;
    if (r < 3) { float f = (float)((int)(rnd() % 20001) - 10000) / 100.0f; uint32_t w; memcpy(&w, &f, 4); return w; }
    if (r < 5) return rnd() % 17;
    return (uint32_t)&W[(NROWS - NW) / 2 + rnd() % NW][(rnd() % (BUF / 4)) * 4];
}

typedef struct { uint32_t r[8]; unsigned fdepth; uint8_t st[8][10]; } state_t;
static const char* RN[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

/* Hook patches, as the game build installs them: `jmp stub` over each hookable entry. */
struct lifted_entry { unsigned addr; void* fn; };
extern const struct lifted_entry lifted_table[];
static uint8_t (*saved)[5];
static void patch(unsigned k, int on) {
    uint8_t* p = (uint8_t*)rc_addr[k];
    if (!on) { memcpy(p, saved[k], 5); return; }
    int32_t rel = (int32_t)((uint32_t)lifted_table[k].fn - (rc_addr[k] + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
}
static uint32_t text_end;
/* .text is writable only while patches change, so a tested function cannot corrupt code for later tests */
static void patch_all(int on, int k1) {
    DWORD old;
    VirtualProtect((void*)0x401000, text_end - 0x401000, PAGE_EXECUTE_READWRITE, &old);
    for (unsigned k = 0; k < rc_count; k++)
        if (lifted_table[k].fn && (k1 < 0 || k == (unsigned)k1)) patch(k, on);
    VirtualProtect((void*)0x401000, text_end - 0x401000, PAGE_EXECUTE_READ, &old);
}

/* orig: run original code through rc_native. Otherwise the translation: called directly, or (via_native) through
   rc_native into the patched entry, i.e. native caller -> stub -> rc_enter, exactly as in the game. */
static int via_native;
static int run(int orig, unsigned k, const uint32_t* regs, state_t* out) {
    Ctx* c = rc_ctx();
    static uint32_t host_sp0;
    if (!host_sp0) host_sp0 = c->host_sp;
    c->host_sp = host_sp0;
    c->nout = 0;
    memcpy(&c->eax, regs, 32);
    c->ftop = c->fdepth = 0;
    c->fsw = 0;
    static const uint16_t cw = 0x007F;  /* the game's x87 mode once D3D is up */
    __asm__ volatile("fninit\n\tfldcw %0" : : "m"(cw));
    c->fcw = cw;
    c->fsbase = __readfsdword(0x18);
    fs0 = __readfsdword(0);
    last_code = 0;
    if (__builtin_setjmp(jb)) {
        __writefsdword(0, fs0);
        return 1;
    }
    t_start = GetTickCount();
    guard = 1;
    if (orig || via_native) {
        rc_native(c, rc_addr[k]);
    } else {
        c->esp -= 4;
        ST32(c->esp, (uint32_t)rc_resume);
        ST32(c->esp - 4, rc_addr[k]);  /* the bridge leaves the target there on the original side; uninitialised locals see it */
        rc_impl[k](c);
        if (c->target != (uint32_t)rc_resume) { guard = 0; last_code = 0xbad; return 1; }
    }
    guard = 0;
    memcpy(out->r, &c->eax, 32);
    out->fdepth = c->fdepth;
    for (unsigned i = 0; i < c->fdepth && i < 8; i++) memcpy(out->st[i], &ST(i), 10);
    return 0;
}

static char** g_argv;
static void map_exe(void) {
    const char* exe = getenv("USM_EXE") ? getenv("USM_EXE") : "USM.exe";
    FILE* fp = fopen(exe, "rb");
    if (!fp) { printf("cannot open %s\n", exe); exit(2); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* file = malloc(n);
    fread(file, 1, n, fp);
    fclose(fp);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(file + ((IMAGE_DOS_HEADER*)file)->e_lfanew);
    /* up to the end of .rsrc: the last (unnamed) section holds no functions, and the system maps something there */
    uint32_t size = 0x591000;
    uint8_t* base = VirtualAlloc((void*)0x400000, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) {
        /* ASLR put a system mapping in the way; a fresh process lays memory out differently */
        const char* r = getenv("DT_RETRY");
        int n = r ? atoi(r) : 0;
        if (n >= 60) exit(3);
        char env[32];
        snprintf(env, sizeof env, "DT_RETRY=%d", n + 1);
        _putenv(env);
        char self[MAX_PATH + 2];
        snprintf(self, sizeof self, "\"%s\"", g_argv[0]);  /* the path has a space in it */
        exit((int)_spawnl(_P_WAIT, g_argv[0], self, g_argv[1] ? g_argv[1] : "0", g_argv[1] && g_argv[2] ? g_argv[2] : "999999", NULL));
    }
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sh[i].VirtualAddress + sh[i].SizeOfRawData > size) continue;
        memcpy(base + sh[i].VirtualAddress, file + sh[i].PointerToRawData, sh[i].SizeOfRawData);
        if (!memcmp(sh[i].Name, ".data", 6)) data_lo = 0x400000 + sh[i].VirtualAddress, data_hi = data_lo + sh[i].Misc.VirtualSize;
    }
    /* every import slot -> import_called */
    IMAGE_DATA_DIRECTORY* d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* id = (void*)(base + d->VirtualAddress); id->Name; id++)
        for (uint32_t* slot = (uint32_t*)(base + id->FirstThunk); *slot; slot++) *slot = (uint32_t)import_called;
    /* a tested function that writes into code or constants would corrupt every later test of the original only */
    DWORD old;
    VirtualProtect(base, data_lo - 0x400000, PAGE_EXECUTE_READ, &old);
    VirtualProtect((void*)data_hi, 0x991000 - data_hi, PAGE_READONLY, &old);
    data0 = malloc(data_hi - data_lo);
    datao = malloc(data_hi - data_lo);
    memcpy(data0, (void*)data_lo, data_hi - data_lo);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_argv = argv;
    map_exe();
    W = VirtualAlloc(NULL, WSZ, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    W0 = malloc(WSZ);
    Wo = malloc(WSZ);
    for (unsigned k = 0; k < rc_count; k++) rc_on[k] = 1;
    AddVectoredExceptionHandler(1, veh);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, THREAD_ALL_ACCESS, FALSE, 0);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    unsigned first = argc > 1 ? strtoul(argv[1], NULL, 0) : 0;
    unsigned count = argc > 2 ? strtoul(argv[2], NULL, 0) : rc_count;
    unsigned pass = 0, fail = 0, untest = 0;
    const char* flags = argc > 3 ? argv[3] : "";
    int verbose = strchr(flags, 'v') != NULL;
    /* e: enter each translation through its hook stub. n: also patch every other hookable entry and switch off
       direct translated->translated calls, so every call nests native -> stub -> translated. */
    int entry = strchr(flags, 'e') || strchr(flags, 'n'), nest = strchr(flags, 'n') != NULL;
    if (entry) {
        text_end = data_lo;
        saved = malloc(rc_count * 5);
        for (unsigned k = 0; k < rc_count; k++) memcpy(saved[k], (void*)rc_addr[k], 5);
        if (nest) memset(rc_on, 0, rc_count);
    }
    if (verbose) printf("W %p W0 %p Wo %p S %p S0 %p So %p\n", (void*)W, (void*)W0, (void*)Wo, (void*)S, (void*)S0, (void*)So);
    for (unsigned k = first; k < rc_count && k < first + count; k++) {
        { const char* pn = getenv("DT_PROGRESS"); FILE* pf = fopen(pn ? pn : "difftest_progress.txt", "w"); if (pf) fprintf(pf, "%u %06x\n", k, rc_addr[k]), fclose(pf); }
        char why[200] = "";
        int good = 0, bad = 0;
        rs = 12345 + k;
        for (int it = 0; it < 16 && !bad; it++) {
            for (int b = 0; b < NROWS; b++)
                for (int i = 0; i < BUF; i += 4) { uint32_t v = rval(); memcpy(&W[b][i], &v, 4); }
            for (int i = 0; i < STK; i += 4) { uint32_t v = rval(); memcpy(&S[i], &v, 4); }
            uint32_t regs[8];
            for (int r = 0; r < 8; r++) regs[r] = rval();
            regs[4] = (uint32_t)&S[STK / 2];
            if (rnd() & 1) regs[5] = (uint32_t)&S[STK / 2 + 64];
            memcpy(W0, W, WSZ);
            memcpy(S0, S, sizeof S);
            memcpy((void*)data_lo, data0, data_hi - data_lo);
            state_t so, sl;
            int co = run(1, k, regs, &so);
            unsigned oc = last_code, oat = last_at, ova = last_va;
            memcpy(Wo, W, WSZ);
            memcpy(So, S, sizeof S);
            memcpy(datao, (void*)data_lo, data_hi - data_lo);
            memcpy(W, W0, WSZ);
            memcpy(S, S0, sizeof S);
            memcpy((void*)data_lo, data0, data_hi - data_lo);
            if (entry && !lifted_table[k].fn) { good = -1000; break; }  /* not hookable: nothing to test this way */
            if (entry) patch_all(1, nest ? -1 : (int)k);
            via_native = entry;
            int cl = run(0, k, regs, &sl);
            via_native = 0;
            if (entry) patch_all(0, nest ? -1 : (int)k);
            unsigned lc = last_code, lat = last_at, lva = last_va;
            if (co || cl) {
                if (co != cl || (oc != lc && oc != 0x70 && lc != 0x70))
                    bad = 1, snprintf(why, sizeof why, "outcome orig=%d(%x at %x va %x) recomp=%d(%x at %x va %x)", co, oc, oat, ova, cl, lc, lat, lva);
                continue;
            }
            good++;
            if (verbose)
                for (int r = 0; r < 8; r++) printf("  it%d %s in %08x orig %08x recomp %08x\n", it, RN[r], regs[r], so.r[r], sl.r[r]);
            for (int r = 0; r < 8 && !bad; r++)
                if (so.r[r] != sl.r[r]) bad = 1, snprintf(why, sizeof why, "%s orig %08x recomp %08x", RN[r], so.r[r], sl.r[r]);
            if (!bad && so.fdepth != sl.fdepth) bad = 1, snprintf(why, sizeof why, "x87 depth orig %u recomp %u", so.fdepth, sl.fdepth);
            for (unsigned i = 0; !bad && i < so.fdepth && i < 8; i++)
                if (memcmp(so.st[i], sl.st[i], 10)) bad = 1, snprintf(why, sizeof why, "st(%u) differs", i);
            for (int b = 0; !bad && b < NROWS; b++)
                if (memcmp(Wo[b], W[b], BUF)) {
                    int o = 0;
                    while (Wo[b][o] == W[b][o]) o++;
                    bad = 1, o &= ~3, snprintf(why, sizeof why, "world %d +%x differs: orig %08x recomp %08x", b, o, *(uint32_t*)&Wo[b][o], *(uint32_t*)&W[b][o]);
                }
            /* the caller's side of the stack: from the final esp up (below it is dead scratch, and the bridge
               spills registers over popped arguments) */
            int lo = so.r[4] >= (uint32_t)S && so.r[4] < (uint32_t)S + STK ? so.r[4] - (uint32_t)S : STK;
            if (!bad && memcmp(So + lo, S + lo, STK - lo)) {
                int o = lo;
                while (So[o] == S[o]) o++;
                bad = 1, snprintf(why, sizeof why, "stack esp%+d differs", o - STK / 2);
            }
            if (!bad && memcmp(datao, (void*)data_lo, data_hi - data_lo)) {
                uint32_t o = 0;
                while (datao[o] == ((uint8_t*)data_lo)[o]) o++;
                bad = 1, snprintf(why, sizeof why, ".data %08x differs", data_lo + o);
            }
        }
        if (bad) fail++, printf("FAIL   %06x  %s\n", rc_addr[k], why);
        else if (good <= 0) untest++, printf("UNTEST %06x\n", rc_addr[k]);
        else pass++, printf("PASS   %06x\n", rc_addr[k]);
    }
    printf("\nPASS %u  FAIL %u  UNTEST %u\n", pass, fail, untest);
    return 0;
}
