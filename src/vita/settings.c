/* The Settings menu: LiveArea's Settings button (sce_sys/livearea/contents/template.xml, target psla:-settings)
   relaunches the app with "-settings", and main.c runs this instead of the game. It edits the same one-number files in
   ux0:data/usm/ that the game reads at start (density.txt, distance.txt, ...), so nothing else changes. Drawn straight
   to the display (no vitaGL: the game is not started). D-pad: pick / change, Cross: save and quit, Circle: quit. */
#ifndef USM_VPC
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <string.h>

#include "menu_font.h"

#ifndef USM_DIR
#define USM_DIR "ux0:data/usm/"
#endif

int settings_requested(void) {
    char param[1024] = {0};
    return sceAppMgrGetAppParam(param) >= 0 && strstr(param, "settings");
}

/* A setting: its file, and the values it can take with their labels. A value of NULL means "no file" (delete it): the
   game's default, or a switch that is on when the file exists (debug.txt) */
typedef struct {
    const char* name;
    const char* file;
    int n, def;
    const char* val[8];
    const char* label[8];
    const char* help;
} Setting;
static const Setting S[] = {
    {"People and traffic", "density.txt", 6, 1, {"0", "0.15", "0.3", "0.5", "0.75", "1"},
     {"None", "Very low", "Low", "Medium", "High", "Like PC"}, "The biggest speed setting. PC game: Like PC."},
    {"View distance", "distance.txt", 7, 2, {"80", "100", "120", "150", "200", "300", "0"},
     {"80 m", "100 m", "120 m", "150 m", "200 m", "300 m", "Everything, no fog"}, "Past it the city fades into fog. Longer costs frame rate."},
    {"Resolution", "resolution.txt", 5, 4, {"480x272", "544x308", "640x362", "720x408", "960x544"},
     {"480x272", "544x308", "640x362", "720x408", "960x544 (full)"}, "The 3D scene's size, scaled to the screen."},
    {"Texture size", "texture_max.txt", 4, 1, {"128", "256", "512", "0"}, {"128", "256", "512", "Full"},
     "Largest texture kept. Bigger looks sharper, uses more memory."},
    {"World texture detail", "dxt_native.txt", 4, 2, {"0", "3", "2", "1"}, {"Old (blurry)", "Low", "Medium", "High"},
     "High is sharpest, but far walls shimmer more."},
    {"Far characters", "anim_half.txt", 5, 2, {"0", "8", "12", "20", "40"},
     {"Full rate", "Half rate past 8 m", "Half rate past 12 m", "Half rate past 20 m", "Half rate past 40 m"},
     "Characters far from Spider-Man animate every other frame."},
    {"Far objects", "build_cull.txt", 4, 2, {"0", "1.5", "2", "3"},
     {"Game's own (3 km)", "1.5 x view distance", "2 x view distance", "3 x view distance"},
     "How far the game still prepares objects."},
    {"Frame rate cap", "fps_cap.txt", 4, 2, {"0", "20", "30", "60"}, {"None", "20 fps", "30 fps", "60 fps"},
     "A steady rate can feel smoother than a varying one."},
    {"Fog in cutscenes", "cut_fog.txt", 2, 0, {"0", "1"}, {"Off", "On"},
     "Off shows the far city in cutscenes, which may slow them down."},
    {"Smoke and fire detail", "smoke.txt", 3, 1, {"1", "2", "3"}, {"Full", "Medium", "Low"},
     "Lower draws fewer puffs. Explosions run a lot smoother."},
    {"Map detail", "map_lod.txt", 4, 2, {"0", "2", "4", "8"}, {"Full (can glitch)", "High", "Medium", "Low"},
     "How much small detail the map drops when zoomed far out."},
    {"Language", "language.txt", 5, 0, {"0", "1", "2", "3", "4"}, {"English", "French", "German", "Spanish", "Italian"},
     "Needs that language's game files in Data, else English."},
    {"DEBUG: write log.txt", "debug.txt", 2, 0, {NULL, ""}, {"Off", "On"},
     "Frame times, settings in use and errors, in ux0:data/usm/log.txt."},
    {"DEBUG: early cull", "early_cull.txt", 3, 1, {"0", "1", "2"}, {"Off", "On", "Check (logs mistakes)"},
     "Skips far objects before the game prepares them."},
    {"DEBUG: second core", "pipe.txt", 2, 1, {"0", "2"}, {"Off", "On"},
     "Draws on a second CPU core. Off only to look for bugs."},
    {"DEBUG: more overlap", "overlap.txt", 2, 1, {"0", "1"}, {"Off", "On"},
     "Second core also runs while the next frame is prepared."},
    {"DEBUG: GL state cache", "gl_cache.txt", 2, 1, {"0", "1"}, {"Off", "On"},
     "Skips repeated graphics calls. Off only to look for bugs."},
    {"DEBUG: late release", "release_later.txt", 2, 1, {"0", "1"}, {"Off", "On"},
     "Fewer pauses when the game drops graphics data."},
    {"DEBUG: display sync", "vsync.txt", 2, 1, {"0", "1"}, {"Off", "On"},
     "Holds the frame rate cap on the display's refresh."},
    {"DEBUG: frame dumps", "dump.txt", 3, 0, {NULL, "5", "1"}, {"Off", "Every 5 s", "Every second"},
     "Pictures + draw lists in ux0:data/usm/frames. Very slow."},
};
enum { N = sizeof S / sizeof *S, W = 960, H = 544 };

static uint32_t* g_fb;
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h && j < H; j++)
        for (int i = x; i < x + w && i < W; i++) g_fb[j * W + i] = c;
}
static int text(int x, int y, const char* s, uint32_t c) {  /* c: 0xAABBGGRR */
    for (; *s; s++) {
        int ch = (unsigned char)*s - 32;
        if (ch < 0 || ch >= 95) ch = 0;
        int w = menu_font_w[ch];
        const unsigned char* px = menu_font_px + menu_font_off[ch];
        for (int j = 0; j < MENU_FONT_H; j++)
            for (int i = 0; i < w; i++) {
                unsigned a = px[j * w + i];
                if (!a || x + i >= W || y + j >= H) continue;
                uint32_t* d = &g_fb[(y + j) * W + x + i];
                uint32_t r = ((c & 0xff) * a + (*d & 0xff) * (255 - a)) / 255,
                         g = ((c >> 8 & 0xff) * a + (*d >> 8 & 0xff) * (255 - a)) / 255,
                         b = ((c >> 16 & 0xff) * a + (*d >> 16 & 0xff) * (255 - a)) / 255;
                *d = 0xff000000u | b << 16 | g << 8 | r;
            }
        x += w;
    }
    return x;
}

/* the value index a file holds now (its text matched against the choices), else the default */
static int load(const Setting* s) {
    char p[256], buf[64] = {0};
    snprintf(p, sizeof p, USM_DIR "%s", s->file);
    FILE* f = fopen(p, "r");
    if (!f) {
        for (int i = 0; i < s->n; i++)
            if (!s->val[i]) return i;
        return s->def;
    }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
    for (int i = 0; i < s->n; i++)
        if (s->val[i] && !strcmp(buf, s->val[i])) return i;
    for (int i = 0; i < s->n; i++)  /* debug.txt: any content means on */
        if (s->val[i] && !s->val[i][0]) return i;
    return s->def;
}
static void save(const Setting* s, int v) {
    char p[256];
    snprintf(p, sizeof p, USM_DIR "%s", s->file);
    if (!s->val[v]) {
        sceIoRemove(p);
        return;
    }
    FILE* f = fopen(p, "w");
    if (f) fputs(s->val[v], f), fclose(f);
}

void settings_run(void) {
    enum { FB = 2 * 1024 * 1024 };  /* one 960x544 32-bit frame fits in 2 MB; two of them, drawn in turn */
    uint32_t* base;
    SceUID mem = sceKernelAllocMemBlock("usm_settings_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 2 * FB, NULL);
    if (mem < 0 || sceKernelGetMemBlockBase(mem, (void**)&base) < 0) sceKernelExitProcess(0);
    int v[N], was[N], sel = 0, hold = 0, page = 0, top = 0;
    for (int i = 0; i < N; i++) v[i] = was[i] = load(&S[i]);
    uint32_t prev = ~0u;  /* ignore the buttons held while launching */
    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t b = pad.buttons, down = b & ~prev;
        hold = b & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT) ? hold + 1 : 0;
        if (hold > 24 && hold % 5 == 0) down |= b;  /* held: repeat */
        prev = b;
        if (down & SCE_CTRL_UP) sel = (sel + N - 1) % N;
        if (down & SCE_CTRL_DOWN) sel = (sel + 1) % N;
        if (down & SCE_CTRL_LEFT) v[sel] = (v[sel] + S[sel].n - 1) % S[sel].n;
        if (down & SCE_CTRL_RIGHT) v[sel] = (v[sel] + 1) % S[sel].n;
        if (down & SCE_CTRL_CROSS) {
            for (int i = 0; i < N; i++)  /* only what changed: resolution.txt would override the game's Options */
                if (v[i] != was[i]) save(&S[i], v[i]);
            break;
        }
        if (down & SCE_CTRL_CIRCLE) break;
        if (down & SCE_CTRL_TRIANGLE)
            for (int i = 0; i < N; i++) v[i] = S[i].def;

        g_fb = (uint32_t*)((char*)base + page * FB);
        rect(0, 0, W, H, 0xff201010);  /* dark blue */
        rect(0, 0, W, 56, 0xff2828c0);  /* red title bar */
        text(28, 14, "Ultimate Spider-Man  -  Settings", 0xffffffff);
        enum { ROWS = 14 };  /* what fits above the help bar (H - 76): the list scrolls to keep the selection in view */
        if (sel < top) top = sel;
        if (sel >= top + ROWS) top = sel - ROWS + 1;
        for (int i = top; i < N && i < top + ROWS; i++) {
            int y = 62 + (i - top) * 29;
            if (i == sel) rect(16, y - 2, W - 32, 31, 0xff60402a);
            text(32, y, S[i].name, i == sel ? 0xffffffff : 0xffc8c8c8);
            char lab[96];
            snprintf(lab, sizeof lab, "<  %s  >", S[i].label[v[i]]);
            text(470, y, lab, i == sel ? 0xff40d0ff : 0xffa0a0a0);
        }
        rect(0, H - 76, W, 76, 0xff100808);
        text(28, H - 70, S[sel].help, 0xffd0d0d0);
        text(28, H - 38, "Cross: save and quit     Circle: quit     Triangle: defaults", 0xff40d0ff);
        if (top > 0) text(W - 44, 14, "^", 0xffffffff);
        if (top + ROWS < N) text(W - 44, H - 38, "more", 0xff40d0ff);
        SceDisplayFrameBuf fb = {sizeof fb, g_fb, W, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, W, H};
        sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceDisplayWaitVblankStart();
        page ^= 1;
    }
    sceKernelExitProcess(0);
}
#endif
