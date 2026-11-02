#include "hook.h"

#include <windows.h>
#include <cstring>

namespace hook {

void jmp(uintptr_t at, void* to) {
    uint8_t patch[5] = {0xE9};
    int32_t rel = int32_t(uintptr_t(to) - (at + 5));
    std::memcpy(patch + 1, &rel, 4);

    DWORD old;
    VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy((void*)at, patch, 5);
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}

}
