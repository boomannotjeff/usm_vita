// Counts presented frames, to measure speed with the frame cap off: the game's Direct3DCreate9 import is
// redirected so the device's Present goes through a counter. Works the same with and without translated code.
#include <windows.h>

#include <cstdint>

volatile long g_frames;

namespace {

using create9_fn = void*(WINAPI*)(UINT);
using create_device_fn = HRESULT(WINAPI*)(void*, UINT, UINT, HWND, DWORD, void*, void**);
using present_fn = HRESULT(WINAPI*)(void*, const RECT*, const RECT*, HWND, const void*);

create9_fn g_create9;
create_device_fn g_create_device;
present_fn g_present;

// Replace vtable slot `i` of COM object `obj`; returns the old entry.
void* patch_slot(void* obj, int i, void* fn) {
    void** vt = *(void***)obj;
    DWORD old;
    VirtualProtect(&vt[i], sizeof(void*), PAGE_READWRITE, &old);
    void* prev = vt[i];
    vt[i] = fn;
    VirtualProtect(&vt[i], sizeof(void*), old, &old);
    return prev;
}

HRESULT WINAPI my_present(void* dev, const RECT* a, const RECT* b, HWND w, const void* r) {
    InterlockedIncrement(&g_frames);
    return g_present(dev, a, b, w, r);
}

HRESULT WINAPI my_create_device(void* d3d, UINT adapter, UINT type, HWND w, DWORD flags, void* pp, void** dev) {
    HRESULT hr = g_create_device(d3d, adapter, type, w, flags, pp, dev);
    if (SUCCEEDED(hr) && dev && *dev && !g_present) g_present = (present_fn)patch_slot(*dev, 17, (void*)&my_present);
    return hr;
}

void* WINAPI my_create9(UINT sdk) {
    void* d3d = g_create9(sdk);
    if (d3d && !g_create_device) g_create_device = (create_device_fn)patch_slot(d3d, 16, (void*)&my_create_device);
    return d3d;
}

}  // namespace

// The USM.exe import slot of d3d9!Direct3DCreate9 (cfg.txt: I 0086f550 D3D9.DLL Direct3DCreate9).
void install_frame_counter() {
    auto slot = (void**)0x86f550;
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    g_create9 = (create9_fn)*slot;
    *slot = (void*)&my_create9;
    VirtualProtect(slot, sizeof(void*), old, &old);
}
