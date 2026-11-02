/* USER32: there is no window on the Vita. The game gets a fake one the size of the screen and an empty message queue. */
#include <psp2/kernel/processmgr.h>

#include "vita.h"

#define S(i) ((char*)H(ARG(i)))
#define HWND_MAIN 0x2000u
#define SCREEN_W 640
#define SCREEN_H 480

static uint32_t g_wndproc;
static void RegisterClassExA_(Ctx* c) { g_wndproc = LD32(ARG(0) + 8); c->eax = 0xc000; }  /* WNDCLASSEX.lpfnWndProc */
static void RegisterClassA_(Ctx* c) { g_wndproc = LD32(ARG(0) + 4); c->eax = 0xc000; }
static void CreateWindowExA_(Ctx* c) { vlog("CreateWindowEx \"%s\"\n", ARG(2) ? S(2) : ""); c->eax = HWND_MAIN; }
static void DefWindowProcA_(Ctx* c) { c->eax = 0; }
static void PeekMessageA_(Ctx* c) { c->eax = 0; }  /* no messages, ever */
static void GetMessageA_(Ctx* c) { sceKernelDelayThread(1000); c->eax = 1; memset(H(ARG(0)), 0, 28); }
static void GetSystemMetrics_(Ctx* c) { c->eax = ARG(0) == 0 ? SCREEN_W : ARG(0) == 1 ? SCREEN_H : 0; }
static void GetClientRect_(Ctx* c) { uint32_t r[4] = {0, 0, SCREEN_W, SCREEN_H}; memcpy(H(ARG(1)), r, 16); c->eax = 1; }
static void GetWindowRect_(Ctx* c) { GetClientRect_(c); }
static void ClientToScreen_(Ctx* c) { c->eax = 1; }
static void True_(Ctx* c) { c->eax = 1; }
static void Zero_(Ctx* c) { c->eax = 0; }
static void GetKeyboardState_(Ctx* c) { memset(H(ARG(0)), 0, 256); c->eax = 1; }
static void MessageBoxA_(Ctx* c) { vlog("MessageBox \"%s\": %s\n", ARG(2) ? S(2) : "", ARG(1) ? S(1) : ""); c->eax = 1; }  /* IDOK */
static void GetForegroundWindow_(Ctx* c) { c->eax = HWND_MAIN; }

/* Showing the window activates the app, as on Windows: the game's main loop waits until its window procedure has
   seen WM_ACTIVATEAPP(TRUE) (flag 0x965bf9, which starts at 0). */
static void ShowWindow_(Ctx* c) {
    static int sent;
    if (!sent && g_wndproc) {
        sent = 1;
        static const uint32_t msgs[][2] = {{0x1c, 1}, {0x06, 1}, {0x07, 0}};  /* ACTIVATEAPP, ACTIVATE(WA_ACTIVE), SETFOCUS */
        for (unsigned i = 0; i < 3; i++) {
            uint32_t a[4] = {HWND_MAIN, msgs[i][0], msgs[i][1], 0};
            guest_call(g_wndproc, 4, a);
        }
        vlog("window shown: sent WM_ACTIVATEAPP to the game\n");
    }
    c->eax = 1;
}
static void GetCursorPos_(Ctx* c) { ST32(ARG(0), SCREEN_W / 2), ST32(ARG(0) + 4, SCREEN_H / 2); c->eax = 1; }
static void PtInRect_(Ctx* c) {  /* (rect*, POINT by value: x, y) */
    int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2), l = (int32_t)LD32(ARG(0)), t = (int32_t)LD32(ARG(0) + 4);
    c->eax = x >= l && x < (int32_t)LD32(ARG(0) + 8) && y >= t && y < (int32_t)LD32(ARG(0) + 12);
}
static void Hwnd_(Ctx* c) { c->eax = HWND_MAIN; }

const ShimDef shims_user32[] = {
    {"GetCursorPos", GetCursorPos_}, {"PtInRect", PtInRect_}, {"IsWindow", True_}, {"GetAsyncKeyState", Zero_},
    {"ClipCursor", True_}, {"SetCursor", Zero_}, {"GetFocus", Hwnd_}, {"GetDesktopWindow", Hwnd_}, {"GetDC", Hwnd_},
    {"UnregisterClassA", True_}, {"MoveWindow", True_}, {"SetWindowTextA", True_}, {"SetForegroundWindow", True_},
    {"ValidateRect", True_}, {"SendMessageA", Zero_}, {"IsDialogMessage", Zero_}, {"IsDialogMessageA", Zero_},
    {"RegisterClassExA", RegisterClassExA_}, {"RegisterClassA", RegisterClassA_}, {"CreateWindowExA", CreateWindowExA_},
    {"DefWindowProcA", DefWindowProcA_}, {"PeekMessageA", PeekMessageA_}, {"GetMessageA", GetMessageA_},
    {"GetSystemMetrics", GetSystemMetrics_}, {"GetClientRect", GetClientRect_}, {"GetWindowRect", GetWindowRect_},
    {"ClientToScreen", ClientToScreen_}, {"ShowWindow", ShowWindow_}, {"UpdateWindow", True_}, {"SetWindowPos", True_},
    {"SetCursorPos", True_}, {"ShowCursor", Zero_}, {"SystemParametersInfoA", True_}, {"LoadIconA", True_},
    {"LoadCursorA", True_}, {"TranslateMessage", True_}, {"DispatchMessageA", Zero_}, {"SetFocus", True_},
    {"GetKeyboardState", GetKeyboardState_}, {"MapVirtualKeyA", Zero_}, {"MessageBoxA", MessageBoxA_},
    {"GetForegroundWindow", GetForegroundWindow_}, {"GetActiveWindow", GetForegroundWindow_},
    {"DestroyWindow", True_}, {"PostQuitMessage", Zero_}, {"SetWindowLongA", Zero_}, {"GetWindowLongA", Zero_},
    {"AdjustWindowRect", True_}, {"AdjustWindowRectEx", True_}, {"ScreenToClient", True_},
    {NULL, NULL},
};
