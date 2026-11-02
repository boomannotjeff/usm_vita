/* usm.vpk: Ultimate Spider-Man on the Vita, running the translated game code. Loads ux0:data/usm/USM.exe into guest
   memory as data (sections only, none of it runs), binds its imports to shims and starts the translated entry. */
#define RC_GUEST_WRITER  /* sets rc_guest, see rt.h */
#include <malloc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#if defined(USM_VITAGL) && !defined(USM_VPC)
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>
#endif

#include "vita.h"

int _newlib_heap_size_user = 192 * 1024 * 1024;  /* guest heap, image and stacks all come from here */
unsigned int sceUserMainThreadStackSize = 16 * 1024 * 1024;  /* translated code nests deep on the host stack: 4 MB ran out while
                                                                streaming (crashes at f_4c1fa0, f_587da0) */

#define IMAGE_SIZE 0x5ea060u /* its SizeOfImage */

/* Allocated before anything else, so every later allocation (guest heap, stacks, TEBs, COM objects) lies above it
   and has a positive guest address. Memory freed below it would come back as "negative" guest addresses. */
static uint8_t* g_image;

/* a missing install step: says so on screen (a system message dialog) instead of crashing, then quits */
static void fatal(const char* msg) {
    vlog("%s\n", msg);
#if defined(USM_VITAGL) && !defined(USM_VPC)
    vglInitExtended(0, 960, 544, 2 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    SceMsgDialogUserMessageParam um = {0};
    um.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK, um.msg = (const SceChar8*)msg;
    SceMsgDialogParam p;
    sceMsgDialogParamInit(&p);
    p.mode = SCE_MSG_DIALOG_MODE_USER_MSG, p.userMsgParam = &um;
    if (sceMsgDialogInit(&p) >= 0)
        while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) glClear(GL_COLOR_BUFFER_BIT), vglSwapBuffers(GL_TRUE);
#endif
    sceKernelExitProcess(1);
}

static int exists(const char* path) {
    struct stat s;
    return stat(path, &s) == 0;
}

static int pops_of(const char* dll, const char* name) {
    for (unsigned i = 0; i < g_nimports; i++)
        if (!strcasecmp(g_imports[i].dll, dll) && !strcmp(g_imports[i].name, name)) return g_imports[i].pops;
    return 0;
}

static uint32_t load_exe(void) {
    vlog("loading " USM_DIR "USM.exe\n");
    FILE* f = fopen(USM_DIR "USM.exe", "rb");
    if (!f) { vlog("cannot open " USM_DIR "USM.exe: copy it there from your PC install\n"); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* file = malloc(n);
    fread(file, 1, n, f);
    fclose(f);
    uint32_t pe = *(uint32_t*)(file + 0x3c);
    uint32_t base = *(uint32_t*)(file + pe + 52), size = *(uint32_t*)(file + pe + 80);
    uint16_t nsec = *(uint16_t*)(file + pe + 6), opt = *(uint16_t*)(file + pe + 20);
    vlog("USM.exe read; image %u bytes\n", size);
    if (size > IMAGE_SIZE) { vlog("image larger than the space reserved for it\n"); return 0; }  /* would overrun g_image */
    uint8_t* image = g_image;
    memset(image, 0, size);
    rc_guest = image - base;
    memcpy(image, file, *(uint32_t*)(file + pe + 84));  /* headers */
    for (unsigned i = 0; i < nsec; i++) {
        uint8_t* s = file + pe + 24 + opt + 40 * i;
        uint32_t vsize = *(uint32_t*)(s + 8), va = *(uint32_t*)(s + 12), raw = *(uint32_t*)(s + 16), off = *(uint32_t*)(s + 20);
        memcpy(image + va, file + off, raw < vsize ? raw : vsize);
    }
    vlog("sections copied, binding imports\n");
    /* imports: each IAT slot gets its shim's fake address */
    unsigned bound = 0;
    uint32_t imp = *(uint32_t*)(file + pe + 24 + 104);
    for (uint8_t* d = image + imp; *(uint32_t*)(d + 12); d += 20) {
        const char* dll = (const char*)image + *(uint32_t*)(d + 12);
        uint32_t* names = (uint32_t*)(image + (*(uint32_t*)d ? *(uint32_t*)d : *(uint32_t*)(d + 16)));
        uint32_t* slots = (uint32_t*)(image + *(uint32_t*)(d + 16));
        for (; *names; names++, slots++, bound++) {
            char ord[16];
            const char* name = (const char*)image + *names + 2;
            if (*names & 0x80000000u) snprintf(ord, sizeof ord, "#%u", *names & 0xffff), name = ord;
            uint32_t var = crt_data_import(name);  /* variables (_iob, _acmdln ...) point at guest memory, not a shim */
            *slots = var ? var : shim_addr(dll, name, pops_of(dll, name));
        }
    }
    uint32_t entry = base + *(uint32_t*)(file + pe + 40);
    free(file);
    vlog("USM.exe loaded at %p (guest %06x), %u imports bound\n", image, base, bound);
    return entry;
}

int main(void) {
#ifdef USM_VPC
    g_image = vpc_image_alloc(IMAGE_SIZE);
#else
    int settings_requested(void);
    void settings_run(void);
    if (settings_requested()) settings_run();  /* LiveArea's Settings button: the menu, then quit (settings.c) */
    g_image = memalign(0x10000, IMAGE_SIZE);
#endif
    vlog("usm.vpk started (build " __DATE__ " " __TIME__ ", heap %d MB, image at %p)\n", _newlib_heap_size_user >> 20, g_image);
    if (!g_image) { vlog("out of memory for the image\n"); sceKernelExitProcess(1); return 1; }
    extern int g_shim_verbose;
    FILE* t = fopen(USM_DIR "trace.txt", "r");
    if (t) fclose(t), g_shim_verbose = 1, vlog("trace.txt found: logging every shim call\n");
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    /* the game loop runs on this thread: core 0 to itself; the renderer (gl.c) gets core 1, the game's other threads and the mixer
       core 2 */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000);
#if defined(USM_VITAGL) && !defined(USM_VPC)
    if (!exists("ur0:data/libshacccg.suprx") && !exists("ur0:data/external/libshacccg.suprx"))
        fatal("ur0:data/libshacccg.suprx is missing.\nInstall the shader compiler (see the README), then start the game again.");
#endif
    uint32_t entry = load_exe();
    if (!entry) fatal("Cannot load " USM_DIR "USM.exe.\nCopy USM.exe and the Data folder of your PC install to " USM_DIR);
    Ctx* c = ctx_new(0x100000);
#ifdef RC_PROFILE
    rc_prof_start(c);
#else
    (void)c;
#endif
    vlog("starting at the translated entry point %06x\n", entry);
    guest_call(entry, 0, NULL);  /* mainCRTStartup: ends through exit() -> ExitProcess */
    vlog("entry point returned\n");
    sceKernelExitProcess(0);
    return 0;
}
