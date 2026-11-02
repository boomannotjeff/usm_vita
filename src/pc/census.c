/* Census mode (USM_PC_CENSUS=1): records every import and COM method the game calls, and every interface it asks
   QueryInterface for, into usm_pc_census.txt. That is the list the Vita shims have to cover
   (tools/recomp/census_diff.py compares the two).
   Imports: each IAT slot points to a thunk that sets a flag and jumps on. COM: the DLLs' vtables are shared by all
   objects of a class, so when the game makes its root objects (Direct3DCreate9, DirectSoundCreate8,
   DirectInput8Create) we make one of each kind of child object ourselves, then point every slot of each vtable at a
   thunk. QueryInterface is wrapped in C to log the IID. */
#define INITGUID
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#include <d3d9.h>
#include <dinput.h>
#include <dsound.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "census.h"

int g_census;

typedef struct {
    uint8_t code[16];  /* mov byte ptr [hit], 1; jmp dword ptr [real] */
    void* real;
    const char* name;  /* "dll!name", or the interface */
    int index;         /* vtable slot, -1 for an import */
    volatile uint8_t hit;
} Thunk;
#define MAX_THUNKS 8192
static Thunk* g_thunk;
static volatile LONG g_nthunks;

typedef struct { void** vt; const char* iface; void* qi; } Vt;
static Vt g_vt[256];
static int g_nvt;
static CRITICAL_SECTION g_cs;

#define MAX_IIDS 128
static struct { const char* iface; GUID iid; } g_iid[MAX_IIDS];
static int g_niid;

static char g_path[MAX_PATH];

static void* thunk(const char* name, int index, void* real) {
    LONG k = InterlockedIncrement(&g_nthunks) - 1;
    if (k >= MAX_THUNKS) return real;
    Thunk* t = &g_thunk[k];
    t->real = real, t->name = name, t->index = index;
    uint8_t* c = t->code;
    void* hit = (void*)&t->hit;
    void* real_slot = &t->real;
    c[0] = 0xc6, c[1] = 0x05, memcpy(c + 2, &hit, 4), c[6] = 1;  /* mov byte ptr [hit], 1 */
    c[7] = 0xff, c[8] = 0x25, memcpy(c + 9, &real_slot, 4);      /* jmp dword ptr [real] */
    return t->code;
}

void* census_import(const char* dll, const char* name, void* real) {
    if (!g_census || !real) return real;
    char* n = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(n, "%s!%s", dll, name);
    CharLowerBuffA(n, (DWORD)strlen(dll));
    return thunk(n, -1, real);
}

static void write_census(void) {
    FILE* f = fopen(g_path, "w");
    if (!f) return;
    EnterCriticalSection(&g_cs);
    for (LONG i = 0; i < g_nthunks && i < MAX_THUNKS; i++) {
        Thunk* t = &g_thunk[i];
        if (!t->hit) continue;
        if (t->index < 0) fprintf(f, "import %s\n", t->name);
        else fprintf(f, "com %s %d\n", t->name, t->index);
    }
    for (int i = 0; i < g_niid; i++) {
        GUID* g = &g_iid[i].iid;
        fprintf(f, "iid %s %08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", g_iid[i].iface, g->Data1, g->Data2,
                g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
                g->Data4[7]);
    }
    LeaveCriticalSection(&g_cs);
    fclose(f);
}

static DWORD WINAPI writer(LPVOID unused) {
    for (;;) Sleep(5000), write_census();
    return 0;
}

/* ---- vtables ---- */
static Vt* find_vt(void** vt) {
    for (int i = 0; i < g_nvt; i++)
        if (g_vt[i].vt == vt) return &g_vt[i];
    return NULL;
}

static HRESULT WINAPI qi_wrap(IUnknown* self, REFIID iid, void** out) {
    EnterCriticalSection(&g_cs);
    Vt* v = find_vt(*(void***)self);
    int seen = 0;
    for (int i = 0; i < g_niid; i++)
        if (g_iid[i].iface == v->iface && IsEqualGUID(&g_iid[i].iid, iid)) seen = 1;
    if (!seen && g_niid < MAX_IIDS) g_iid[g_niid].iface = v->iface, g_iid[g_niid++].iid = *iid;
    void* real = v->qi;
    LeaveCriticalSection(&g_cs);
    return ((HRESULT(WINAPI*)(IUnknown*, REFIID, void**))real)(self, iid, out);
}

/* n: the method count of `iface` (com_gen.c); the slots are rewritten once per vtable */
static void patch(void* obj, const char* iface, int n) {
    if (!obj) return;
    void** vt = *(void***)obj;
    EnterCriticalSection(&g_cs);
    if (find_vt(vt) || g_nvt == sizeof g_vt / sizeof *g_vt) { LeaveCriticalSection(&g_cs); return; }
    Vt* v = &g_vt[g_nvt++];
    v->vt = vt, v->iface = iface, v->qi = vt[0];
    DWORD old;
    VirtualProtect(vt, n * sizeof(void*), PAGE_READWRITE, &old);
    for (int i = 0; i < n; i++) vt[i] = thunk(iface, i, i ? vt[i] : (void*)qi_wrap);
    VirtualProtect(vt, n * sizeof(void*), old, &old);
    LeaveCriticalSection(&g_cs);
}

/* ---- d3d9: the device is patched when the game makes it, along with one object of every kind it can create ---- */
typedef HRESULT(WINAPI* create_device_fn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                          IDirect3DDevice9**);
static create_device_fn g_create_device;

static void probe_device(IDirect3DDevice9* d) {
    static const DWORD vs[] = {0xfffe0101, 0x0000001f, 0x80000000, 0x900f0000, 0x00000001, 0xc00f0000, 0x90e40000, 0x0000ffff};
    static const DWORD ps[] = {0xffff0101, 0x00000001, 0x800f0000, 0x90e40000, 0x0000ffff};
    static const D3DVERTEXELEMENT9 decl[] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    struct { void* o; const char* iface; int n; } p[12] = {0};
    int k = 0;
    IDirect3DTexture9* tex = NULL;
    IDirect3DDevice9_CreateTexture(d, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
    p[k++] = (typeof(*p)){tex, "IDirect3DTexture9", 22};
    IDirect3DSurface9* surf = NULL;
    if (tex) IDirect3DTexture9_GetSurfaceLevel(tex, 0, &surf);
    p[k++] = (typeof(*p)){surf, "IDirect3DSurface9", 17};
    IDirect3DCubeTexture9* cube = NULL;
    IDirect3DDevice9_CreateCubeTexture(d, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL);
    p[k++] = (typeof(*p)){cube, "IDirect3DCubeTexture9", 22};
    IDirect3DVolumeTexture9* vol = NULL;
    IDirect3DDevice9_CreateVolumeTexture(d, 8, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vol, NULL);
    p[k++] = (typeof(*p)){vol, "IDirect3DVolumeTexture9", 22};
    IDirect3DVertexBuffer9* vb = NULL;
    IDirect3DDevice9_CreateVertexBuffer(d, 256, 0, 0, D3DPOOL_MANAGED, &vb, NULL);
    p[k++] = (typeof(*p)){vb, "IDirect3DVertexBuffer9", 14};
    IDirect3DIndexBuffer9* ib = NULL;
    IDirect3DDevice9_CreateIndexBuffer(d, 256, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL);
    p[k++] = (typeof(*p)){ib, "IDirect3DIndexBuffer9", 14};
    IDirect3DVertexShader9* v = NULL;
    IDirect3DDevice9_CreateVertexShader(d, vs, &v);
    p[k++] = (typeof(*p)){v, "IDirect3DVertexShader9", 5};
    IDirect3DPixelShader9* f = NULL;
    IDirect3DDevice9_CreatePixelShader(d, ps, &f);
    p[k++] = (typeof(*p)){f, "IDirect3DPixelShader9", 5};
    IDirect3DVertexDeclaration9* dc = NULL;
    IDirect3DDevice9_CreateVertexDeclaration(d, decl, &dc);
    p[k++] = (typeof(*p)){dc, "IDirect3DVertexDeclaration9", 5};
    IDirect3DQuery9* q = NULL;
    IDirect3DDevice9_CreateQuery(d, D3DQUERYTYPE_EVENT, &q);
    p[k++] = (typeof(*p)){q, "IDirect3DQuery9", 8};
    IDirect3DStateBlock9* sb = NULL;
    IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &sb);
    p[k++] = (typeof(*p)){sb, "IDirect3DStateBlock9", 6};
    IDirect3DSwapChain9* sc = NULL;
    IDirect3DDevice9_GetSwapChain(d, 0, &sc);
    p[k++] = (typeof(*p)){sc, "IDirect3DSwapChain9", 10};
    /* released before patching, so these calls aren't counted; the vtables live on in the DLL */
    void* vts[12];
    for (int i = 0; i < k; i++) vts[i] = p[i].o ? *(void**)p[i].o : NULL;
    for (int i = k - 1; i >= 0; i--)
        if (p[i].o) IUnknown_Release((IUnknown*)p[i].o);
    for (int i = 0; i < k; i++)
        if (vts[i]) patch(&vts[i], p[i].iface, p[i].n);
    patch(d, "IDirect3DDevice9", 119);
}

static HRESULT WINAPI create_device(IDirect3D9* self, UINT a, D3DDEVTYPE t, HWND w, DWORD fl,
                                    D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT r = g_create_device(self, a, t, w, fl, pp, out);
    if (r >= 0 && *out) probe_device(*out);
    return r;
}

typedef IDirect3D9*(WINAPI* d3dcreate_fn)(UINT);
static d3dcreate_fn g_d3dcreate;
static IDirect3D9* WINAPI my_Direct3DCreate9(UINT v) {
    IDirect3D9* d = g_d3dcreate(v);
    if (d && !find_vt(*(void***)d)) {
        void** vt = *(void***)d;
        g_create_device = (create_device_fn)vt[16];
        DWORD old;
        VirtualProtect(&vt[16], sizeof(void*), PAGE_READWRITE, &old);
        vt[16] = (void*)create_device;
        VirtualProtect(&vt[16], sizeof(void*), old, &old);
        patch(d, "IDirect3D9", 17);
    }
    return d;
}

/* ---- dsound ---- */
typedef HRESULT(WINAPI* dscreate_fn)(LPCGUID, IDirectSound8**, IUnknown*);
static dscreate_fn g_dscreate;
static HRESULT WINAPI my_DirectSoundCreate8(LPCGUID g, IDirectSound8** out, IUnknown* outer) {
    HRESULT r = g_dscreate(g, out, outer);
    if (r < 0 || !*out || find_vt(*(void***)*out)) return r;
    IDirectSound8* ds;
    if (g_dscreate(g, &ds, NULL) >= 0) {
        IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
        /* secondary first: if both kinds share a vtable it must be patched with IDirectSoundBuffer8's length */
        struct { void* o; const char* iface; int n; } p[6] = {0};
        int k = 0;
        WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
        DSBUFFERDESC d2 = {sizeof d2, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY |
                          DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2, 4096, 0, &wf};
        IDirectSoundBuffer* sec = NULL;
        IDirectSound8_CreateSoundBuffer(ds, &d2, &sec, NULL);
        IDirectSoundBuffer8* sec8 = NULL;
        if (sec) IDirectSoundBuffer_QueryInterface(sec, &IID_IDirectSoundBuffer8, (void**)&sec8);
        p[k++] = (typeof(*p)){sec8, "IDirectSoundBuffer8", 24};
        IDirectSound3DBuffer* b3 = NULL;
        if (sec) IDirectSoundBuffer_QueryInterface(sec, &IID_IDirectSound3DBuffer, (void**)&b3);
        p[k++] = (typeof(*p)){b3, "IDirectSound3DBuffer", 21};
        IDirectSoundNotify* nt = NULL;
        if (sec) IDirectSoundBuffer_QueryInterface(sec, &IID_IDirectSoundNotify, (void**)&nt);
        p[k++] = (typeof(*p)){nt, "IDirectSoundNotify", 4};
        DSBUFFERDESC d = {sizeof d, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D};
        IDirectSoundBuffer* prim = NULL;
        IDirectSound8_CreateSoundBuffer(ds, &d, &prim, NULL);
        p[k++] = (typeof(*p)){prim, "IDirectSoundBuffer", 21};
        IDirectSound3DListener* lis = NULL;
        if (prim) IDirectSoundBuffer_QueryInterface(prim, &IID_IDirectSound3DListener, (void**)&lis);
        p[k++] = (typeof(*p)){lis, "IDirectSound3DListener", 18};
        void* vts[6];
        for (int i = 0; i < k; i++) vts[i] = p[i].o ? *(void**)p[i].o : NULL;
        for (int i = k - 1; i >= 0; i--)
            if (p[i].o) IUnknown_Release((IUnknown*)p[i].o);
        if (sec) IDirectSoundBuffer_Release(sec);
        IDirectSound8_Release(ds);
        for (int i = 0; i < k; i++)
            if (vts[i]) patch(&vts[i], p[i].iface, p[i].n);
    }
    patch(*out, "IDirectSound8", 12);
    return r;
}

/* ---- dinput: main.c's DirectInput8Create wrapper hands us the object ---- */
void census_dinput(void* di) {
    if (!g_census || !di || find_vt(*(void***)di)) return;
    IDirectInput8A* d = di;
    IDirectInputDevice8A* kb = NULL;
    IDirectInput8_CreateDevice(d, &GUID_SysKeyboard, &kb, NULL);
    void* vt = kb ? *(void**)kb : NULL;
    if (kb) IDirectInputDevice8_Release(kb);
    if (vt) patch(&vt, "IDirectInputDevice8A", 32);
    patch(di, "IDirectInput8A", 11);
}

void* census_hook(const char* dll, const char* name, void* real) {
    if (!g_census || !real) return NULL;
    if (!_stricmp(dll, "D3D9.DLL") && !strcmp(name, "Direct3DCreate9"))
        return g_d3dcreate = real, census_import(dll, name, (void*)my_Direct3DCreate9);
    if (!_stricmp(dll, "DSOUND.DLL") && !strcmp(name, "DirectSoundCreate8"))
        return g_dscreate = real, census_import(dll, name, (void*)my_DirectSoundCreate8);
    return NULL;
}

void census_init(const char* dir) {
    if (!getenv("USM_PC_CENSUS")) return;
    g_census = 1;
    InitializeCriticalSection(&g_cs);
    g_thunk = VirtualAlloc(NULL, MAX_THUNKS * sizeof(Thunk), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    snprintf(g_path, sizeof g_path, "%s\\usm_pc_census.txt", dir);
    CreateThread(NULL, 0, writer, NULL, 0, NULL);
}
