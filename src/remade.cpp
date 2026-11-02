// The table of remade functions: name, original address. Each one is jmp-patched over the original.
#include "remade.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "hook.h"
#include "recomp/rt.h"

uint32_t __thiscall os_file_seek(void* self, int32_t offset, int32_t whence);

#define REMADE_LIST(X) \
    X(os_file_seek, 0x58e020)

#define DEFINE_HITS(name, addr) volatile long remade_hits_##name;
REMADE_LIST(DEFINE_HITS)

namespace {

struct entry {
    const char* name;
    uintptr_t addr;
    void* fn;
    volatile long* hits;
};

#define ENTRY(name, addr) {#name, addr, (void*)&name, &remade_hits_##name},
entry g_table[] = {REMADE_LIST(ENTRY)};

FILE* g_log;
char g_hits_path[MAX_PATH];

// Bulk-lifted functions (src/lifted, generated). Only the ones listed in usm_re_hooks.txt get installed.
extern "C" {
struct lifted_entry { unsigned addr; void* fn; };
extern const lifted_entry lifted_table[];   // fn: the register-preserving wrapper the hook jumps to
extern void* const lifted_impl[];           // the lifted function itself, same index
extern const unsigned lifted_count;
extern volatile long lifted_hits[];
extern unsigned char rc_on[];               // recompiled callers call a function translated only while it is hooked
extern const uint32_t rc_ranges[][3];       // {lo, hi, index}: the code each translation was made from
extern const unsigned rc_nranges;
}
std::vector<unsigned> g_lifted_on;  // indexes into lifted_table that are hooked

// Self-quarantine, so one play session can converge on a bad hook without a human bisecting:
//  - usm_re_deny.txt: addresses that are never hooked again (hex, one per line). Written when a fault or hang is
//    pinned on a lifted function, or when the previous run ended without a clean exit.
//  - usm_re_journal.txt: lifted functions in the order they first ran this session, ending in CLEAN (normal exit)
//    or CULPRIT (a deny was already written precisely). Anything else at the next start means a crash or a
//    kill after a hang, and the last kTail functions to first run are the suspects.
constexpr unsigned kTail = 4;
char g_deny_path[MAX_PATH], g_journal_path[MAX_PATH];
FILE* g_journal;
std::set<unsigned> g_deny;
std::vector<char> g_seen;  // per lifted_table index: already in the journal
CRITICAL_SECTION g_journal_cs;
DWORD g_last_av;  // tick of the last access violation seen

void deny(unsigned addr, const char* why) {
    if (!g_deny.insert(addr).second) return;
    if (FILE* d = std::fopen(g_deny_path, "a")) std::fprintf(d, "%x\n", addr), std::fclose(d);
    std::fprintf(g_log, "QUARANTINE %06x (%s): not hooked from the next launch\n", addr, why);
}

void load_quarantine() {
    GetFullPathNameA("usm_re_deny.txt", MAX_PATH, g_deny_path, nullptr);
    GetFullPathNameA("usm_re_journal.txt", MAX_PATH, g_journal_path, nullptr);
    if (FILE* d = std::fopen(g_deny_path, "r")) {
        char line[64];
        while (std::fgets(line, sizeof line, d))
            if (unsigned a = std::strtoul(line, nullptr, 16)) g_deny.insert(a);
        std::fclose(d);
    }
    std::vector<unsigned> ran;
    bool ended_cleanly = true;
    if (FILE* j = std::fopen(g_journal_path, "r")) {
        char line[64];
        while (std::fgets(line, sizeof line, j)) {
            ended_cleanly = !std::strncmp(line, "CLEAN", 5) || !std::strncmp(line, "CULPRIT", 7);
            // Markers start with a letter that is also a hex digit (strtoul("CRASHED") == 0xC): skip them.
            if (line[0] != 'C')
                if (unsigned a = std::strtoul(line, nullptr, 16)) ran.push_back(a);
        }
        std::fclose(j);
    }
    if (!ended_cleanly && !ran.empty()) {
        std::fprintf(g_log, "previous run did not exit cleanly; last functions to first run:");
        for (size_t i = ran.size() > kTail ? ran.size() - kTail : 0; i < ran.size(); i++) {
            std::fprintf(g_log, " %06x", ran[i]);
            deny(ran[i], "suspect after an unclean exit");
        }
        std::fputc('\n', g_log);
    }
    g_journal = std::fopen(g_journal_path, "w");
    InitializeCriticalSection(&g_journal_cs);
}

// Append lifted functions that have run since the last poll. Cheap: one pass over the hit counters.
void poll_journal() {
    if (!g_journal) return;
    EnterCriticalSection(&g_journal_cs);
    bool any = false;
    for (unsigned i : g_lifted_on)
        if (!g_seen[i] && lifted_hits[i]) g_seen[i] = 1, std::fprintf(g_journal, "%x\n", lifted_table[i].addr), any = true;
    if (any) std::fflush(g_journal);
    LeaveCriticalSection(&g_journal_cs);
}

// Index into lifted_table of the lifted function whose code contains `at` (not the wrapper), or -1.
int lifted_at(uintptr_t at) {
    HMODULE self = nullptr, in = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&lifted_at, &self);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)at, &in) || in != self)
        return -1;
    int best = -1;
    uintptr_t best_fn = 0;
    for (unsigned i = 0; i < lifted_count; i++) {
        uintptr_t fn = uintptr_t(lifted_impl[i]);
        if (fn <= at && fn > best_fn) best_fn = fn, best = int(i);
    }
    return best;
}

// A fault or hang pinned on lifted function `i`: quarantine it, and tell the next start the journal needs no guessing.
void convict(int i, const char* why) {
    deny(lifted_table[i].addr, why);
    if (g_journal) std::fputs("CULPRIT\n", g_journal), std::fflush(g_journal);
}

// Code other mods patched in memory (WidescreenFix): bytes of .text that differ from USM.exe on disk, ignoring
// `skip` (our own hook jmps). A translated function comes from the disk image, so one with a patch inside it must
// stay original or the patch is lost.
std::vector<uint8_t> g_disk_text;
std::vector<unsigned> text_patches(const std::set<unsigned>& skip) {
    const uint8_t* base = (const uint8_t*)GetModuleHandleA(nullptr);
    auto* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* text = IMAGE_FIRST_SECTION(nt);
    if (g_disk_text.empty()) {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (FILE* f = std::fopen(exe, "rb")) {
            g_disk_text.resize(text->SizeOfRawData);
            std::fseek(f, text->PointerToRawData, SEEK_SET);
            if (std::fread(g_disk_text.data(), 1, g_disk_text.size(), f) != g_disk_text.size()) g_disk_text.clear();
            std::fclose(f);
        }
    }
    std::vector<unsigned> out;
    const uint8_t* mem = base + text->VirtualAddress;
    unsigned va = unsigned(uintptr_t(base)) + text->VirtualAddress;
    for (size_t k = 0; k < g_disk_text.size() && k < text->Misc.VirtualSize; k++)
        if (mem[k] != g_disk_text[k] && !skip.count(unsigned(va + k))) out.push_back(unsigned(va + k));
    return out;
}
std::set<unsigned> g_own_patch;  // bytes our hooks overwrite
std::vector<unsigned> g_foreign;  // other mods' patched bytes seen at install

// Index of the translated function whose span [addr, next addr) contains `a`, or -1.
int lifted_span(unsigned a) {
    unsigned lo = 0, hi = lifted_count;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (lifted_table[m].addr <= a) lo = m + 1;
        else hi = m;
    }
    return lo ? int(lo - 1) : -1;
}

// usm_re_hooks.txt: one hex address per line, `all`, or `# comment`. Missing file: no lifted hooks.
void install_lifted() {
    FILE* f = std::fopen("usm_re_hooks.txt", "r");
    if (!f) return;
    std::set<unsigned> want;
    bool all = false;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (!std::strncmp(line, "all", 3)) all = true;
        else if (unsigned a = std::strtoul(line, nullptr, 16)) want.insert(a);
    }
    std::fclose(f);
    for (auto& e : g_table)
        for (unsigned k = 0; k < 5; k++) g_own_patch.insert(unsigned(e.addr) + k);
    g_foreign = text_patches(g_own_patch);
    for (size_t k = 0; k < g_foreign.size(); k++)  // one line per patched run of bytes
        if (!k || g_foreign[k] != g_foreign[k - 1] + 1) std::fprintf(g_log, "foreign patch at %06x\n", g_foreign[k]);
    // every translation made from a patched byte, inlined copies included (spans by start address miss them)
    std::set<int> patched;
    for (unsigned a : g_foreign)
        for (unsigned r = 0; r < rc_nranges && rc_ranges[r][0] <= a; r++)
            if (a <= rc_ranges[r][1]) patched.insert(int(rc_ranges[r][2]));
    if (!g_foreign.empty())
        std::fprintf(g_log, "%u code bytes patched by another mod, in %u translated functions: those stay original\n",
                     unsigned(g_foreign.size()), unsigned(patched.size()));
    unsigned denied = 0;
    for (unsigned i = 0; i < lifted_count; i++)
        if (all || want.erase(lifted_table[i].addr)) {
            if (g_deny.count(lifted_table[i].addr)) { denied++; continue; }
            if (patched.count(int(i))) continue;
            if (rc_standalone_only[i]) continue;  // calls setjmp: its longjmp only comes back in usm_pc.exe
            rc_on[i] = 1;
            g_lifted_on.push_back(i);  // journaled even when not hooked: translated callers run it
            if (!lifted_table[i].fn) continue;  // tiny, a chunk or discovered: translated only for translated callers
            hook::jmp(lifted_table[i].addr, lifted_table[i].fn);
            for (unsigned k = 0; k < 5; k++) g_own_patch.insert(lifted_table[i].addr + k);
        }
    g_seen.assign(lifted_count, 0);
    std::fprintf(g_log, "installed %u lifted functions (%u quarantined)", unsigned(g_lifted_on.size()), denied);
    if (!want.empty()) std::fprintf(g_log, " (%u listed addresses are not in the lifted table)", unsigned(want.size()));
    std::fputc('\n', g_log);
}

// usm_re_hits.txt: every hooked lifted function that has run, with its call count. Rewritten each report.
void write_lifted_hits() {
    if (g_lifted_on.empty()) return;
    FILE* f = std::fopen(g_hits_path, "w");
    if (!f) return;
    unsigned ran = 0;
    for (unsigned i : g_lifted_on)
        if (long h = lifted_hits[i]) std::fprintf(f, "0x%06x %ld\n", lifted_table[i].addr, h), ran++;
    std::fclose(f);
    std::fprintf(g_log, "lifted: %u of %u hooked functions have run\n", ran, unsigned(g_lifted_on.size()));
}

// A fault inside our module is almost always a bad remade/lifted function: name it in the log. Only
// observes (returns CONTINUE_SEARCH), so the game's own exception handling is unchanged.
LONG CALLBACK on_fault(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    // Whatever faulted, record which lifted functions have run so far: a bad lifted function often
    // crashes later in game code, and only the ones that ran can be guilty. At most once a second.
    static DWORD last_dump;
    if (GetTickCount() - last_dump > 1000) {
        last_dump = GetTickCount();
        std::fprintf(g_log, "access violation at %08x (reading/writing %08lx); hits written\n",
                     unsigned(uintptr_t(e->ExceptionRecord->ExceptionAddress)),
                     (unsigned long)e->ExceptionRecord->ExceptionInformation[1]);
        write_lifted_hits();
        std::fflush(g_log);
    }
    poll_journal();
    g_last_av = GetTickCount();
    uintptr_t at = uintptr_t(e->ExceptionRecord->ExceptionAddress);
    // Jumped to a bad address (corrupt return address or function pointer): log the exe-text dwords on the stack,
    // the callers that led here, since the faulting address itself says nothing.
    static DWORD last_stack;
    if (GetTickCount() - last_stack > 1000) {
        last_stack = GetTickCount();
        std::fprintf(g_log, "  fault eip=%08x info0=%u addr=%08lx; stack text dwords:", unsigned(at),
                     (unsigned)e->ExceptionRecord->ExceptionInformation[0], (unsigned long)e->ExceptionRecord->ExceptionInformation[1]);
        const unsigned* sp = (const unsigned*)e->ContextRecord->Esp;
        for (int k = 0; k < 64 && !IsBadReadPtr(sp + k, 4); k++)
            if (sp[k] >= 0x401000 && sp[k] < 0x900000) std::fprintf(g_log, " %06x", sp[k]);
        // Translated code runs on its own host stack; the game's callers are on the guest stack.
        std::fprintf(g_log, "\n  guest stack text dwords:");
        const unsigned* gp = (const unsigned*)uintptr_t(rc_ctx()->esp);
        for (int k = 0; k < 64 && !IsBadReadPtr(gp + k, 4); k++)
            if (gp[k] >= 0x401000 && gp[k] < 0x900000) std::fprintf(g_log, " %06x", gp[k]);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
    int i = lifted_at(at);
    if (i < 0) return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(g_log, "FAULT access violation in lifted %06x + 0x%x, reading/writing %08lx\n", lifted_table[i].addr,
                 unsigned(at - uintptr_t(lifted_impl[i])), (unsigned long)e->ExceptionRecord->ExceptionInformation[1]);
    convict(i, "access violation inside it");
    std::fflush(g_log);
    return EXCEPTION_CONTINUE_SEARCH;
}

// A translated function hit a trap (bad return, int3): quarantine the one containing `at`.
void on_trap(uint32_t at) {
    int best = -1;
    for (unsigned i = 0; i < lifted_count && lifted_table[i].addr <= at; i++) best = int(i);
    if (best < 0) return;
    std::fprintf(g_log, "TRAP at %06x in translated %06x\n", at, lifted_table[best].addr);
    convict(best, "trapped in the translation");
    std::fflush(g_log);
}

HANDLE g_main_thread;
}  // namespace
extern volatile long g_frames;  // frame_counter.cpp
namespace {

// Every 10 s, log hit counts that changed since the last report.
DWORD WINAPI report(void*) {
    long last[sizeof g_table / sizeof *g_table] = {};
    for (int round = 0;; round++) {
        Sleep(10000);
        if (round == 0) {  // a mod that patches after us: its patch inside a translated function is lost
            std::set<unsigned> known(g_own_patch);
            known.insert(g_foreign.begin(), g_foreign.end());
            unsigned late = 0;
            for (unsigned a : text_patches(known))
                if (int i = lifted_span(a); i >= 0 && rc_on[i] && late++ < 20)
                    std::fprintf(g_log, "LATE PATCH at %06x inside translated %06x (patch is lost)\n", a, lifted_table[i].addr);
        }
        bool any = false;
        for (size_t i = 0; i < sizeof g_table / sizeof *g_table; i++) {
            long h = *g_table[i].hits;
            if (h == last[i]) continue;
            std::fprintf(g_log, "%s%s=%ld", any ? " " : "hits: ", g_table[i].name, h);
            last[i] = h;
            any = true;
        }
        if (any) std::fputc('\n', g_log);
        // main-thread CPU time per 10 s: with the frame rate capped, translated vs original costs compare directly
        static long last_frames;
        long fr = g_frames;
        if (fr) std::fprintf(g_log, "frames: %ld in 10 s (%.1f fps)\n", fr - last_frames, (fr - last_frames) / 10.0);
        last_frames = fr;
        static ULONG64 last_cycles;
        ULONG64 cyc = 0;
        if (g_main_thread && QueryThreadCycleTime(g_main_thread, &cyc)) {
            if (last_cycles) std::fprintf(g_log, "main thread: %.2f Gcycles in 10 s\n", (cyc - last_cycles) / 1e9);
            last_cycles = cyc;
        }
        write_lifted_hits();
        std::fflush(g_log);
    }
}


// Hang watchdog: if the main thread's EIP stays in one 0x100-byte block for 4 samples (2 s), log its
// registers and every exe-text-looking dword on its stack (return addresses), then the hits. Once per hang.
// USM_PROFILE=<seconds>: sample the main thread's EIP about 1000 times a second, starting 30 s in, and log where
// the time goes: translated code (our module), each other module, and the hottest USM.exe 256-byte blocks.
DWORD WINAPI profile(void* arg) {
    unsigned secs = unsigned(uintptr_t(arg));
    char dv[8];
    Sleep(GetEnvironmentVariableA("USM_PROFILE_DELAY", dv, sizeof dv) ? std::atoi(dv) * 1000 : 30000);
    timeBeginPeriod(1);
    std::map<std::string, unsigned> by_module;
    std::map<unsigned, unsigned> exe_blocks, self_pcs, exe_pcs;  // self_pcs: offsets into our module, named offline (llvm-nm)
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&profile, &self);
    HMODULE exe = GetModuleHandleA(nullptr);
    unsigned total = 0;
    DWORD end = GetTickCount() + secs * 1000;
    while (GetTickCount() < end) {
        Sleep(1);
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(g_main_thread) == DWORD(-1)) break;
        bool ok = GetThreadContext(g_main_thread, &c);
        ResumeThread(g_main_thread);
        if (!ok) continue;
        total++;
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)uintptr_t(c.Eip), &m);
        char name[MAX_PATH] = "?";
        if (m == self) std::strcpy(name, "[translated + runtime]");
        else if (m) {
            GetModuleFileNameA(m, name, MAX_PATH);
            if (char* b = std::strrchr(name, '\\')) std::memmove(name, b + 1, std::strlen(b));
        }
        by_module[name]++;
        if (m == exe) exe_blocks[c.Eip & ~0xffu]++, exe_pcs[unsigned(c.Eip)]++;
        if (m == self) self_pcs[unsigned(c.Eip - uintptr_t(self))]++;
    }
    timeEndPeriod(1);
    std::fprintf(g_log, "PROFILE %u samples over %u s:\n", total, secs);
    std::vector<std::pair<unsigned, std::string>> mods;
    for (auto& [k, v] : by_module) mods.push_back({v, k});
    std::sort(mods.rbegin(), mods.rend());
    for (auto& [v, k] : mods) std::fprintf(g_log, "  %5.1f%%  %s\n", 100.0 * v / total, k.c_str());
    std::vector<std::pair<unsigned, unsigned>> blocks;
    for (auto& [k, v] : exe_blocks) blocks.push_back({v, k});
    std::sort(blocks.rbegin(), blocks.rend());
    for (size_t i = 0; i < blocks.size() && i < 12; i++)
        std::fprintf(g_log, "  %5.1f%%  USM.exe %06x\n", 100.0 * blocks[i].first / total, blocks[i].second);
    std::fprintf(g_log, "most called native targets (whole run):\n");
    rc_dump_native(g_log, 30);
    // every sampled offset in our module, for tools/recomp/prof_names.py
    if (FILE* f = std::fopen("usm_re_profile.txt", "w")) {
        for (auto& [k, v] : self_pcs) std::fprintf(f, "%x %u\n", k, v);
        for (auto& [k, v] : exe_pcs) std::fprintf(f, "exe %x %u\n", k, v);  // original code, by USM.exe address
        std::fprintf(f, "total %u\n", total);
        std::fclose(f);
    }
    std::fflush(g_log);
    return 0;
}

DWORD WINAPI watch_hang(void*) {
    DWORD last_block = 0, same = 0;
    bool logged = false;
    for (;;) {
        Sleep(500);
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(g_main_thread) == DWORD(-1)) return 0;
        bool ok = GetThreadContext(g_main_thread, &c);
        unsigned stack[96] = {};
        if (ok) {
            SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), (void*)c.Esp, stack, sizeof stack, &got);
        }
        ResumeThread(g_main_thread);
        poll_journal();
        if (!ok) continue;
        DWORD block = c.Eip >> 8;
        same = block == last_block ? same + 1 : 0;
        last_block = block;
        if (same < 4) { logged = false; continue; }
        if (logged) continue;
        logged = true;
        std::fprintf(g_log, "HANG? main thread stuck near eip=%08lx esp=%08lx ebp=%08lx eax=%08lx ecx=%08lx edx=%08lx\n  text dwords on stack:",
                     c.Eip, c.Esp, c.Ebp, c.Eax, c.Ecx, c.Edx);
        for (unsigned v : stack)
            if (v >= 0x401000 && v < 0x900000) std::fprintf(g_log, " %06x", v);
        std::fputc('\n', g_log);
        // Spinning inside a lifted function is a precise verdict; spinning in game code leaves the journal to guess.
        if (int i = lifted_at(c.Eip); i >= 0) {
            std::fprintf(g_log, "  eip is inside lifted %06x\n", lifted_table[i].addr);
            convict(i, "main thread hung inside it");
        }
        write_lifted_hits();
        std::fflush(g_log);
    }
}

}  // namespace

void install_remade() {
    g_log = std::fopen("usm_re.log", "a");  // opened during DllMain, while the loader's cwd is Scripts
    char v[4];
    if (GetEnvironmentVariableA("USM_NO_REMADE", v, sizeof v) && v[0] == '1') {
        if (g_log) std::fprintf(g_log, "USM_NO_REMADE=1: original code only\n"), std::fflush(g_log);
        return;
    }
    for (auto& e : g_table) hook::jmp(e.addr, e.fn);
    if (!g_log) return;
    std::fprintf(g_log, "installed %u remade functions\n", unsigned(sizeof g_table / sizeof *g_table));
    GetFullPathNameA("usm_re_hits.txt", MAX_PATH, g_hits_path, nullptr);  // cwd changes after DllMain
    load_quarantine();
    install_lifted();
    AddVectoredExceptionHandler(1, on_fault);
    rc_on_trap = on_trap;
    std::fflush(g_log);
    CreateThread(nullptr, 0, report, nullptr, 0, nullptr);
    // DllMain of an .asi runs on the game's main thread
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    if (g_main_thread) CreateThread(nullptr, 0, watch_hang, nullptr, 0, nullptr);
    if (g_main_thread && GetEnvironmentVariableA("USM_PROFILE", v, sizeof v))
        CreateThread(nullptr, 0, profile, (void*)uintptr_t(std::atoi(v)), 0, nullptr);
}

void shutdown_remade() {
    if (!g_journal) return;
    poll_journal();
    // The game's own crash handler exits through ExitProcess, so reaching here is not proof of a clean exit:
    // an access violation in the last 5 s means this was a crash.
    if (g_last_av && GetTickCount() - g_last_av < 5000) {
        std::fputs("CRASHED\n", g_journal);
    } else {
        std::fputs("CLEAN\n", g_journal);
    }
    std::fflush(g_journal);
}
