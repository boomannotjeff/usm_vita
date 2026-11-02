#include <windows.h>
#include <cstdio>

#include "remade.h"

// Entry point RVA of the USM.exe mapped in docs/re-notes.md. Any other exe gets no patches.
static constexpr DWORD kUsmEntryRva = 0x4223d6;

void install_pack_probe();  // pack_probe.cpp; no-op unless USM_PACK_PROBE=1
void install_frame_counter();  // frame_counter.cpp

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) shutdown_remade();
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    auto base = (const BYTE*)GetModuleHandleA(nullptr);
    auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    bool match = nt->OptionalHeader.AddressOfEntryPoint == kUsmEntryRva;

    if (FILE* f = std::fopen("usm_re.log", "w")) {
        std::fprintf(f, "usm_re loaded, exe %s\n", match ? "matches" : "UNKNOWN - hooks off");
        std::fclose(f);
    }
    if (!match) return TRUE;

    install_frame_counter();
    install_remade();
    install_pack_probe();
    return TRUE;
}
