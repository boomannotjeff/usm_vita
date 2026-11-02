/* DirectInput 8 on SceCtrl. The Vita is a gamepad to the game (analog left stick, buttons), plus a keyboard for the
   actions the game has no gamepad default for.
   USM.exe's default bindings (table at 0x88da80: name, keyboard 1, keyboard 2, gamepad) give the pad only Forward/
   Backward/TurnLeft/TurnRight (codes 4,3,2,1 = LY-, LY+, LX-, LX+) and Jump, StickToWalls, Punch, Kick (codes 0x15..0x18
   = buttons 1..4). Gamepad codes: 1..16 axis directions (two per DIJOYSTATE axis), 0x11..0x14 PoV, 0x15+ buttons.
   Web (R), zip (L; wall crawl is Circle) and camera (right stick) also get pad bindings from the registry defaults (shims_misc.c
   reg_defaults), so prompts name Vita buttons. The keyboard only carries what menus read directly: the D-pad as
   arrows, Esc (Start) and M (Select, the map). */
#include <psp2/ctrl.h>
#ifndef USM_VPC
#include <psp2/touch.h>
#endif

#include "com.h"

/* not a SceCtrl bit: a finger on the back touchpad = button 9 (code 0x1d), CameraCenter (R3 on PC; shims_misc.c).
   vpc sets it from TOUCHB in vpc_input.txt or the T key */
#define REAR_TOUCH (1u << 30)

/* DIK scan codes of the keyboard defaults used */
enum { K_ESC = 1, K_M = 50, K_UP = 200, K_LEFT = 203, K_RIGHT = 205, K_DOWN = 208 };
enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_PAD = 3 };
#define PAD_GUID 0x56495441u  /* "VITA": Data1 of the pad's instance GUID */

static SceCtrlData g_pad;
static uint8_t g_keys[256], g_prev[256], g_mouse[2], g_mprev[2];  /* mouse: left, right button */
static uint32_t g_seq;
static int32_t g_min[8] = {0, 0, 0, 0, 0, 0, 0, 0}, g_max[8] = {65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535};

static void poll(void) {
    memset(&g_pad, 0, sizeof g_pad);
    g_pad.lx = g_pad.ly = g_pad.rx = g_pad.ry = 128;
    sceCtrlPeekBufferPositive(0, &g_pad, 1);
#ifndef USM_VPC
    SceTouchData t;
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &t, 1) > 0 && t.reportNum) g_pad.buttons |= REAR_TOUCH;
#endif
    uint32_t b = g_pad.buttons;
    memset(g_keys, 0, sizeof g_keys);
#define KEY(k, on) do { if (on) g_keys[k] = 0x80; } while (0)
    KEY(K_UP, b & SCE_CTRL_UP);  /* menus */
    KEY(K_DOWN, b & SCE_CTRL_DOWN);
    KEY(K_LEFT, b & SCE_CTRL_LEFT);
    KEY(K_RIGHT, b & SCE_CTRL_RIGHT);
    KEY(K_ESC, b & SCE_CTRL_START);    /* Pause */
    KEY(K_M, b & SCE_CTRL_SELECT);     /* BackButton */
#undef KEY
    /* the mouse buttons the QTEs ask for ("alternate left and right click"): L and R only. The game also binds the
       left click to the zip (BlackButton) and the right one to ThrowWeb, the bumpers' own actions; on Square/Triangle
       they fired those next to punch/kick */
    g_mouse[0] = b & SCE_CTRL_LTRIGGER ? 0x80 : 0, g_mouse[1] = b & SCE_CTRL_RTRIGGER ? 0x80 : 0;
}

static int dev_kind(Ctx* c) { return (int)LD32(ARG(0) + 8); }

/* stick byte (0..255, 128 centre) to the axis range the game set; a small dead zone keeps it still at rest */
static int32_t axis(int v, int i) {
    int d = v - 128;
    if (d > -12 && d < 12) d = 0;
    double t = (d + 128) / 255.0;
    return g_min[i] + (int32_t)((g_max[i] - g_min[i]) * t + 0.5);
}

static void pad_state(uint32_t out, uint32_t cb) {  /* DIJOYSTATE(2): lX lY lZ lRx lRy lRz sliders[2] pov[4] buttons */
    poll();
    int32_t a[6] = {axis(g_pad.lx, 0), axis(g_pad.ly, 1), axis(g_pad.rx, 2), axis(128, 3), axis(128, 4), axis(g_pad.ry, 5)};
    for (int i = 0; i < 6 && 4u * i + 4 <= cb; i++) ST32(out + 4 * i, (uint32_t)a[i]);
    uint32_t b = g_pad.buttons;
    int up = b & SCE_CTRL_UP, dn = b & SCE_CTRL_DOWN, lf = b & SCE_CTRL_LEFT, rt = b & SCE_CTRL_RIGHT;
    uint32_t pov = up ? (rt ? 4500 : lf ? 31500 : 0) : dn ? (rt ? 13500 : lf ? 22500 : 18000) : rt ? 9000 : lf ? 27000 : 0xffffffffu;
    if (cb >= 48) ST32(out + 32, pov), ST32(out + 36, 0xffffffffu), ST32(out + 40, 0xffffffffu), ST32(out + 44, 0xffffffffu);
    /* buttons 1..4 are the game's pad defaults: Jump, StickToWalls (Circle), Punch, Kick; 5 = zip (L), 6 = web (R);
       9 = camera centre (rear touch) */
    static const uint32_t map[] = {SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_LTRIGGER,
                                   SCE_CTRL_RTRIGGER, SCE_CTRL_SELECT, SCE_CTRL_START, REAR_TOUCH};
    for (unsigned i = 0; i < sizeof map / sizeof *map && 48 + i < cb; i++) ST8(out + 48 + i, b & map[i] ? 0x80 : 0);
}

static void dev_GetDeviceState(Ctx* c) {  /* (cb, data) */
    uint32_t cb = ARG(1), out = ARG(2);
    memset(H(out), 0, cb);
    if (dev_kind(c) == DEV_PAD) pad_state(out, cb);
    else if (dev_kind(c) == DEV_KEYBOARD) poll(), memcpy(H(out), g_keys, cb < 256 ? cb : 256);
    else if (dev_kind(c) == DEV_MOUSE && cb >= 14) poll(), ST8(out + 12, g_mouse[0]), ST8(out + 13, g_mouse[1]);  /* DIMOUSESTATE: lX lY lZ buttons */
    c->eax = 0;
}
/* buffered keyboard: one DIDEVICEOBJECTDATA {ofs, data, time, seq[, appdata]} per key that changed since last call */
static void dev_GetDeviceData(Ctx* c) {  /* (cbObjectData, rgdod, pdwInOut, flags) */
    uint32_t sz = ARG(1), out = ARG(2), pn = ARG(3), max = LD32(pn), n = 0;
    if (dev_kind(c) == DEV_KEYBOARD) {
        poll();
        for (int k = 0; k < 256; k++) {
            if (g_keys[k] == g_prev[k]) continue;
            if (out && n < max) {
                uint32_t o = out + n * sz;
                memset(H(o), 0, sz);
                ST32(o, (uint32_t)k), ST32(o + 4, g_keys[k]), ST32(o + 12, ++g_seq);
            }
            if (n < max || !out) g_prev[k] = g_keys[k];
            n++;
        }
    } else if (dev_kind(c) == DEV_MOUSE) {  /* button changes, at DIMOFS_BUTTON0/1 = 12/13 */
        poll();
        for (int k = 0; k < 2; k++) {
            if (g_mouse[k] == g_mprev[k]) continue;
            if (out && n < max) {
                uint32_t o = out + n * sz;
                memset(H(o), 0, sz);
                ST32(o, 12u + k), ST32(o + 4, g_mouse[k]), ST32(o + 12, ++g_seq);
            }
            if (n < max || !out) g_mprev[k] = g_mouse[k];
            n++;
        }
    }
    ST32(pn, out ? (n < max ? n : max) : n);
    c->eax = 0;
}
/* DIPROP_RANGE (4): DIPROPRANGE {size, headersize, obj, how, min, max}; how 0 = whole device, 1 = by offset, 2 = by id */
static void dev_SetProperty(Ctx* c) {
    uint32_t prop = ARG(1), p = ARG(2);
    if (prop == 4) {
        uint32_t obj = LD32(p + 8), how = LD32(p + 12);
        int lo = 0, hi = 7;
        if (how == 1) lo = hi = (int)(obj / 4);
        else if (how == 2) lo = hi = (int)((obj >> 8) & 0xff);
        for (int i = lo; i <= hi && i < 8; i++) g_min[i] = (int32_t)LD32(p + 16), g_max[i] = (int32_t)LD32(p + 20);
    }
    c->eax = 0;
}
/* the pad's axes and buttons, for EnumObjects: DIDEVICEOBJECTINSTANCEA {size, guidType, ofs, type, flags, name[260] ...} */
static void dev_EnumObjects(Ctx* c) {  /* (callback, ref, flags) */
    if (dev_kind(c) != DEV_PAD) { c->eax = 0; return; }
    static const uint32_t guid1[] = {0xa36d02e0u, 0xa36d02e1u, 0xa36d02e2u, 0, 0, 0xa36d02e3u};  /* X Y Z . . Rz */
    static const char* names[] = {"X Axis", "Y Axis", "Z Axis", "", "", "Z Rotation"};
    uint32_t o = G(guest_alloc(316)), cb = ARG(1), ref = ARG(2), flags = ARG(3);
    static const uint8_t tail[8] = {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    for (int i = 0; i < 6; i++) {
        if (!guid1[i] || !(flags == 0 || flags & 3)) continue;  /* DIDFT_ALL or DIDFT_AXIS */
        memset(H(o), 0, 316);
        ST32(o, 316), ST32(o + 4, guid1[i]), ST16(o + 8, 0xc9f3), ST16(o + 10, 0x11cf);
        memcpy(H(o + 12), tail, 8);
        ST32(o + 20, 4u * i), ST32(o + 24, 2u | (uint32_t)i << 8);  /* DIDFT_ABSAXIS, instance i */
        strcpy((char*)H(o + 32), names[i]);
        uint32_t args[2] = {o, ref};
        if (!guest_call(cb, 2, args)) break;  /* DIENUM_STOP */
    }
    c->eax = 0;
}
static void dev_GetCapabilities(Ctx* c) {  /* DIDEVCAPS: size, flags, devtype, axes, buttons, povs */
    uint32_t p = ARG(1), k = (uint32_t)dev_kind(c);
    ST32(p + 4, 1);  /* DIDC_ATTACHED */
    ST32(p + 8, k == DEV_PAD ? 0x0215 : k == DEV_KEYBOARD ? 0x13 : 0x12);
    ST32(p + 12, k == DEV_PAD ? 4 : k == DEV_MOUSE ? 2 : 0);
    ST32(p + 16, k == DEV_PAD ? 9 : k == DEV_MOUSE ? 3 : 128);
    ST32(p + 20, k == DEV_PAD ? 1 : 0);
    c->eax = 0;
}
static uint32_t pad_instance(void);
static void dev_GetDeviceInfo(Ctx* c) {  /* DIDEVICEINSTANCEA: the pad's, or a keyboard/mouse with the system GUIDs */
    uint32_t out = ARG(1), size = LD32(out) ? LD32(out) : 0x244;
    if (dev_kind(c) == DEV_PAD) { memcpy(H(out), H(pad_instance()), size < 0x244 ? size : 0x244); ST32(out, size); }
    else {
        memset(H(out) + 4, 0, (size < 0x244 ? size : 0x244) - 4);
        ST32(out + 4, dev_kind(c) == DEV_KEYBOARD ? 0x6f1d2b61u : 0x6f1d2b60u);
        ST32(out + 36, dev_kind(c) == DEV_KEYBOARD ? 0x13 : 0x12);
        strcpy((char*)H(out + 40), dev_kind(c) == DEV_KEYBOARD ? "Keyboard" : "Mouse");
    }
    c->eax = 0;
}
static void dev_GetProperty(Ctx* c) {  /* DIPROP_RANGE (4) answers what SetProperty set; everything else reads 0 */
    uint32_t prop = ARG(1), p = ARG(2);
    if (prop == 4) {
        uint32_t obj = LD32(p + 8), how = LD32(p + 12), i = how == 1 ? obj / 4 : how == 2 ? (obj >> 8) & 0xff : 0;
        ST32(p + 16, (uint32_t)g_min[i & 7]), ST32(p + 20, (uint32_t)g_max[i & 7]);
    } else if (LD32(p) >= 20) {
        ST32(p + 16, 0);
    }
    c->eax = 0;
}
static void dev_CreateEffect(Ctx* c) { ST32(ARG(3), 0); c->eax = 0x80004001u; }  /* E_NOTIMPL: no force feedback */
static void dev_Ok(Ctx* c) { c->eax = 0; }  /* also the Enum* with nothing to enumerate: no callbacks, DI_OK */
static void dev_NotFound(Ctx* c) { c->eax = 0x80070002u; }  /* DIERR_OBJECTNOTFOUND */
static void dev_NoImpl(Ctx* c) { c->eax = 0x80004001u; }    /* E_NOTIMPL: force feedback, action-map images */
static const ShimDef dev_impl[] = {{"GetDeviceState", dev_GetDeviceState}, {"GetDeviceData", dev_GetDeviceData},
                                   {"SetProperty", dev_SetProperty}, {"EnumObjects", dev_EnumObjects},
                                   {"GetCapabilities", dev_GetCapabilities}, {"GetDeviceInfo", dev_GetDeviceInfo},
                                   {"GetProperty", dev_GetProperty}, {"CreateEffect", dev_CreateEffect},
                                   {"SetDataFormat", dev_Ok}, {"SetCooperativeLevel", dev_Ok}, {"Acquire", dev_Ok},
                                   {"Unacquire", dev_Ok}, {"Poll", dev_Ok}, {"SetEventNotification", dev_Ok},
                                   {"GetObjectInfo", dev_NotFound}, {"EnumEffects", dev_Ok},
                                   {"GetEffectInfo", dev_NotFound}, {"GetForceFeedbackState", dev_NoImpl},
                                   {"EnumCreatedEffectObjects", dev_Ok}, {"EnumEffectsInFile", dev_Ok},
                                   {"GetImageInfo", dev_NoImpl}, {NULL, NULL}};

static uint32_t vt_di, vt_dev;

/* DIDEVICEINSTANCEA {size, guidInstance, guidProduct, devType, instanceName[260], productName[260], guidFF, page, usage} */
static uint32_t pad_instance(void) {
    static uint32_t inst;
    if (inst) return inst;
    inst = G(guest_alloc(0x244));
    ST32(inst, 0x244);
    ST32(inst + 4, PAD_GUID), ST32(inst + 20, PAD_GUID + 1);
    ST32(inst + 36, 0x0215);  /* DI8DEVTYPE_GAMEPAD, standard */
    strcpy((char*)H(inst + 40), "PS Vita Controller");
    strcpy((char*)H(inst + 300), "PS Vita Controller");
    return inst;
}
static void di_EnumDevices(Ctx* c) {  /* (devType, callback, ref, flags): one pad for game controllers or everything */
    uint32_t type = ARG(1);
    if (type == 0 || type == 4) {
        uint32_t args[2] = {pad_instance(), ARG(3)};
        guest_call(ARG(2), 2, args);
    }
    c->eax = 0;
}
static void di_CreateDevice(Ctx* c) {  /* (rguid, ppDevice, outer): GUID_SysKeyboard 6f1d2b61, GUID_SysMouse 6f1d2b60 */
    uint32_t g = LD32(ARG(1));
    int kind = g == 0x6f1d2b61u ? DEV_KEYBOARD : g == 0x6f1d2b60u ? DEV_MOUSE : g == PAD_GUID ? DEV_PAD : 0;
    if (!kind) { ST32(ARG(2), 0); c->eax = 0x80040154u; return; }  /* REGDB_E_CLASSNOTREG: no such device */
    uint32_t d = com_new(vt_dev, 16);
    ST32(d + 8, (uint32_t)kind);
    ST32(ARG(2), d);
    vlog("DirectInput device %d created\n", kind);
    c->eax = 0;
}
static void di_GetDeviceStatus(Ctx* c) { c->eax = 0; }
static void di_FindDevice(Ctx* c) { c->eax = 0x80040154u; }  /* REGDB_E_CLASSNOTREG */
static const ShimDef di_impl[] = {{"CreateDevice", di_CreateDevice}, {"EnumDevices", di_EnumDevices},
                                  {"GetDeviceStatus", di_GetDeviceStatus}, {"FindDevice", di_FindDevice},
                                  {"EnumDevicesBySemantics", di_GetDeviceStatus}, {NULL, NULL}};

static void DirectInput8Create_(Ctx* c) {  /* (hinst, version, riid, ppvOut, outer) */
    if (!vt_di) {
        vt_di = com_vtable("IDirectInput8A", di_impl);
        vt_dev = com_vtable("IDirectInputDevice8A", dev_impl);
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
#ifndef USM_VPC
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
#endif
    }
    ST32(ARG(3), com_new(vt_di, 16));
    vlog("DirectInput8Create\n");
    c->eax = 0;
}

const ShimDef shims_dinput8[] = {{"DirectInput8Create", DirectInput8Create_}, {NULL, NULL}};
