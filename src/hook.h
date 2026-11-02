#pragma once
#include <cstdint>

namespace hook {

// Overwrite the 5 bytes at `at` (an address in USM.exe) with `jmp to`.
void jmp(uintptr_t at, void* to);

}
