/* usm_pc.exe: Ultimate Spider-Man running on recompiled code only.
   USM.exe (next to this exe) is mapped at its own base as data: its sections, with the imports bound to the
   real DLLs, but none of its code is ever executed. The game starts at the translated entry point. Its code
   pages are left non-executable, so anything that still jumps into original code (a callback from Windows or a
   DLL: WndProc, thread starts, static constructors, SEH handlers) faults; the fault handler sends a jump to a
   translated function's entry into its translation and logs and stops on anything else.
   That makes "no original code runs" checked rather than assumed, which is the gate for the Vita port.
   Log: usm_pc.log next to the exe. */
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "census.h"
#include "rt.h"

extern void* const rc_stub[];  /* recomp_table.c: native->translated entry of each function */

static FILE* g_log;
static char g_dir[MAX_PATH], g_exe[MAX_PATH];
static uint8_t* g_base;
static uint32_t g_text_lo, g_text_hi;

static void plog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static int find_fn(uint32_t a) {
    unsigned lo = 0, hi = rc_count;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (rc_addr[m] < a) lo = m + 1;
        else hi = m;
    }
    return lo < rc_count && rc_addr[lo] == a ? (int)lo : -1;
}

/* The coverage gate. An execute fault in USM.exe's .text is native code (Windows, a DLL, the C runtime) entering
   the game: at a function entry it is sent into the translation, anywhere else it is a hole in the coverage. */
static LONG CALLBACK gate(EXCEPTION_POINTERS* e) {
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    uint32_t at = (uint32_t)(uintptr_t)r->ExceptionAddress;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->ExceptionInformation[0] != 8 || at < g_text_lo || at >= g_text_hi) {
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
            static volatile LONG crashes;
            if (InterlockedIncrement(&crashes) <= 3) {
                Ctx* c = rc_ctx();
                plog("crash %08lx at %08x (%s %08lx); guest eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
                     r->ExceptionCode, at, r->ExceptionInformation[0] ? "writing" : "reading", (unsigned long)r->ExceptionInformation[1],
                     c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
                const uint32_t* sp = (const uint32_t*)(uintptr_t)c->esp;
                plog("  guest return addresses:");
                for (int i = 0; i < 48 && !IsBadReadPtr(sp + i, 4); i++)
                    if (sp[i] >= g_text_lo && sp[i] < g_text_hi) plog(" %06x", sp[i]);
                plog("\n");
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    int k = find_fn(at);
    if (k >= 0) {
        /* each callback once, with the native caller: the list the Vita shims must call back into */
        static uint8_t* seen;
        if (!seen) seen = calloc(rc_count, 1);
        if (seen && !seen[k]) {
            seen[k] = 1;
            plog("callback %06x from native %08lx\n", at, *(DWORD*)(uintptr_t)e->ContextRecord->Esp);
        }
        e->ContextRecord->Eip = (DWORD)(uintptr_t)rc_stub[k];
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    /* original code that survey mode let run, carrying on into the next page: not an entry, so not a hole */
    MEMORY_BASIC_INFORMATION mi;
    uint32_t fault = (uint32_t)r->ExceptionInformation[1];  /* the page start, when execution fell through into it */
    if (!getenv("USM_PC_STRICT") && !(fault & 0xfff) && fault - at < 16 &&
        VirtualQuery((void*)(uintptr_t)(fault - 1), &mi, sizeof mi) && mi.Protect == PAGE_EXECUTE_READ) {
        DWORD old;
        VirtualProtect((void*)(uintptr_t)fault, 0x1000, PAGE_EXECUTE_READ, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const uint32_t* sp = (const uint32_t*)(uintptr_t)e->ContextRecord->Esp;
    plog("UNTRANSLATED code at %06x was entered; return addresses on the stack:", at);
    for (int i = 0; i < 32 && !IsBadReadPtr(sp + i, 4); i++)
        if (sp[i] >= g_text_lo && sp[i] < g_text_hi) plog(" %06x", sp[i]);
    plog("\n");
    /* holes.txt collects every hole so one regenerate (discover.py reads it) fixes them all */
    char hp[MAX_PATH];
    snprintf(hp, sizeof hp, "%s\\usm_pc_holes.txt", g_dir);
    FILE* h = fopen(hp, "a");
    if (h) fprintf(h, "%06x\n", at), fclose(h);
    if (getenv("USM_PC_STRICT")) return EXCEPTION_CONTINUE_SEARCH;
    /* survey mode (default): let this page run as original code so the run goes on and finds the next hole */
    /* both pages: an instruction straddling a page boundary faults on the second page, with `at` on the first */
    DWORD old;
    VirtualProtect((void*)(uintptr_t)(at & ~0xfffu), 0x2000, PAGE_EXECUTE_READ, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Calls that must not see this loader as "the program": the game's own module is the mapped USM.exe. */
static HMODULE WINAPI my_GetModuleHandleA(LPCSTR name) { return name ? GetModuleHandleA(name) : (HMODULE)g_base; }
static HMODULE WINAPI my_GetModuleHandleW(LPCWSTR name) { return name ? GetModuleHandleW(name) : (HMODULE)g_base; }
static DWORD WINAPI my_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) {
    if (m && m != (HMODULE)g_base) return GetModuleFileNameA(m, out, n);
    lstrcpynA(out, g_exe, n);
    return (DWORD)strlen(out);
}

/* DirectInput checks that the instance handle is a module the loader knows: the mapped USM.exe is not one */
typedef HRESULT(WINAPI* di8create_fn)(HINSTANCE, DWORD, const void*, void**, void*);
static di8create_fn g_di8create;
static HRESULT WINAPI my_DirectInput8Create(HINSTANCE h, DWORD v, const void* iid, void** out, void* outer) {
    HRESULT r = g_di8create(h == (HINSTANCE)g_base ? GetModuleHandleA(NULL) : h, v, iid, out, outer);
    if (r >= 0 && out) census_dinput(*out);
    return r;
}

/* the game looks DirectInput8Create up at run time */
static FARPROC WINAPI my_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = GetProcAddress(m, name);
    if (!p || (uintptr_t)name <= 0xffff) return p;
    if (!strcmp(name, "DirectInput8Create")) g_di8create = (di8create_fn)p, p = (FARPROC)my_DirectInput8Create;
    if (g_census) {
        char path[MAX_PATH];
        GetModuleFileNameA(m, path, MAX_PATH);
        char* dll = strrchr(path, '\\');
        p = (FARPROC)census_import(dll ? dll + 1 : path, name, (void*)p);
    }
    return p;
}

/* longjmp to a translated setjmp goes back into its C frame (rt.c); any other one is the C runtime's */
typedef void(__cdecl* longjmp_fn)(void*, int);
static longjmp_fn g_longjmp;
static void __cdecl my_longjmp(void* jb, int v) {
    rc_longjmp((uint32_t)(uintptr_t)jb, v);
    g_longjmp(jb, v);
}

/* The game's own memory, for the Vita budget: live and peak bytes it holds through the heap, VirtualAlloc and the
   C runtime (DXVK's and the driver's memory, which the Vita build won't have, is not counted). */
static volatile LONG g_live[3], g_peak[3];  /* heap, virtual, crt */
static void mem_add(int kind, LONG n) {
    LONG v = InterlockedExchangeAdd(&g_live[kind], n) + n;
    for (LONG p = g_peak[kind]; v > p; p = g_peak[kind])
        if (InterlockedCompareExchange(&g_peak[kind], v, p) == p) break;
}
static LPVOID WINAPI my_HeapAlloc(HANDLE h, DWORD f, SIZE_T n) {
    LPVOID p = HeapAlloc(h, f, n);
    if (p) mem_add(0, (LONG)n);
    return p;
}
static LPVOID WINAPI my_HeapReAlloc(HANDLE h, DWORD f, LPVOID old, SIZE_T n) {
    SIZE_T was = old ? HeapSize(h, 0, old) : 0;
    LPVOID p = HeapReAlloc(h, f, old, n);
    if (p) mem_add(0, (LONG)n - (LONG)was);
    return p;
}
static BOOL WINAPI my_HeapFree(HANDLE h, DWORD f, LPVOID p) {
    if (p) mem_add(0, -(LONG)HeapSize(h, 0, p));
    return HeapFree(h, f, p);
}
static LPVOID WINAPI my_VirtualAlloc(LPVOID a, SIZE_T n, DWORD t, DWORD prot) {
    LPVOID p = VirtualAlloc(a, n, t, prot);
    if (p && (t & MEM_COMMIT)) mem_add(1, (LONG)n);
    return p;
}
static BOOL WINAPI my_VirtualFree(LPVOID a, SIZE_T n, DWORD t) {
    MEMORY_BASIC_INFORMATION mi;
    if (t & MEM_RELEASE && VirtualQuery(a, &mi, sizeof mi) && mi.State == MEM_COMMIT) mem_add(1, -(LONG)mi.RegionSize);
    else if (t & MEM_DECOMMIT) mem_add(1, -(LONG)n);
    return VirtualFree(a, n, t);
}
typedef void* (__cdecl* malloc_fn)(size_t);
typedef void* (__cdecl* realloc_fn)(void*, size_t);
typedef void (__cdecl* free_fn)(void*);
typedef size_t (__cdecl* msize_fn)(void*);
static malloc_fn g_malloc, g_new, g_newa;
static realloc_fn g_realloc;
static free_fn g_free, g_delete, g_deletea;
static msize_fn g_msize;
/* _aligned_malloc (the game's own operator new sits on it) has no size query in msvcr71: remember sizes here */
typedef void* (__cdecl* amalloc_fn)(size_t, size_t);
static amalloc_fn g_amalloc;
static free_fn g_afree;
#define NSLOT (1u << 21)
static struct { void* p; size_t n; }* g_asz;
static CRITICAL_SECTION g_acs;
static void* __cdecl my_aligned_malloc(size_t n, size_t align) {
    void* p = g_amalloc(n, align);
    if (!p) return p;
    EnterCriticalSection(&g_acs);
    for (unsigned h = ((uintptr_t)p >> 4) & (NSLOT - 1);; h = (h + 1) & (NSLOT - 1))
        if (!g_asz[h].p || g_asz[h].p == (void*)1) { g_asz[h].p = p, g_asz[h].n = n; break; }
    LeaveCriticalSection(&g_acs);
    mem_add(2, (LONG)n);
    return p;
}
static void __cdecl my_aligned_free(void* p) {
    if (p) {
        EnterCriticalSection(&g_acs);
        for (unsigned h = ((uintptr_t)p >> 4) & (NSLOT - 1); g_asz[h].p; h = (h + 1) & (NSLOT - 1))
            if (g_asz[h].p == p) { mem_add(2, -(LONG)g_asz[h].n); g_asz[h].p = (void*)1; break; }  /* 1: deleted */
        LeaveCriticalSection(&g_acs);
    }
    g_afree(p);
}
static HGLOBAL WINAPI my_GlobalAlloc(UINT f, SIZE_T n) { HGLOBAL h = GlobalAlloc(f, n); if (h) mem_add(0, (LONG)n); return h; }
static HGLOBAL WINAPI my_GlobalFree(HGLOBAL h) { if (h) mem_add(0, -(LONG)GlobalSize(h)); return GlobalFree(h); }
/* mapped files: the Vita build will read them into memory, so they count */
static LPVOID WINAPI my_MapViewOfFile(HANDLE m, DWORD acc, DWORD hi, DWORD lo, SIZE_T n) {
    LPVOID p = MapViewOfFile(m, acc, hi, lo, n);
    MEMORY_BASIC_INFORMATION mi;
    if (p && VirtualQuery(p, &mi, sizeof mi)) mem_add(1, (LONG)mi.RegionSize);
    return p;
}

static void* crt_alloc(malloc_fn f, size_t n) { void* p = f(n); if (p) mem_add(2, (LONG)n); return p; }
static void crt_release(free_fn f, void* p) { if (p) mem_add(2, -(LONG)g_msize(p)); f(p); }
static void* __cdecl my_malloc(size_t n) { return crt_alloc(g_malloc, n); }
static void* __cdecl my_new(size_t n) { return crt_alloc(g_new, n); }
static void* __cdecl my_newa(size_t n) { return crt_alloc(g_newa, n); }
static void __cdecl my_free(void* p) { crt_release(g_free, p); }
static void __cdecl my_delete(void* p) { crt_release(g_delete, p); }
static void __cdecl my_deletea(void* p) { crt_release(g_deletea, p); }
static void* __cdecl my_calloc(size_t a, size_t b) { void* p = my_malloc(a * b); if (p) memset(p, 0, a * b); return p; }
static void* __cdecl my_realloc(void* old, size_t n) {
    size_t was = old ? g_msize(old) : 0;
    void* p = g_realloc(old, n);
    if (p || !n) mem_add(2, (LONG)n - (LONG)was);
    return p;
}
static DWORD WINAPI mem_report(LPVOID unused) {
    LONG last = -1;
    for (;;) {
        Sleep(10000);
        LONG total = g_peak[0] + g_peak[1] + g_peak[2];
        if (total == last) continue;
        last = total;
        plog("memory (MB, live/peak): heap %ld/%ld, virtual %ld/%ld, crt %ld/%ld\n", g_live[0] >> 20, g_peak[0] >> 20,
             g_live[1] >> 20, g_peak[1] >> 20, g_live[2] >> 20, g_peak[2] >> 20);
    }
    return 0;
}

static void* crt_shim(HMODULE crt, const char* name) {
    static const struct { const char* name; void** real; void* mine; } t[] = {
        {"malloc", (void**)&g_malloc, (void*)my_malloc}, {"free", (void**)&g_free, (void*)my_free},
        {"realloc", (void**)&g_realloc, (void*)my_realloc}, {"calloc", NULL, (void*)my_calloc},
        {"??2@YAPAXI@Z", (void**)&g_new, (void*)my_new}, {"??3@YAXPAX@Z", (void**)&g_delete, (void*)my_delete},
        {"??_U@YAPAXI@Z", (void**)&g_newa, (void*)my_newa}, {"??_V@YAXPAX@Z", (void**)&g_deletea, (void*)my_deletea},
        {"_aligned_malloc", (void**)&g_amalloc, (void*)my_aligned_malloc}, {"_aligned_free", (void**)&g_afree, (void*)my_aligned_free},
    };
    g_msize = (msize_fn)GetProcAddress(crt, "_msize");
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++)
        if (!strcmp(name, t[i].name)) {
            if (t[i].real) *t[i].real = (void*)GetProcAddress(crt, name);
            return t[i].mine;
        }
    return NULL;
}

static void* shim(const char* dll, const char* name) {
    if (!_stricmp(dll, "MSVCR71.DLL")) {
        void* p = crt_shim(GetModuleHandleA("msvcr71.dll"), name);
        if (p) return p;
    }
    if (!_stricmp(dll, "MSVCR71.DLL") && !strcmp(name, "longjmp")) {
        g_longjmp = (longjmp_fn)GetProcAddress(GetModuleHandleA("msvcr71.dll"), name);
        return (void*)my_longjmp;
    }
    if (!_stricmp(dll, "DINPUT8.DLL") && !strcmp(name, "DirectInput8Create")) {
        g_di8create = (di8create_fn)GetProcAddress(GetModuleHandleA("dinput8.dll"), name);
        return (void*)my_DirectInput8Create;
    }
    if (!_stricmp(dll, "KERNEL32.DLL")) {
        if (!strcmp(name, "GetModuleHandleA")) return (void*)my_GetModuleHandleA;
        if (!strcmp(name, "GetModuleHandleW")) return (void*)my_GetModuleHandleW;
        if (!strcmp(name, "GetModuleFileNameA")) return (void*)my_GetModuleFileNameA;
        if (!strcmp(name, "GetProcAddress")) return (void*)my_GetProcAddress;
        if (!strcmp(name, "HeapAlloc")) return (void*)my_HeapAlloc;
        if (!strcmp(name, "HeapReAlloc")) return (void*)my_HeapReAlloc;
        if (!strcmp(name, "HeapFree")) return (void*)my_HeapFree;
        if (!strcmp(name, "VirtualAlloc")) return (void*)my_VirtualAlloc;
        if (!strcmp(name, "VirtualFree")) return (void*)my_VirtualFree;
        if (!strcmp(name, "GlobalAlloc")) return (void*)my_GlobalAlloc;
        if (!strcmp(name, "GlobalFree")) return (void*)my_GlobalFree;
        if (!strcmp(name, "MapViewOfFile")) return (void*)my_MapViewOfFile;
    }
    return NULL;
}

static HMODULE load_dll(const char* name) {
    /* dsound.dll in the game folder is the ASI loader: it would load the hook-build mods into this process */
    if (!_stricmp(name, "DSOUND.DLL")) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\dsound.dll");
        return LoadLibraryA(sys);
    }
    return LoadLibraryA(name);
}

static int map_exe(void) {
    FILE* f = fopen(g_exe, "rb");
    if (!f) { plog("cannot open %s\n", g_exe); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* file = malloc(n);
    fread(file, 1, n, f);
    fclose(f);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(file + ((IMAGE_DOS_HEADER*)file)->e_lfanew);
    uint32_t base = nt->OptionalHeader.ImageBase, size = nt->OptionalHeader.SizeOfImage;
    g_base = VirtualAlloc((void*)(uintptr_t)base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (g_base != (uint8_t*)(uintptr_t)base) return -1;
    memcpy(g_base, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        memcpy(g_base + sh[i].VirtualAddress, file + sh[i].PointerToRawData, min(sh[i].SizeOfRawData, sh[i].Misc.VirtualSize));
        if (!memcmp(sh[i].Name, ".text", 6)) g_text_lo = base + sh[i].VirtualAddress, g_text_hi = g_text_lo + sh[i].Misc.VirtualSize;
    }
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) plog("note: USM.exe has a TLS directory (not set up)\n");

    /* imports */
    IMAGE_DATA_DIRECTORY* d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    unsigned bound = 0, missing = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* id = (void*)(g_base + d->VirtualAddress); id->Name; id++) {
        const char* dll = (const char*)(g_base + id->Name);
        HMODULE m = load_dll(dll);
        if (!m) { plog("cannot load %s (error %lu)\n", dll, GetLastError()); return 0; }
        uint32_t* names = (uint32_t*)(g_base + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
        uint32_t* slots = (uint32_t*)(g_base + id->FirstThunk);
        for (; *names; names++, slots++) {
            const char* name = (*names & IMAGE_ORDINAL_FLAG32) ? NULL : (const char*)(g_base + *names + 2);
            void* p = name ? shim(dll, name) : NULL;
            if (!p) p = (void*)GetProcAddress(m, name ? name : (LPCSTR)(uintptr_t)(*names & 0xffff));
            if (!p) { plog("missing import %s!%s\n", dll, name ? name : "(ordinal)"); missing++; }
            if (name && g_census) {
                void* w = census_hook(dll, name, p);
                p = w ? w : census_import(dll, name, p);
            }
            *slots = (uint32_t)(uintptr_t)p;
            bound++;
        }
    }
    plog("mapped USM.exe at %06x, %u imports bound, %u missing\n", base, bound, missing);

    /* code: readable (translated code reads data and jump tables from it) but never executable */
    DWORD old;
    VirtualProtect((void*)(uintptr_t)g_text_lo, g_text_hi - g_text_lo, PAGE_READONLY, &old);
    free(file);
    return 1;
}

int main(int argc, char** argv) {
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    *strrchr(g_dir, '\\') = 0;
    SetCurrentDirectoryA(g_dir);
    snprintf(g_exe, sizeof g_exe, "%s\\USM.exe", g_dir);
    char logp[MAX_PATH];
    snprintf(logp, sizeof logp, "%s\\usm_pc.log", g_dir);
    g_log = fopen(logp, getenv("USM_PC_RETRY") ? "a" : "w");
    census_init(g_dir);
    int ok = map_exe();
    if (ok < 0) {  /* ASLR put something at 0x400000 in this process: a fresh one lays memory out differently */
        const char* r = getenv("USM_PC_RETRY");
        int n = r ? atoi(r) : 0;
        if (n >= 40) { plog("could not reserve the exe's base after %d launches\n", n); return 3; }
        char env[32];
        snprintf(env, sizeof env, "USM_PC_RETRY=%d", n + 1);
        _putenv(env);
        fclose(g_log);
        char self[MAX_PATH + 2];
        snprintf(self, sizeof self, "\"%s\"", argv[0]);
        return (int)_spawnl(_P_WAIT, argv[0], self, NULL);
    }
    if (!ok) return 2;
    for (unsigned k = 0; k < rc_count; k++) rc_on[k] = 1;
    AddVectoredExceptionHandler(1, gate);
    InitializeCriticalSection(&g_acs);
    g_asz = calloc(NSLOT, sizeof *g_asz);
    CreateThread(NULL, 0, mem_report, NULL, 0, NULL);

    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    uint32_t entry = (uint32_t)(uintptr_t)g_base + nt->OptionalHeader.AddressOfEntryPoint;
    int k = find_fn(entry);
    if (k < 0) { plog("the entry point %06x is not translated\n", entry); return 2; }
    plog("starting at the translated entry point %06x\n", entry);
    ((void (*)(void))rc_stub[k])();  /* mainCRTStartup: ends the process through exit() */
    plog("entry point returned\n");
    return 0;
}
