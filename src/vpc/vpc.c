/* usm_vpc.exe's platform layer: the newlib, sce* and vitaGL calls the Vita code makes, on Win32 + desktop GL */
#include <windows.h>
#include <fcntl.h>
#include <math.h>
#include <mmsystem.h>
#include <stdio.h>

#include "vitaGL.h"
#include "sce.h"
#include "rt.h"

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup
#undef memalign
#undef mallinfo
#undef glShaderSource

/* ---- heap: one header before every block (raw pointer, size) ---- */
static volatile LONG g_heap_bytes;
void* vpc_memalign(size_t align, size_t n) {
    if (align < 16) align = 16;
    uint8_t* raw = malloc(n + align + 16);
    if (!raw) return NULL;
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + 16 + align - 1) & ~(uintptr_t)(align - 1));
    ((void**)p)[-1] = raw, ((size_t*)p)[-2] = n;
    InterlockedExchangeAdd(&g_heap_bytes, (LONG)n);
    return p;
}
void* vpc_malloc(size_t n) { return vpc_memalign(16, n); }
void* vpc_calloc(size_t n, size_t m) {
    void* p = vpc_memalign(16, n * m);
    if (p) memset(p, 0, n * m);
    return p;
}
size_t vpc_usable(void* p) { return p ? ((size_t*)p)[-2] : 0; }
void vpc_free(void* p) {
    if (!p) return;
    InterlockedExchangeAdd(&g_heap_bytes, -(LONG)vpc_usable(p));
    free(((void**)p)[-1]);
}
void* vpc_realloc(void* p, size_t n) {
    if (!p) return vpc_malloc(n);
    if (!n) { vpc_free(p); return NULL; }
    void* q = vpc_malloc(n);
    if (!q) return NULL;
    size_t old = vpc_usable(p);
    memcpy(q, p, old < n ? old : n);
    vpc_free(p);
    return q;
}
char* vpc_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = vpc_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
struct mallinfo vpc_mallinfo(void) {  /* uordblks: bytes in use, what the Vita's report reads */ return (struct mallinfo){(size_t)g_heap_bytes}; }

/* positioned reads/writes that leave other threads' file positions alone, like newlib's */
ssize_t pread(int fd, void* buf, size_t n, off_t off) {
    OVERLAPPED o = {.Offset = (DWORD)off};
    DWORD got = 0;
    if (!ReadFile((HANDLE)_get_osfhandle(fd), buf, (DWORD)n, &got, &o)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return got;
}
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
    OVERLAPPED o = {.Offset = (DWORD)off};
    DWORD put = 0;
    return WriteFile((HANDLE)_get_osfhandle(fd), buf, (DWORD)n, &put, &o) ? (ssize_t)put : -1;
}

/* a crash goes to log.txt (as the Vita's core dump would): where, and the last shims called. Addresses resolve with
   llvm-nm -n usm_vpc.exe */
void vlog(const char* fmt, ...);
void shim_log_recent(int n);
static LONG WINAPI on_crash(EXCEPTION_POINTERS* e) {
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    CONTEXT* c = e->ContextRecord;
    vlog("crash: exception %08lx at %08lx (access %08lx), thread %lx\n  eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esp=%08lx ebp=%08lx esi=%08lx edi=%08lx\n",
         r->ExceptionCode, (unsigned long)(uintptr_t)r->ExceptionAddress, r->NumberParameters > 1 ? (unsigned long)r->ExceptionInformation[1] : 0,
         GetCurrentThreadId(), c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esp, c->Ebp, c->Esi, c->Edi);
    vlog("  host stack:");
    for (int i = 0; i < 48; i++) {
        uint32_t v = ((uint32_t*)(uintptr_t)c->Esp)[i];
        if (v >= 0x10001000 && v < 0x12000000) vlog(" %08x", v);  /* return addresses into usm_vpc.exe */
    }
    vlog("\n");
    /* the guest side: the Ctx as last synced, and what looks like return addresses into .text on the guest stack */
    extern Ctx* rc_ctx(void);
    Ctx* g = rc_ctx();
    vlog("  guest: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n  guest stack:",
         g->eax, g->ecx, g->edx, g->ebx, g->esp, g->ebp, g->esi, g->edi);
    for (int i = 0; i < 64; i++) {
        uint32_t v = LD32(g->esp + 4 * i);
        if (v >= 0x401000 && v < 0x82c000) vlog(" %06x", v);
    }
    vlog("\n");
    shim_log_recent(8);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* before main: files are binary like on the Vita (no CRLF translation or ^Z ends), Sleep has 1 ms steps */
__attribute__((constructor)) static void vpc_init(void) {
    _set_fmode(_O_BINARY);
    timeBeginPeriod(1);
    SetUnhandledExceptionFilter(on_crash);
}

/* ---- threads ---- */
typedef struct { SceKernelThreadEntry entry; SceSize args; void* argp; HANDLE h; DWORD id; } Thread;
static Thread g_threads[64];
static volatile LONG g_nthreads;
static DWORD WINAPI thread_main(void* p) {
    Thread* t = p;
    return (DWORD)t->entry(t->args, t->argp);
}
SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int prio, SceSize stack, SceUInt attr,
                             int affinity, const void* opt) {
    (void)name, (void)prio, (void)attr, (void)affinity, (void)opt;
    LONG i = InterlockedIncrement(&g_nthreads) - 1;
    if (i >= 64) return -1;
    Thread* t = &g_threads[i];
    t->entry = entry;
    t->h = CreateThread(NULL, stack, thread_main, t, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &t->id);
    return t->h ? (SceUID)t->id : -1;
}
int sceKernelStartThread(SceUID id, SceSize args, void* argp) {
    for (LONG i = 0; i < g_nthreads; i++) {
        Thread* t = &g_threads[i];
        if ((SceUID)t->id != id) continue;
        if (args) t->argp = memcpy(vpc_malloc(args), argp, args);  /* the Vita copies the arguments too */
        t->args = args;
        ResumeThread(t->h);
        return 0;
    }
    return -1;
}
int sceKernelGetThreadId(void) { return (int)GetCurrentThreadId(); }
int sceKernelChangeThreadCpuAffinityMask(SceUID t, int mask) { (void)t, (void)mask; return 0; }
int sceKernelDelayThread(SceUInt us) { Sleep(us / 1000); return 0; }
SceUInt64 sceKernelGetProcessTimeWide(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (SceUInt64)(c.QuadPart / (double)f.QuadPart * 1e6);
}
int sceKernelExitProcess(int code) { ExitProcess((UINT)code); }
SceUID sceKernelFindMemBlockByAddr(const void* addr, SceSize size) {
    (void)size;
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery(addr, &m, sizeof m) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ? 1 : -1;
}
SceUID sceKernelCreateSema(const char* name, SceUInt attr, int init, int max, void* opt) {
    (void)name, (void)attr, (void)opt;
    return (SceUID)(intptr_t)CreateSemaphoreA(NULL, init, max, NULL);
}
int sceKernelWaitSema(SceUID s, int n, SceUInt* timeout) {
    for (int i = 0; i < n; i++)
        if (WaitForSingleObject((HANDLE)(intptr_t)s, timeout ? *timeout / 1000 : INFINITE)) return -1;
    return 0;
}
int sceKernelSignalSema(SceUID s, int n) { return ReleaseSemaphore((HANDLE)(intptr_t)s, n, NULL) ? 0 : -1; }

/* ---- audio: silent, but each output blocks for one grain so the mixer keeps the Vita's pace ---- */
/* shortcut: no sound on the PC, add waveOut if a sound bug has to be checked here */
static int g_grain = 512, g_rate = 48000;
int sceAudioOutOpenPort(int type, int grain, int rate, int mode) {
    (void)type, (void)mode;
    g_grain = grain, g_rate = rate;
    return 1;
}
int sceAudioOutOutput(int port, const void* buf) {
    (void)port, (void)buf;
    static SceUInt64 next;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (next < now) next = now;
    next += (SceUInt64)g_grain * 1000000 / g_rate;
    if (next > now + 1000) Sleep((DWORD)((next - now) / 1000));
    return 0;
}

/* ---- window and GL ---- */
static HWND g_hwnd;
static HDC g_hdc;
#define X(n, N) PFNGL##N##PROC p_##n;
VPC_GL_PROCS(X)
#undef X

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) ExitProcess(0);
    return DefWindowProcA(h, m, w, l);
}

int vglInitExtended(int pool, int w, int h, int ram, int msaa) {
    (void)pool, (void)ram, (void)msaa;
    SetProcessDPIAware();  /* 960x544 real pixels, or Windows scaling moves the 2D rects */
    WNDCLASSA wc = {.style = CS_OWNDC, .lpfnWndProc = wndproc, .hInstance = GetModuleHandleA(NULL), .lpszClassName = "usm_vpc",
                    .hCursor = LoadCursor(NULL, IDC_ARROW)};
    RegisterClassA(&wc);
    RECT r = {0, 0, w, h};
    DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowA("usm_vpc", "USM (Vita code on PC)", style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    g_hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24, pfd.cStencilBits = 8;
    SetPixelFormat(g_hdc, ChoosePixelFormat(g_hdc, &pfd), &pfd);
    wglMakeCurrent(g_hdc, wglCreateContext(g_hdc));  /* a legacy context: the compatibility profile (client arrays) */
#define X(n, N) if (!(p_##n = (PFNGL##N##PROC)wglGetProcAddress(#n))) { fprintf(stderr, "no %s\n", #n); ExitProcess(2); }
    VPC_GL_PROCS(X)
#undef X
    BOOL(WINAPI * swap_interval)(int) = (void*)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval) swap_interval(0);  /* the game paces itself */
    return 0;
}

void vglSwapBuffers(GLboolean dialog) {
    (void)dialog;
    SwapBuffers(g_hdc);
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) TranslateMessage(&m), DispatchMessageA(&m);
}

size_t vglMemFree(int type) { (void)type; return 64u << 20; }

void vpc_shader_source(GLuint s, GLsizei n, const GLchar* const* src, const GLint* len) {
    (void)n, (void)len;
    const char* p = src[0];
    const char* pre = strstr(p, "precision highp float;");
    char* b = malloc(strlen(p) + 32);
    strcpy(b, "#version 120\n");
    if (pre) strncat(b, p, pre - p), strcat(b, pre + strlen("precision highp float;"));
    else strcat(b, p);
    const GLchar* one = b;
    p_glShaderSource(s, 1, &one, NULL);
    free(b);
}

/* ---- pad: the keyboard while the window has focus, plus a script (USM_DIR "vpc_input.txt") ----
   Keys: arrows = d-pad, WASD = left stick, IJKL = right stick, Space = Cross, Backspace = Circle, F = Square,
   G = Triangle, Q = L, E = R, Enter = Start, Tab = Select.
   Script lines: "<seconds> [BUTTON ...]" holds those buttons from that time (after start) until the next line;
   names CROSS CIRCLE SQUARE TRIANGLE L R START SELECT UP DOWN LEFT RIGHT, and LX=n LY=n RX=n RY=n (0-255). */
typedef struct { double t; unsigned buttons; int lx, ly, rx, ry; } Step;
static Step g_script[1024];
static int g_nscript = -1;
static void load_script(void) {
    g_nscript = 0;
    FILE* f = fopen(USM_DIR "vpc_input.txt", "r");
    if (!f) return;
    static const struct { const char* n; unsigned b; } names[] = {
        {"CROSS", SCE_CTRL_CROSS}, {"CIRCLE", SCE_CTRL_CIRCLE}, {"SQUARE", SCE_CTRL_SQUARE}, {"TRIANGLE", SCE_CTRL_TRIANGLE},
        {"L", SCE_CTRL_LTRIGGER}, {"R", SCE_CTRL_RTRIGGER}, {"START", SCE_CTRL_START}, {"SELECT", SCE_CTRL_SELECT},
        {"UP", SCE_CTRL_UP}, {"DOWN", SCE_CTRL_DOWN}, {"LEFT", SCE_CTRL_LEFT}, {"RIGHT", SCE_CTRL_RIGHT},
        {"TOUCHB", 1u << 30}};  /* the back touchpad (input.c REAR_TOUCH) */
    char line[256];
    while (g_nscript < 1024 && fgets(line, sizeof line, f)) {
        Step s = {.lx = 128, .ly = 128, .rx = 128, .ry = 128};
        char* tok = strtok(line, " \t\r\n");
        if (!tok || *tok == '#') continue;
        s.t = atof(tok);
        while ((tok = strtok(NULL, " \t\r\n"))) {
            int v;
            if (sscanf(tok, "LX=%d", &v) == 1) s.lx = v;
            else if (sscanf(tok, "LY=%d", &v) == 1) s.ly = v;
            else if (sscanf(tok, "RX=%d", &v) == 1) s.rx = v;
            else if (sscanf(tok, "RY=%d", &v) == 1) s.ry = v;
            else
                for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                    if (!strcmp(tok, names[i].n)) s.buttons |= names[i].b;
        }
        g_script[g_nscript++] = s;
    }
    fclose(f);
    vlog("vpc: %d scripted pad steps\n", g_nscript);
}

int sceCtrlSetSamplingMode(int mode) { (void)mode; return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData* d, int n) {
    (void)port, (void)n;
    static SceUInt64 t0;
    if (g_nscript < 0) load_script(), t0 = sceKernelGetProcessTimeWide();
    d->buttons = 0, d->lx = d->ly = d->rx = d->ry = 128;
    double t = (sceKernelGetProcessTimeWide() - t0) / 1e6;
    for (int i = g_nscript - 1; i >= 0; i--)
        if (g_script[i].t <= t) {
            d->buttons = g_script[i].buttons, d->lx = g_script[i].lx, d->ly = g_script[i].ly, d->rx = g_script[i].rx, d->ry = g_script[i].ry;
            break;
        }
    if (!g_hwnd || GetForegroundWindow() != g_hwnd) return 1;
#define K(vk) (GetAsyncKeyState(vk) & 0x8000)
    static const struct { int vk; unsigned b; } keys[] = {
        {VK_UP, SCE_CTRL_UP}, {VK_DOWN, SCE_CTRL_DOWN}, {VK_LEFT, SCE_CTRL_LEFT}, {VK_RIGHT, SCE_CTRL_RIGHT},
        {VK_SPACE, SCE_CTRL_CROSS}, {VK_BACK, SCE_CTRL_CIRCLE}, {'F', SCE_CTRL_SQUARE}, {'G', SCE_CTRL_TRIANGLE},
        {'Q', SCE_CTRL_LTRIGGER}, {'E', SCE_CTRL_RTRIGGER}, {VK_RETURN, SCE_CTRL_START}, {VK_TAB, SCE_CTRL_SELECT},
        {'T', 1u << 30}};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (K(keys[i].vk)) d->buttons |= keys[i].b;
    if (K('A')) d->lx = 0;
    if (K('D')) d->lx = 255;
    if (K('W')) d->ly = 0;
    if (K('S')) d->ly = 255;
    if (K('J')) d->rx = 0;
    if (K('L')) d->rx = 255;
    if (K('I')) d->ry = 0;
    if (K('K')) d->ry = 255;
    return 1;
}

/* the lowest free 64 KB-aligned region: memory below it would get negative guest addresses (logged by GA). The 1 MB
   under rc_guest is reserved, since its guest addresses would be the shim range. */
void* vpc_image_alloc(size_t n) {
    MEMORY_BASIC_INFORMATION m;
    for (uintptr_t a = 0x10000; VirtualQuery((void*)a, &m, sizeof m); a = (uintptr_t)m.BaseAddress + m.RegionSize) {
        uintptr_t lo = ((uintptr_t)m.BaseAddress + 0xffff) & ~(uintptr_t)0xffff;
        if (m.State != MEM_FREE || lo + n > (uintptr_t)m.BaseAddress + m.RegionSize) continue;
        void* p = VirtualAlloc((void*)lo, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) continue;
        if (lo >= 0x500000) VirtualAlloc((void*)(lo - 0x500000), 0x100000, MEM_RESERVE, PAGE_NOACCESS);
        return p;
    }
    return vpc_memalign(0x10000, n);
}
