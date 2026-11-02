/* The small DLLs: winmm timers, the registry (a text file, see below), COM (only DxDiag, for
   the DirectX version check), shell folders and Bink (movies: bink.c).
   D3D9, DirectSound and DirectInput have their own files. */
#include <psp2/kernel/processmgr.h>
#include <stdlib.h>

#include "vita.h"
#include "vita_util.h"

#define S(i) ((char*)H(ARG(i)))

/* ---- the registry: values kept in ux0:data/usm/registry.txt, one per line: path <tab> type <tab> hex bytes. The game
   keeps its options there (HKCU\Software\...\Settings: Resolution, DetailedShadows, Distance, ...) ---- */
#include <pthread.h>
#include <stdio.h>
#include <strings.h>

typedef struct {
    char path[200];
    uint32_t type, size;
    uint8_t data[260];
} RegVal;
static RegVal g_reg[256];
static int g_nreg = -1;
static char g_keys[64][200];  /* open keys: handle 0x1000 + i */
static pthread_mutex_t g_reg_m = PTHREAD_MUTEX_INITIALIZER;
#define REG_FILE USM_DIR "registry.txt"

static RegVal* reg_find(const char* path);
static void reg_defaults(void);
static void reg_load(void) {
    if (g_nreg >= 0) return;
    g_nreg = 0;
    FILE* f = fopen(REG_FILE, "r");
    if (!f) { reg_defaults(); return; }
    char line[800];
    while (g_nreg < 256 && fgets(line, sizeof line, f)) {
        RegVal* v = &g_reg[g_nreg];
        char* t1 = strchr(line, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2 || t1 - line >= (int)sizeof v->path) continue;
        memcpy(v->path, line, t1 - line), v->path[t1 - line] = 0;
        v->type = (uint32_t)strtoul(t1 + 1, NULL, 10), v->size = 0;
        for (char* h = t2 + 1; h[0] && h[1] && h[0] != '\n' && v->size < sizeof v->data; h += 2) {
            char b[3] = {h[0], h[1], 0};
            v->data[v->size++] = (uint8_t)strtoul(b, NULL, 16);
        }
        g_nreg++;
    }
    fclose(f);
    reg_defaults();
}
/* the Vita's defaults for the detail options, as the in-game Options menu writes them; less to draw = less game work */
static void reg_defaults(void) {
    static const struct { const char* name; uint32_t type, size; const char* data; } D[] = {
        {"Resolution", 1, 8, "720x408"},  /* 3/4 of the screen each way, scaled up: the 3D scene is the GPU's cost */
        {"DetailedShadows", 4, 4, "\0\0\0\0"},
        {"DistanceClipping", 4, 4, "\1\0\0\0"},
        {"Distance", 4, 4, "\1\0\0\0"},
        {"ChromeEffect", 4, 4, "\0\0\0\0"},  /* the "smshiny" reflection pass (0x76f8e0) */
    };
    for (unsigned i = 0; i < sizeof D / sizeof *D && g_nreg < 256; i++) {
        char p[200];
        snprintf(p, sizeof p, "HKCU\\Software\\Activision\\Ultimate Spider-Man\\Settings\\%s", D[i].name);
        RegVal* v = reg_find(p);
        if (v && !(i == 0 && v->size >= 7 && !memcmp(v->data, "960x544", 7))) continue;  /* the old default goes */
        if (!v) v = &g_reg[g_nreg++];
        strcpy(v->path, p), v->type = D[i].type, v->size = D[i].size;
        memcpy(v->data, D[i].data, D[i].size);
    }
    /* ux0:data/usm/resolution.txt ("640x362"): the 3D resolution, over whatever the Options menu saved (read at start) */
    FILE* rf = fopen(USM_DIR "resolution.txt", "r");
    unsigned rw, rh;
    if (rf) {
        if (fscanf(rf, "%ux%u", &rw, &rh) == 2 && rw >= 160 && rw <= 960 && rh >= 90 && rh <= 544) {
            static const char* path = "HKCU\\Software\\Activision\\Ultimate Spider-Man\\Settings\\Resolution";
            RegVal* v = reg_find(path);
            if (!v && g_nreg < 256) v = &g_reg[g_nreg++], strcpy(v->path, path);
            if (v) {
                v->type = 1, v->size = (uint32_t)snprintf((char*)v->data, sizeof v->data, "%ux%u", rw, rh) + 1;
                extern uint32_t g_mode_extra[2];
                g_mode_extra[0] = rw, g_mode_extra[1] = rh;
                vlog("resolution.txt: %ux%u\n", rw, rh);
            }
        } else vlog("resolution.txt: want e.g. 640x362 (160x90 to 960x544)\n");
        fclose(rf);
    }
    /* pad bindings (the 3rd slot of each action, code | 3 << 16) for the actions USM.exe leaves without one, so the
       Vita drives them as a gamepad (input.c buttons: 5 L, 6 R = codes 0x19, 0x1a; right
       stick on Z = codes 5/6, Rz = 11/12). Set when missing or unbound (ffff), never over a binding the player chose. */
    static const struct { const char* name; uint8_t code; } P[] = {
        {"ThrowWeb3", 0x1a}, {"BlackButton3", 0x19}, {"CameraUp3", 0x0c}, {"CameraDown3", 0x0b},
        {"CameraLeft3", 0x06}, {"CameraRight3", 0x05}, {"CameraCenter3", 0x1d},  /* 0x1d: rear touch (input.c) */
    };  /* Pause and BackButton stay on Esc/M (input.c), which menus may also read directly */
    for (unsigned i = 0; i < sizeof P / sizeof *P; i++) {
        char p[200];
        snprintf(p, sizeof p, "HKCU\\Software\\Activision\\Ultimate Spider-Man\\Controls\\Player1\\%s", P[i].name);
        RegVal* v = reg_find(p);
        if (v && !(v->size == 4 && v->data[0] == 0xff && v->data[1] == 0xff)) continue;
        if (!v) {
            if (g_nreg >= 256) break;
            v = &g_reg[g_nreg++];
            strcpy(v->path, p);
        }
        v->type = 4, v->size = 4;
        v->data[0] = P[i].code, v->data[1] = 0, v->data[2] = 3, v->data[3] = 0;
    }
}
static void reg_save(void) {
    FILE* f = fopen(REG_FILE, "w");
    if (!f) return;
    for (int i = 0; i < g_nreg; i++) {
        fprintf(f, "%s\t%u\t", g_reg[i].path, g_reg[i].type);
        for (uint32_t k = 0; k < g_reg[i].size; k++) fprintf(f, "%02x", g_reg[i].data[k]);
        fputc('\n', f);
    }
    fclose(f);
}
/* the full path of key h plus sub ("" for none) */
static int reg_path(uint32_t h, uint32_t sub, char* out) {
    const char* base = h == 0x80000000u ? "HKCR" : h == 0x80000001u ? "HKCU" : h == 0x80000002u ? "HKLM"
                     : h - 0x1000u < 64 && g_keys[h - 0x1000u][0] ? g_keys[h - 0x1000u] : NULL;
    if (!base) return 0;
    snprintf(out, 200, "%s%s%s", base, sub && *(char*)H(sub) ? "\\" : "", sub ? (char*)H(sub) : "");
    return 1;
}
static RegVal* reg_find(const char* path) {
    for (int i = 0; i < g_nreg; i++)
        if (!strcasecmp(g_reg[i].path, path)) return &g_reg[i];
    return NULL;
}
/* open (create = 0: only a key some value lives under) or create key h\sub into *out */
static uint32_t reg_open(uint32_t h, uint32_t sub, uint32_t out, int create) {
    char p[200];
    pthread_mutex_lock(&g_reg_m);
    reg_load();
    uint32_t r = 2;  /* ERROR_FILE_NOT_FOUND */
    if (reg_path(h, sub, p)) {
        size_t n = strlen(p);
        int found = create;
        for (int i = 0; i < g_nreg && !found; i++) found = !strncasecmp(g_reg[i].path, p, n) && g_reg[i].path[n] == '\\';
        for (int i = 0; found && i < 64; i++)
            if (!g_keys[i][0]) { strcpy(g_keys[i], p), ST32(out, 0x1000u + i), r = 0; break; }
    }
    pthread_mutex_unlock(&g_reg_m);
    return r;
}
static void RegOpenKeyA_(Ctx* c) { c->eax = reg_open(ARG(0), ARG(1), ARG(2), 0); }     /* (key, sub, out) */
static void RegOpenKeyExA_(Ctx* c) { c->eax = reg_open(ARG(0), ARG(1), ARG(4), 0); }   /* (key, sub, opts, sam, out) */
static void RegCreateKeyA_(Ctx* c) { c->eax = reg_open(ARG(0), ARG(1), ARG(2), 1); }
static void RegCreateKeyExA_(Ctx* c) {  /* (key, sub, res, class, opts, sam, sa, out, disposition) */
    c->eax = reg_open(ARG(0), ARG(1), ARG(7), 1);
    if (!c->eax && ARG(8)) ST32(ARG(8), 2);  /* REG_OPENED_EXISTING_KEY */
}
static void RegCloseKey_(Ctx* c) {
    if (ARG(0) - 0x1000u < 64) g_keys[ARG(0) - 0x1000u][0] = 0;
    c->eax = 0;
}
static void RegQueryValueExA_(Ctx* c) {  /* (key, name, res, type*, data*, size*) */
    char p[200];
    pthread_mutex_lock(&g_reg_m);
    RegVal* v = reg_path(ARG(0), ARG(1), p) ? reg_find(p) : NULL;
    uint32_t r = 2;
    if (v) {
        uint32_t cap = ARG(5) ? LD32(ARG(5)) : 0;
        if (ARG(3)) ST32(ARG(3), v->type);
        r = ARG(4) && cap < v->size ? 234 : 0;  /* ERROR_MORE_DATA */
        if (ARG(4) && !r) memcpy(H(ARG(4)), v->data, v->size);
        /* a string stored without its 0 (older registry.txt) still reads as one: the options menu compares two reads
           of Resolution in different buffers and took the garbage after it for a change ("RESTART" prompt) */
        if (ARG(4) && !r && v->type == 1 && v->size && v->data[v->size - 1] && cap > v->size) ST8(ARG(4) + v->size, 0);
        if (ARG(5)) ST32(ARG(5), v->size);
        if (strstr(p, "Controls\\Player1\\") && p[strlen(p) - 1] == '3' && v->size == 4)  /* the pad bindings read */
            vlog("registry read: %s = %02x%02x%02x%02x\n", strstr(p, "Player1"), v->data[0], v->data[1], v->data[2], v->data[3]);
    }
    pthread_mutex_unlock(&g_reg_m);
    c->eax = r;
}
static void RegSetValueExA_(Ctx* c) {  /* (key, name, res, type, data, size) */
    char p[200];
    pthread_mutex_lock(&g_reg_m);
    reg_load();
    uint32_t r = 2;
    if (reg_path(ARG(0), ARG(1), p)) {
        RegVal* v = reg_find(p);
        if (!v && g_nreg < 256) v = &g_reg[g_nreg++], strcpy(v->path, p);
        if (v) {
            v->type = ARG(3), v->size = ARG(5) < sizeof v->data ? ARG(5) : sizeof v->data;
            memcpy(v->data, H(ARG(4)), v->size);
            reg_save();
            vlog("registry: %s = type %u, %u bytes\n", p, v->type, v->size);
            r = 0;
        }
    }
    pthread_mutex_unlock(&g_reg_m);
    c->eax = r;
}

static void timeGetTime_(Ctx* c) { c->eax = (uint32_t)(sceKernelGetProcessTimeWide() / 1000); }
static void Zero_(Ctx* c) { c->eax = 0; }
static void NotFound_(Ctx* c) { c->eax = 2; }                 /* ERROR_FILE_NOT_FOUND */
static void SHGetFolderPathA_(Ctx* c) { strcpy(S(4), GAME_CWD "\\save"); c->eax = 0; }
static void SysStringLen_(Ctx* c) { c->eax = ARG(0) ? LD32(ARG(0) - 4) / 2 : 0; }  /* BSTR: byte length before the text */
static void VariantInit_(Ctx* c) { memset(H(ARG(0)), 0, 16); c->eax = 0; }
static void VariantClear_(Ctx* c) { ST16(ARG(0), 0); c->eax = 0; }

/* DxDiag, which the game asks for the DirectX version (FUN_0081c2a0; anything but 9.0c and it quits with
   "DirectX 9.0c not found"). One object serves as provider, root and child container: Initialize and
   GetRootContainer/GetChildContainer hand it back, GetProp answers the three version properties. */
static uint32_t g_dxdiag;
static int wide_is(uint32_t w, const char* s) {
    for (; *s; s++, w += 2)
        if (LD16(w) != (uint8_t)*s) return 0;
    return LD16(w) == 0;
}
static void dx_Self(Ctx* c) { ST32(ARG(1), g_dxdiag); c->eax = 0; }  /* GetRootContainer(pp) */
static void dx_Child(Ctx* c) { ST32(ARG(2), g_dxdiag); c->eax = 0; }  /* GetChildContainer(name, pp) */
static void dx_GetProp(Ctx* c) {                                     /* GetProp(name, VARIANT*) */
    uint32_t name = ARG(1), v = ARG(2);
    memset(H(v), 0, 16);
    if (wide_is(name, "dwDirectXVersionMajor")) ST16(v, 19), ST32(v + 8, 9);       /* VT_UI4 */
    else if (wide_is(name, "dwDirectXVersionMinor")) ST16(v, 19), ST32(v + 8, 0);
    else if (wide_is(name, "szDirectXVersionLetter")) {
        uint8_t* b = guest_alloc(16);                                              /* BSTR "c" */
        b[0] = 2, b[4] = 'c';
        ST16(v, 8), ST32(v + 8, G(b + 4));                                         /* VT_BSTR */
    } else { c->eax = 0x80004005u; return; }
    c->eax = 0;
}
static void dx_Ok(Ctx* c) { c->eax = 0; }
static void CoCreateInstance_(Ctx* c) {  /* (clsid, outer, ctx, iid, ppv) */
    if (LD32(ARG(0)) != 0xa65b8071u) { ST32(ARG(4), 0); c->eax = 0x80040154u; return; }  /* only CLSID_DxDiagProvider */
    if (!g_dxdiag) {
        /* IUnknown(3), then container methods up to GetProp at slot 8; provider Initialize/GetRootContainer at 3/4 */
        uint32_t* vt = guest_alloc(9 * 4);
        vt[0] = shim_new("IDxDiag::QueryInterface", dx_Ok, 12);
        vt[1] = shim_new("IDxDiag::AddRef", dx_Ok, 4);
        vt[2] = shim_new("IDxDiag::Release", dx_Ok, 4);
        vt[3] = shim_new("IDxDiag::Initialize/GetNumberOfChildContainers", dx_Ok, 8);
        vt[4] = shim_new("IDxDiag::GetRootContainer/EnumChildContainerNames", dx_Self, 8);
        vt[5] = shim_new("IDxDiag::GetChildContainer", dx_Child, 12);
        vt[6] = shim_new("IDxDiag::GetNumberOfProps", dx_Ok, 8);
        vt[7] = shim_new("IDxDiag::EnumPropNames", dx_Ok, 16);
        vt[8] = shim_new("IDxDiag::GetProp", dx_GetProp, 12);
        g_dxdiag = G(guest_alloc(16));
        ST32(g_dxdiag, G(vt));
    }
    ST32(ARG(4), g_dxdiag);
    c->eax = 0;
}

/* msvcp71 (thiscall: `this` in ecx, arguments popped by the callee). std::string has MSVC 7.1's layout, because the
   game's inline code reads it: allocator (4), buffer/pointer union (16) at 4, size at 20, capacity at 24; the text
   is inline while the capacity is under 16. */
static void str_set(uint32_t s, const char* t) {
    uint32_t n = (uint32_t)strlen(t);
    if (n < 16) memcpy(H(s + 4), t, n + 1), ST32(s + 24, 15);
    else { char* p = malloc(n + 1); memcpy(p, t, n + 1); ST32(s + 4, GA(p)), ST32(s + 24, n); }
    ST32(s + 20, n);
}
static const char* str_text(uint32_t s) { return LD32(s + 24) < 16 ? (const char*)H(s + 4) : (const char*)H(LD32(s + 4)); }
static void string_ctor_cstr(Ctx* c) { str_set(c->ecx, S(0)); c->eax = c->ecx; }
static void string_ctor_copy(Ctx* c) { str_set(c->ecx, str_text(ARG(0))); c->eax = c->ecx; }
static void string_dtor(Ctx* c) { if (LD32(c->ecx + 24) >= 16) free(H(LD32(c->ecx + 4))); ST32(c->ecx + 24, 15), ST32(c->ecx + 20, 0); }
static void ThisRet(Ctx* c) { c->eax = c->ecx; }       /* ofstream: a debug log the Vita doesn't keep */
static void ostream_put(Ctx* c) { c->eax = ARG(0); }   /* operator<<(ostream&, const char*) returns the stream */
static void Nomemory(Ctx* c) { vlog("std::_Nomemory\n"); rc_trap(c, 0, "out of memory in the C++ library"); }


const ShimDef shims_misc[] = {
    {"??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z", string_ctor_cstr},
    {"??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z", string_ctor_copy},
    {"??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ", string_dtor},
    {"??0?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAE@PBDHH@Z", ThisRet},
    {"??0?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAE@XZ", ThisRet},
    {"?open@?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAEXPBDHH@Z", Zero_},
    {"?close@?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAEXXZ", Zero_},
    {"??_D?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAEXXZ", Zero_},
    {"??$?6U?$char_traits@D@std@@@std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@0@AAV10@PBD@Z", ostream_put},
    {"?_Nomemory@std@@YAXXZ", Nomemory},
    {"timeKillEvent", Zero_},
    {"timeGetTime", timeGetTime_}, {"timeBeginPeriod", Zero_}, {"timeEndPeriod", Zero_},
    {"RegOpenKeyA", RegOpenKeyA_}, {"RegOpenKeyExA", RegOpenKeyExA_}, {"RegQueryValueExA", RegQueryValueExA_}, {"RegCloseKey", RegCloseKey_},
    {"RegCreateKeyA", RegCreateKeyA_}, {"RegCreateKeyExA", RegCreateKeyExA_}, {"RegSetValueExA", RegSetValueExA_},
    {"CoInitialize", Zero_}, {"CoInitializeEx", Zero_}, {"CoUninitialize", Zero_}, {"CoCreateInstance", CoCreateInstance_},
    {"VariantInit", VariantInit_}, {"VariantClear", VariantClear_}, {"SysStringLen", SysStringLen_}, {"SysFreeString", Zero_},
    {"SHGetFolderPathA", SHGetFolderPathA_},
    {"DebugSetMute", Zero_},  /* d3d9.dll and d3d9d.dll, through GetProcAddress */
    {NULL, NULL},
};
