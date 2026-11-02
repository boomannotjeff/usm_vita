// Research probe: do pack blobs get pointer fixups after they are read?
// Patches USM.exe's import slots for ReadFile / ReadFileEx so every file read the game makes is seen,
// snapshots each large read, and 5 s later diffs the buffer against the snapshot. Writes usm_pack_probe.log
// next to the .asi. Only installed when USM_PACK_PROBE=1 is set in the environment.
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

constexpr uintptr_t kIatReadFile = 0x86f094;     // kernel32!ReadFile slot in USM.exe
constexpr uintptr_t kIatReadFileEx = 0x86f154;   // kernel32!ReadFileEx slot
constexpr DWORD kMinSize = 0x1000;               // headers and small reads are not interesting
constexpr size_t kMaxSnapshotBytes = 256u << 20;

struct Snap {
    uint8_t* buf;
    DWORD size;
    DWORD pos;                                    // file offset of the read
    char name[64];
    DWORD tick;
    std::vector<uint8_t> copy;
};

decltype(&ReadFile) g_read;
decltype(&ReadFileEx) g_read_ex;
CRITICAL_SECTION g_lock;
std::vector<Snap> g_snaps;
size_t g_bytes;
FILE* g_log;

void file_name(HANDLE h, char* out, size_t n) {
    out[0] = 0;
    char full[MAX_PATH];
    DWORD len = GetFinalPathNameByHandleA(h, full, MAX_PATH, 0);
    if (!len || len >= MAX_PATH) return;
    const char* s = std::strrchr(full, '\\');
    std::snprintf(out, n, "%s", s ? s + 1 : full);
}

void snapshot(HANDLE h, void* buf, DWORD size, DWORD pos) {
    if (size < kMinSize || !buf) return;
    EnterCriticalSection(&g_lock);
    if (g_bytes + size <= kMaxSnapshotBytes) {
        Snap s{(uint8_t*)buf, size, pos, {}, GetTickCount(), {}};
        file_name(h, s.name, sizeof s.name);
        s.copy.assign((uint8_t*)buf, (uint8_t*)buf + size);
        g_bytes += size;
        g_snaps.push_back(std::move(s));
    }
    LeaveCriticalSection(&g_lock);
}

BOOL WINAPI hk_read(HANDLE h, LPVOID buf, DWORD size, LPDWORD got, LPOVERLAPPED ov) {
    DWORD pos = ov ? ov->Offset : SetFilePointer(h, 0, nullptr, FILE_CURRENT);
    BOOL ok = g_read(h, buf, size, got, ov);
    if (ok && got) {
        snapshot(h, buf, *got, pos);
    } else if (ov && !ok && GetLastError() == ERROR_IO_PENDING) {
        EnterCriticalSection(&g_lock);
        std::fprintf(g_log, "async ReadFile pending, size %lu: not captured\n", size);
        LeaveCriticalSection(&g_lock);
    }
    return ok;
}

// ReadFileEx completes later through a completion routine; wrap it to capture the finished buffer.
struct Pending { LPOVERLAPPED_COMPLETION_ROUTINE cb; HANDLE h; void* buf; };
std::map<LPOVERLAPPED, Pending> g_pending;

void WINAPI done_ex(DWORD err, DWORD got, LPOVERLAPPED ov) {
    EnterCriticalSection(&g_lock);
    Pending p = g_pending[ov];
    g_pending.erase(ov);
    LeaveCriticalSection(&g_lock);
    if (!err) snapshot(p.h, p.buf, got, ov->Offset);
    p.cb(err, got, ov);
}

BOOL WINAPI hk_read_ex(HANDLE h, LPVOID buf, DWORD size, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE cb) {
    EnterCriticalSection(&g_lock);
    g_pending[ov] = {cb, h, buf};
    LeaveCriticalSection(&g_lock);
    return g_read_ex(h, buf, size, ov, done_ex);
}

// Our own snapshot copies, taken under the lock before scanning. shortcut: copies made during a scan are
// missing, so a match inside one could be misreported as the destination; rare, fine for a probe.
std::vector<std::pair<const uint8_t*, size_t>> g_copies;

bool in_any_copy(const uint8_t* p) {
    for (auto& [a, n] : g_copies)
        if (p >= a && p < a + n) return true;
    return false;
}

constexpr size_t kScanChunk = 1u << 20;
uint8_t* g_scan;                                  // kScanChunk + 64 bytes; memory is copied here, never read in place

// Copy memory that other threads may free at any moment; fails instead of faulting.
bool safe_copy(const void* from, void* to, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), from, to, n, &got) && got == n;
}

// Where did these 64 bytes end up? Scans committed memory, ignoring the staging buffer and our own copies.
const uint8_t* find_bytes(const uint8_t* pat, const Snap& s) {
    MEMORY_BASIC_INFORMATION mi;
    for (const uint8_t* a = (const uint8_t*)0x10000; a < (const uint8_t*)0x7fff0000; a = (const uint8_t*)mi.BaseAddress + mi.RegionSize) {
        if (!VirtualQuery(a, &mi, sizeof mi)) break;
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & 0xEE)) continue;
        const uint8_t* lo = (const uint8_t*)mi.BaseAddress;
        const uint8_t* end = lo + mi.RegionSize;
        if (lo == g_scan) continue;
        for (const uint8_t* c = lo; c + 64 <= end; c += kScanChunk) {
            size_t n = std::min<size_t>(kScanChunk + 60, end - c);
            if (!safe_copy(c, g_scan, n)) continue;
            for (size_t i = 0; i + 64 <= n; i += 4) {
                if (*(const uint32_t*)(g_scan + i) != *(const uint32_t*)pat || std::memcmp(g_scan + i, pat, 64)) continue;
                const uint8_t* p = c + i;
                if ((p >= s.buf && p < s.buf + s.size) || in_any_copy(p)) continue;
                return p;
            }
        }
    }
    return nullptr;
}

bool boring(const uint8_t* p) {   // all-equal dwords (zeros, padding) match everywhere
    for (int i = 4; i < 64; i += 4)
        if (*(const uint32_t*)(p + i) != *(const uint32_t*)p) return false;
    return true;
}

// The read goes into a reused staging buffer, so find where the chunk was copied to (dest), then diff.
// A pointer fixup turns a stored file offset `o` into `pack_base + o` (delta == pack_base = dest - pos),
// or a chunk-relative offset into `dest + o` (delta == dest).
void diff(const Snap& s) {
    const uint8_t* dest = nullptr;
    unsigned probe_off = 0;
    int tries = 0;
    for (unsigned off = 0x40; off + 64 <= s.size && !dest && tries < 3; off += s.size / 8 & ~3u) {
        if (boring(s.copy.data() + off)) continue;
        tries++;
        if (const uint8_t* p = find_bytes(s.copy.data() + off, s)) dest = p - off, probe_off = off;
    }
    if (!dest) {
        std::fprintf(g_log, "%-32s pos %08lx size %8lu: copy not found (compressed, freed, or rewritten)\n", s.name, s.pos, s.size);
        return;
    }
    std::vector<uint8_t> cur(s.size);
    if (!safe_copy(dest, cur.data(), s.size)) {
        std::fprintf(g_log, "%-32s pos %08lx size %8lu: dest %p not fully mapped\n", s.name, s.pos, s.size, dest);
        return;
    }
    uint32_t pack_base = uint32_t(uintptr_t(dest)) - s.pos;
    unsigned n = s.size / 4, changed = 0, rel_pack = 0, rel_chunk = 0, small_to_ptr = 0;
    std::map<uint32_t, unsigned> deltas;
    const uint32_t* now = (const uint32_t*)cur.data();
    const uint32_t* was = (const uint32_t*)s.copy.data();
    uint32_t first_off = 0;
    for (unsigned i = 0; i < n; i++) {
        if (now[i] == was[i]) continue;
        if (!changed) first_off = i * 4;
        changed++;
        uint32_t d = now[i] - was[i];
        deltas[d]++;
        if (d == pack_base) rel_pack++;
        if (d == uint32_t(uintptr_t(dest))) rel_chunk++;
        if (was[i] < 0x2000000 && now[i] >= 0x10000) small_to_ptr++;
    }
    uint32_t top = 0;
    unsigned top_n = 0;
    for (auto& [d, c] : deltas)
        if (c > top_n) top = d, top_n = c;
    std::fprintf(g_log,
                 "%-32s pos %08lx size %8lu dest %p (found via +%x): changed %u/%u dwords, delta==pack_base %u, delta==dest %u, small->ptr %u, top delta %08x x%u, first at +%x\n",
                 s.name, s.pos, s.size, dest, probe_off, changed, n, rel_pack, rel_chunk, small_to_ptr, top, top_n, first_off);
}

DWORD WINAPI watcher(void*) {
    for (;;) {
        Sleep(2000);
        std::vector<Snap> due;
        EnterCriticalSection(&g_lock);
        DWORD now = GetTickCount();
        for (size_t i = 0; i < g_snaps.size();) {
            if (now - g_snaps[i].tick >= 5000) {   // give the loader time to finish fixing the blob up
                g_bytes -= g_snaps[i].size;
                due.push_back(std::move(g_snaps[i]));
                g_snaps.erase(g_snaps.begin() + i);
            } else {
                i++;
            }
        }
        g_copies.clear();
        for (auto* v : {&g_snaps, &due})
            for (auto& s : *v) g_copies.push_back({s.copy.data(), s.copy.size()});
        LeaveCriticalSection(&g_lock);
        // Scanning takes seconds; doing it under the lock stalled the game's reads until Windows killed it.
        for (auto& s : due) diff(s);
        std::fflush(g_log);
    }
}

void patch_slot(uintptr_t slot, void* to, void** orig) {
    DWORD old;
    VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old);
    *orig = *(void**)slot;
    *(void**)slot = to;
    VirtualProtect((void*)slot, 4, old, &old);
}

}  // namespace

void install_pack_probe() {
    char v[8];
    if (!GetEnvironmentVariableA("USM_PACK_PROBE", v, sizeof v) || v[0] != '1') return;
    g_log = std::fopen("usm_pack_probe.log", "w");
    if (!g_log) return;
    InitializeCriticalSection(&g_lock);
    g_scan = (uint8_t*)VirtualAlloc(nullptr, kScanChunk + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    patch_slot(kIatReadFile, (void*)&hk_read, (void**)&g_read);
    patch_slot(kIatReadFileEx, (void*)&hk_read_ex, (void**)&g_read_ex);
    CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);
    std::fprintf(g_log, "pack probe installed on ReadFile/ReadFileEx imports\n");
    std::fflush(g_log);
}
