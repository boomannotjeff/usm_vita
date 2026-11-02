/* the vitaGL renderer (USM_VITAGL builds): draws from the D3D9 state d3d9_null.c keeps. Shaders and fixed-function
   state become CG programs (d3d_cg.c) cached by what decides their source. The 640x480 backbuffer is letterboxed on
   the 960x544 screen; texture render targets are FBOs drawn upside down (u_flip). Not done yet: mip/cube targets, cube
   textures, fixed-function lighting (vertex or material color only).
   Two threads: the game thread only records commands (the gl_* entry points at the bottom); the render thread, on
   core 1, runs them and is the only caller of vitaGL. Everything above the recording section runs there and reads
   its own copy of the device state, patched from the game's before each draw and clear. */
#include <pthread.h>
#include <stddef.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include "d3d9.h"
#include "d3d_cg.h"
#include "glyphs.h"

static D3DState* const g_main = &st;  /* the game's state (d3d9_null.c): the recording side only */
static D3DState g_rst;                /* the render thread's copy */
#define st g_rst

static uint32_t g_bw, g_bh;  /* the current target: size, placement in its framebuffer, -1 for a texture */
static float g_scale, g_ox, g_oy, g_flip = 1;
static uint32_t bb_w, bb_h;  /* the backbuffer's, letterboxed on the screen */
static float bb_scale, bb_ox, bb_oy;
static GLuint g_fb;
static GLuint g_white, g_black, g_glyphs;  /* g_glyphs: the button icons (glyphs.h) */
static uint32_t g_arrays;  /* attribute arrays enabled */
static double g_dump_every;  /* seconds between frame dumps (dump_frame), 0: off */
static FILE* g_report;       /* open while the frame being dumped draws */
static Res* g_last_rt;       /* the texture target last drawn into in that frame */

static GLuint solid(uint32_t rgba) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgba);
    return t;
}

static void init_(uint32_t bw, uint32_t bh) {
    static int done;
    g_bw = bw ? bw : W, g_bh = bh ? bh : HGT;
    float sx = 960.f / g_bw, sy = 544.f / g_bh;
    g_scale = sx < sy ? sx : sy;
    g_ox = (960.f - g_bw * g_scale) / 2, g_oy = (544.f - g_bh * g_scale) / 2;
    bb_w = g_bw, bb_h = g_bh, bb_scale = g_scale, bb_ox = g_ox, bb_oy = g_oy;
    vlog("gl: backbuffer %ux%u at %.3fx, offset %.0f,%.0f\n", g_bw, g_bh, g_scale, g_ox, g_oy);
    if (done) return;
    done = 1;
    vglInitExtended(0, 960, 544, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);  /* 8 MB of RAM left to the system */
    cg_log = vlog;
    FILE* d = fopen(USM_DIR "dump.txt", "r");
    if (d) {
        if (fscanf(d, "%lf", &g_dump_every) == 1) mkdir(USM_DIR "frames", 0777), vlog("gl: dumping a frame every %.1f s\n", g_dump_every);
        fclose(d);
    }
    g_white = solid(0xffffffffu), g_black = solid(0);
    glGenTextures(1, &g_glyphs);
    glBindTexture(GL_TEXTURE_2D, g_glyphs);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLYPHS_W, GLYPHS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_glyphs_rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static int to_backbuffer(void) { return !st.rt[0] || st.rt[0] == g_backbuffer; }
/* drawing the world view: the whole backbuffer, or a target its size (the scene goes to one, then is copied); not
   the minimap (a small viewport) or small targets, which get no fog and no draw distance. Nor does the pause map: a
   full-screen view from far above, drawn with the game's own fog off. Judged per frame from D3DRS_FOGENABLE as the
   last frame ended (gl_present: on in free roam, off on the map): per draw it is also off for the far skyline and
   the clear, which must stay fogged. Also off after a frame that drew no 3D world in the full view (comic-panel
   cutscenes: their 3D is in small viewports, the full-view clear and backdrop must keep the game's colour) */
static volatile int g_world_fog = 1;
static uint32_t g_world_draws;  /* this frame's fogged, depth-writing full-view draws (recording side) */
static uint32_t g_world_back;   /* times the world view came back (after a mission, the map, a cutscene) */
static int main_view(const D3DState* s) {
    uint32_t w = bb_w, h = bb_h;
    if (s->rt[0] && s->rt[0] != g_backbuffer) {
        Res* r = res(s->rt[0]);
        uint32_t parent = r->parent;
        Res* t = parent ? res(parent) : r;
        if (t->w != bb_w || t->h != bb_h) return 0;
    }
    return s->viewport[0] == 0 && s->viewport[1] == 0 && s->viewport[2] == w && s->viewport[3] == h;
}
static int main_scene(const D3DState* s) { return g_world_fog && main_view(s); }

/* binds st.rt[0]'s framebuffer and sets the g_ target values; a texture target gets an FBO with its own depth */
static int bind_target(void) {
    GLuint fb = 0;
    if (to_backbuffer())
        g_bw = bb_w, g_bh = bb_h, g_scale = bb_scale, g_ox = bb_ox, g_oy = bb_oy, g_flip = 1;
    else {
        Res* s = res(st.rt[0]);
        uint32_t parent = s->parent;  /* read once: the game thread may clear it */
        Res* t = parent ? res(parent) : s;
        if (s->plevel || t->cube) return 0;  /* shortcut: only level 0 of 2D targets, add mips/cubes if the game needs them */
        if (!t->fbo) {
            if (!t->gl) glGenTextures(1, &t->gl);
            glBindTexture(GL_TEXTURE_2D, t->gl);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            GLuint rb;
            glGenRenderbuffers(1, &rb);
            t->rb = rb;
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, t->w, t->h);
            glGenFramebuffers(1, &t->fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g_fb = t->fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->gl, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
            GLenum ok = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            vlog("gl: render target %ux%u format %u%s\n", t->w, t->h, t->fmt, ok == GL_FRAMEBUFFER_COMPLETE ? "" : " INCOMPLETE");
            t->uploaded = 1;
        }
        fb = t->fbo;
        if (g_report) g_last_rt = t;
        g_bw = t->w, g_bh = t->h, g_scale = 1, g_ox = g_oy = 0, g_flip = -1;
    }
    if (fb != g_fb) glBindFramebuffer(GL_FRAMEBUFFER, g_fb = fb);
    return 1;
}

/* D3D rect (top-left origin, target pixels) to GL pixels; a flipped target keeps D3D's rows */
static void rect_gl(float x, float y, float w, float h, GLint* o) {
    o[0] = (GLint)(g_ox + x * g_scale), o[1] = (GLint)(g_oy + (g_flip < 0 ? y : g_bh - y - h) * g_scale);
    o[2] = (GLint)(w * g_scale + 0.5f), o[3] = (GLint)(h * g_scale + 0.5f);
}
static float vpf(int i) { float f; memcpy(&f, &st.viewport[i], 4); return f; }

/* Frame dumps, for comparing against the PC: USM_DIR "dump.txt" holding N makes every frame N seconds apart go to
   USM_DIR "frames/fNNNNN.bmp", with a report of each of its draws in fNNNNN.txt. */
static uint32_t g_frame;
static void report(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_report, fmt, ap);
    va_end(ap);
}
static void write_bmp(const char* path, uint32_t w, uint32_t hh) {
    static uint8_t px[1024 * 1024 * 4];
    if (w * hh > 1024 * 1024) return;
    glReadPixels(0, 0, w, hh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * hh;
    uint8_t h[54] = {'B', 'M'};
    memcpy(h + 2, &size, 4), h[10] = 54, h[14] = 40;
    memcpy(h + 18, &w, 4), memcpy(h + 22, &hh, 4), h[26] = 1, h[28] = 24;
    fwrite(h, 1, 54, f);
    for (uint32_t i = 0; i < w * hh; i++) {  /* GL's rows are bottom first, like a BMP's */
        uint8_t bgr[3] = {px[4 * i + 2], px[4 * i + 1], px[4 * i]};
        fwrite(bgr, 1, 3, f);
        static const uint8_t pad[4];
        if ((i + 1) % w == 0) fwrite(pad, 1, row - w * 3, f);
    }
    fclose(f);
}
static void dump_frame(void) {
    static uint64_t next;
    char path[256];
    g_frame++;
    if (g_report) {
        fclose(g_report), g_report = NULL;
        snprintf(path, sizeof path, USM_DIR "frames/f%05u.bmp", g_frame - 1);
        write_bmp(path, 960, 544);
        if (g_last_rt && g_last_rt->fbo) {  /* and the last texture target drawn into, as GL holds it (bottom up) */
            glBindFramebuffer(GL_FRAMEBUFFER, g_last_rt->fbo);
            snprintf(path, sizeof path, USM_DIR "frames/f%05u_rt.bmp", g_frame - 1);
            write_bmp(path, g_last_rt->w, g_last_rt->h);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        g_last_rt = NULL;
        vlog("gl: frame %u dumped\n", g_frame - 1);
    }
    uint64_t now = g_dump_every > 0 ? sceKernelGetProcessTimeWide() : 0;
    if (g_dump_every > 0 && now >= next) {
        next = now + (uint64_t)(g_dump_every * 1e6);
        snprintf(path, sizeof path, USM_DIR "frames/f%05u.txt", g_frame);
        g_report = fopen(path, "w");
    }
}

/* ux0:data/usm/fps_cap.txt (e.g. 20): frames shown at a steady rate instead of whenever ready. Paced here on the
   render thread; the game thread follows, as it waits for the render thread before handing it the next frame */
static void pace(void) {
    static int cap = -1;
    static uint64_t next;
    if (cap < 0) {
        cap = 0;
        FILE* f = fopen(USM_DIR "fps_cap.txt", "r");
        if (f) {
            if (fscanf(f, "%d", &cap) != 1 || cap < 5 || cap > 60) cap = 0;
            fclose(f);
        }
        vlog("gl: fps cap %d (fps_cap.txt)\n", cap);
    }
    if (!cap) return;
    uint64_t now = sceKernelGetProcessTimeWide(), period = 1000000u / cap;
    if (now < next) sceKernelDelayThread((SceUInt)(next - now)), now = next;
    next = now - next < period ? next + period : now + period;  /* late by a whole frame: restart from now */
}

static void present_(void) {
    if (g_fb) glBindFramebuffer(GL_FRAMEBUFFER, g_fb = 0);
    dump_frame();
    pace();
    vglSwapBuffers(GL_FALSE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1), glDepthMask(1), glStencilMask(0xff);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);  /* the letterbox bars */
}

static float far_dist(void);
static void clear_(uint32_t count, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!bind_target()) return;
    GLbitfield m = (flags & 1 ? GL_COLOR_BUFFER_BIT : 0) | (flags & 2 ? GL_DEPTH_BUFFER_BIT : 0) |
                   (flags & 4 ? GL_STENCIL_BUFFER_BIT : 0);
    if (!m) return;
    if (g_report) report("clear %x color %08x z %g stencil %u rects %u to %s %x %ux%u ds %x vp %u,%u %ux%u\n", flags, color, z, stencil, count,
                         to_backbuffer() ? "bb" : "rt", st.rt[0], g_bw, g_bh, st.ds, st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3]);
    /* the world view's opaque clear goes to the game's fog color: the sky dome does not reach the horizon, and past the
       draw distance the dark clear color showed through as grey slabs */
    if (flags & 1 && color >> 24 == 0xff && st.rs[34] && far_dist() > 0 && main_scene(&st)) color = st.rs[34] | 0xff000000u;
    glClearColor((color >> 16 & 255) / 255.f, (color >> 8 & 255) / 255.f, (color & 255) / 255.f, (color >> 24) / 255.f);
    glClearDepthf(z);
    glClearStencil((GLint)stencil);
    glColorMask(1, 1, 1, 1), glDepthMask(1), glStencilMask(0xff);
    glEnable(GL_SCISSOR_TEST);
    GLint r[4];
    /* D3D9 clears the viewport, cut to the scissor rect when the scissor test is on (the minimap clears this way) */
    int32_t c0[4] = {(int32_t)st.viewport[0], (int32_t)st.viewport[1], (int32_t)(st.viewport[0] + st.viewport[2]),
                     (int32_t)(st.viewport[1] + st.viewport[3])};
    if (st.rs[174]) {
        const int32_t* s = (const int32_t*)st.scissor;
        for (int j = 0; j < 2; j++) c0[j] = c0[j] > s[j] ? c0[j] : s[j], c0[j + 2] = c0[j + 2] < s[j + 2] ? c0[j + 2] : s[j + 2];
    }
    uint32_t n = count && rects ? (count < 64 ? count : 64) : 1;
    for (uint32_t i = 0; i < n; i++) {  /* D3DRECT {x1, y1, x2, y2} */
        int32_t x1 = c0[0], y1 = c0[1], x2 = c0[2], y2 = c0[3];
        if (count && rects) {
            const int32_t* q = rects + 4 * i;
            x1 = x1 > q[0] ? x1 : q[0], y1 = y1 > q[1] ? y1 : q[1], x2 = x2 < q[2] ? x2 : q[2], y2 = y2 < q[3] ? y2 : q[3];
        }
        if (x2 <= x1 || y2 <= y1) continue;
        rect_gl((float)x1, (float)y1, (float)(x2 - x1), (float)(y2 - y1), r);
        glScissor(r[0], r[1], r[2], r[3]);
        glClear(m);
    }
    glDisable(GL_SCISSOR_TEST);
}

/* ---- textures: level 0 goes up on unlock, smaller levels come from glGenerateMipmap ---- */
static uint8_t* scratch(uint32_t n) {
    static uint8_t* b;
    static uint32_t cap;
    if (n > cap) free(b), b = malloc(cap = n);
    return b;
}
/* DXT1 to RGBA8, w x h (a multiple of 4) */
static void dxt1_decode(const uint8_t* p, uint32_t w, uint32_t h, uint8_t* out) {
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++, p += 8) {
            uint16_t c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8;
            uint8_t pal[4][4];
            for (int i = 0; i < 2; i++) {
                uint16_t c = i ? c1 : c0;
                pal[i][0] = (c >> 11) * 255 / 31, pal[i][1] = (c >> 5 & 63) * 255 / 63, pal[i][2] = (c & 31) * 255 / 31, pal[i][3] = 255;
            }
            for (int k = 0; k < 4; k++) {  /* c0 <= c1: the 4th colour is transparent black */
                pal[2][k] = c0 > c1 ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
                pal[3][k] = c0 > c1 ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
            }
            uint32_t idx = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
            for (int i = 0; i < 16; i++)
                memcpy(out + 4 * ((by * 4 + i / 4) * w + bx * 4 + (i & 3)), pal[idx >> 2 * i & 3], 4);
        }
}
static int g_texmax = -1;
static void texmax_load(void);
/* draw distance (ux0:data/usm/distance.txt, view-depth units; 0 = everything, no fog): static world meshes wholly
   beyond it are not drawn (gl_draw), and fog fades in over its last 45% so the edge does not show */
static float far_dist(void) {
    static float far = -1;
    if (far < 0) {
        far = 120;
        FILE* f = fopen(USM_DIR "distance.txt", "r");
        if (f) {
            if (fscanf(f, "%f", &far) != 1) far = 120;
            fclose(f);
        }
        if (getenv("VPC_FAR")) far = (float)atof(getenv("VPC_FAR"));  /* vpc tests */
        vlog("gl: draw distance %.0f (distance.txt; 0 = everything, no fog)\n", far);
    }
    return far;
}
float gl_world_dist(void) { return g_world_fog ? far_dist() : 0; }
/* a world texture (DXT1 with mip levels: walls, ground; UI art like the minimap has one level, fonts are DXT3) goes up
   decoded at an eighth of its size, at least 8 px a side, as 16 bits: PS2-like and lighter than the DXT. Tiled walls
   still looked sharp at a quarter */
static int upload_dxt_small(uint32_t f, const uint8_t* p, uint32_t w, uint32_t h) {
    texmax_load();
    if (!g_texmax || f != FMT_DXT1 || w < 16 || h < 16 || w % 4 || h % 4) return 0;
    uint32_t s = 3;
    while (s > 1 && ((w >> s) < 8 || (h >> s) < 8)) s--;
    uint8_t* rgba = scratch(w * h * 4 + (w >> s) * (h >> s) * 2);
    uint16_t* o = (uint16_t*)(rgba + w * h * 4);
    dxt1_decode(p, w, h, rgba);
    uint32_t ow = w >> s, oh = h >> s, k = 1u << s;
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t sum[4] = {0};
            for (uint32_t j = 0; j < k; j++)
                for (uint32_t i = 0; i < k; i++)
                    for (int c = 0; c < 4; c++) sum[c] += rgba[4 * ((y * k + j) * w + x * k + i) + c];
            uint32_t n = k * k, r = sum[0] / n, g = sum[1] / n, b = sum[2] / n, a = sum[3] / n;
            o[y * ow + x] = (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128));
        }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ow, oh, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, o);
    return 1;
}
static void upload_(Res* r, const uint8_t* p) {  /* level r->base, copied when the game unlocked it */
    uint32_t w = r->w >> r->base, h = r->h >> r->base, f = r->fmt;
    if (!w) w = 1;
    if (!h) h = 1;
    uint32_t n = w * h;
    if (!r->gl) glGenTextures(1, &r->gl);
    glBindTexture(GL_TEXTURE_2D, r->gl);
    int mips = r->levels > 1;
    if (is_dxt(f) && !(r->usage & 0x601) && r->levels > 1 && upload_dxt_small(f, p, w, h))
        mips = 0;  /* vitaGL's glGenerateMipmap garbles RGBA5551 on the Vita (rainbow noise past the first mip) */
    else if (is_dxt(f)) {
        GLenum gf = f == FMT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                  : f <= 0x33545844u ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, gf, w, h, 0, pitch_of(f, w) * rows_of(f, h), p);
        mips = 0;
    } else
        switch (f) {
        case 21: case 22: {  /* A8R8G8B8, X8R8G8B8: BGRA bytes */
            uint8_t* o = scratch(n * 4);
            for (uint32_t i = 0; i < n; i++)
                o[4 * i] = p[4 * i + 2], o[4 * i + 1] = p[4 * i + 1], o[4 * i + 2] = p[4 * i], o[4 * i + 3] = f == 22 ? 255 : p[4 * i + 3];
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, o);
            break;
        }
        case 23: glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p); break;
        case 24: case 25: case 26: case 30: {  /* X1R5G5B5, A1R5G5B5, A4R4G4B4, X4R4G4B4: alpha moves to the low bits */
            uint16_t* o = (uint16_t*)scratch(n * 2);
            const uint16_t* s = (const uint16_t*)p;
            int is4 = f == 26 || f == 30;
            for (uint32_t i = 0; i < n; i++)
                o[i] = is4 ? (uint16_t)(s[i] << 4 | (f == 30 ? 15 : s[i] >> 12)) : (uint16_t)(s[i] << 1 | (f == 24 ? 1 : s[i] >> 15));
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, is4 ? GL_UNSIGNED_SHORT_4_4_4_4 : GL_UNSIGNED_SHORT_5_5_5_1, o);
            break;
        }
        case 28: glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, p); break;
        case 50: glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, p); break;
        case 51: glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, w, h, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, p); break;
        default:
            vlog("gl: texture format %u not uploaded\n", f);
            r->uploaded = 0;
            return;
        }
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
    r->uploaded = 1 | (mips ? 2 : 0);
}

static GLenum wrap(uint32_t a) { return a == 2 || a == 5 ? GL_MIRRORED_REPEAT : a == 3 || a == 4 ? GL_CLAMP_TO_EDGE : GL_REPEAT; }
static void bind_textures(void) {
    for (int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        Res* r = st.tex[i] ? res(st.tex[i]) : NULL;
        if (!r || r->cube || !r->uploaded) {  /* never written (render targets): transparent black adds nothing */
            glBindTexture(GL_TEXTURE_2D, r && r->usage & 1 ? g_black : g_white);
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, r->gl);
        const uint32_t* s = st.ss[i];
        int lin = s[6] >= 2, mip = r->uploaded & 2 ? s[7] : 0;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s[5] >= 2 ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        mip == 0 ? (lin ? GL_LINEAR : GL_NEAREST)
                        : mip == 1 ? (lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                                   : (lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap(s[1]));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap(s[2]));
    }
    glActiveTexture(GL_TEXTURE0);
}

/* ---- vertex input ---- */
/* A static vertex or index buffer lives in a GL buffer, sent (C_BUFFER) when first drawn after a write lock, so
   vitaGL draws it without copying. Dynamic ones, and draws that mix them, copy their vertices into the command. */
static void buffer_(Res* b, GLenum target, const void* data) {
    if (!b->gl) glGenBuffers(1, &b->gl);
    glBindBuffer(target, b->gl);
    glBufferData(target, b->size, data, GL_STATIC_DRAW);
}

typedef struct {
    uint8_t stream, type, usage, index;
    uint16_t off;
} Elem;
static int elems_of(const D3DState* s, Elem* e) {
    int n = 0;
    if (s->decl) {
        const uint8_t* d = (const uint8_t*)obj_copy(s->decl, NULL);
        for (; n < 32 && d[0] != 0xff; n++, d += 8)
            e[n] = (Elem){d[0], d[4], d[6], d[7], (uint16_t)(d[2] | d[3] << 8)};
        return n;
    }
    uint32_t f = s->fvf, off = 0, pos = f & 0x400e;
#define ADD(t, u, i, sz) (e[n++] = (Elem){0, t, u, i, (uint16_t)off}, off += sz)
    if (pos == 4) ADD(3, 9, 0, 16);                       /* XYZRHW */
    else if (pos == 0x4002) ADD(3, 0, 0, 16);             /* XYZW */
    else if (pos) { ADD(2, 0, 0, 12); off += 4 * ((pos - 2) / 2); }  /* XYZ, XYZB1-5: blend weights skipped */
    if (f & 0x10) ADD(2, 3, 0, 12);                       /* normal */
    if (f & 0x20) off += 4;                               /* point size */
    if (f & 0x40) ADD(4, 10, 0, 4);                       /* diffuse */
    if (f & 0x80) ADD(4, 10, 1, 4);                       /* specular */
    for (uint32_t i = 0, k = f >> 8 & 15; i < k && i < 8; i++) {
        uint32_t c = f >> (16 + 2 * i) & 3;               /* 0: 2 floats, 1: 3, 2: 4, 3: 1 */
        ADD(c == 3 ? 0 : c + 1, 5, i, c == 3 ? 4 : 4 * (c + 2));
    }
#undef ADD
    return n;
}
static const struct {
    uint8_t n;
    uint16_t gl;
    uint8_t norm;
} TYPES[17] = {{1, GL_FLOAT, 0}, {2, GL_FLOAT, 0}, {3, GL_FLOAT, 0}, {4, GL_FLOAT, 0}, {4, GL_UNSIGNED_BYTE, 1},
               {4, GL_UNSIGNED_BYTE, 0}, {2, GL_SHORT, 0}, {4, GL_SHORT, 0}, {4, GL_UNSIGNED_BYTE, 1}, {2, GL_SHORT, 1},
               {4, GL_SHORT, 1}, {2, GL_UNSIGNED_SHORT, 1}, {4, GL_UNSIGNED_SHORT, 1}, {0, 0, 0}, {0, 0, 0},
               {2, GL_HALF_FLOAT, 0}, {4, GL_HALF_FLOAT, 0}};

/* vertex shader inputs, read once per shader object */
typedef struct {
    uint32_t obj, mask;
    uint8_t usage[16], index[16];
} Inputs;
static const Inputs* inputs_of(uint32_t vs) {
    static Inputs c[256];
    Inputs* e = &c[(vs >> 4) & 255];
    if (e->obj != vs) {
        memset(e, 0, sizeof *e);
        e->obj = vs;
        e->mask = cg_inputs(obj_copy(vs, NULL), e->usage, e->index);
    }
    return e;
}

/* ---- programs ---- */
typedef struct {
    uint32_t vs, ps, swz, sizes, flags, afunc;  /* flags: 1 pre-transformed, 2 material color */
    FfpPixel fp;
} Key;
typedef struct {
    Key k;
    GLuint prog;
    GLint vc, pc, fix, m, mat, tf, aref, flip, fog, fogc;
    int used, id;
    /* vertex constants: the vc[96] array (p->vc, skinning shaders) or, when d3d_cg.c declared only the ones the shader
       reads, one uniform each: location and register */
    uint8_t nvc, vcr[96];
    GLint vcl[96];
} Prog;
static Prog g_progs[1024];

static GLuint compile(GLenum type, const char* src) {
    if (!src) return 0;
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char b[1024] = "";
        glGetShaderInfoLog(s, sizeof b, NULL, b);
        vlog("gl: CG compile failed: %s\n%s\n", b, src);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
static uint32_t fnv(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
/* Linked programs persist on the memory card (as in mk_wiita): ux0:data/usm/shadercache/<hash of both sources>.bin,
   so a shader compiles once ever instead of stalling 1-3 s every launch. A changed translator changes the source and
   so the name. ux0:data/usm/no-shader-cache.txt turns it off. */
#define CACHE_DIR USM_DIR "shadercache"
#ifdef __vita__
static int cache_on(void) {
    static int on = -1;
    if (on < 0) {
        FILE* f = fopen(USM_DIR "no-shader-cache.txt", "rb");
        on = !f;
        if (f) fclose(f);
    }
    return on;
}
static void cache_path(char* out, const char* vs, const char* ps) {
    uint64_t h = 1469598103934665603ull;
    for (const char* s = vs; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    h = (h ^ 0xff) * 1099511628211ull;
    for (const char* s = ps; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    snprintf(out, 128, CACHE_DIR "/%016llx.bin", (unsigned long long)h);
}
/* the whole cache is read into memory once (~130 programs, ~200 KB), as mk_wiita preloads its shaders: a program's
   first use in play then does no memory card I/O on the render thread */
typedef struct { char name[24]; void* d; long n; } CacheEnt;
static CacheEnt* g_cache;
static int g_ncache = -1;
static void cache_preload(void) {
    g_ncache = 0;
    DIR* dir = opendir(CACHE_DIR);
    if (!dir) return;
    int cap = 0;
    for (struct dirent* e; (e = readdir(dir));) {
        if (strlen(e->d_name) >= sizeof g_cache->name) continue;
        char path[128];
        snprintf(path, sizeof path, CACHE_DIR "/%s", e->d_name);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        void* d = n > 0 ? malloc(n) : NULL;
        if (d && fread(d, 1, n, f) == (size_t)n) {
            if (g_ncache == cap) g_cache = realloc(g_cache, (cap = cap ? 2 * cap : 256) * sizeof *g_cache);
            CacheEnt* c = &g_cache[g_ncache++];
            strcpy(c->name, e->d_name), c->d = d, c->n = n, d = NULL;
        }
        free(d);
        fclose(f);
    }
    closedir(dir);
    vlog("gl: %d cached programs preloaded\n", g_ncache);
}
static int cache_load(GLuint pr, const char* path) {
    if (!*path || !cache_on()) return 0;
    if (g_ncache < 0) cache_preload();
    const char* name = strrchr(path, '/') + 1;
    for (int i = 0; i < g_ncache; i++)
        if (!strcmp(g_cache[i].name, name)) {
            glProgramBinary(pr, 0, g_cache[i].d, (GLsizei)g_cache[i].n);
            free(g_cache[i].d), g_cache[i] = g_cache[--g_ncache];  /* used once: a program is made once per run */
            return 1;
        }
    return 0;
}
static void cache_save(GLuint pr, const char* path) {
    if (!*path || !cache_on()) return;
    GLint n = 0;
    glGetProgramiv(pr, GL_PROGRAM_BINARY_LENGTH, &n);
    if (n <= 0) return;
    void* d = malloc(n + 64);
    GLsizei w = 0;
    GLenum fmt = 0;
    glGetProgramBinary(pr, n + 64, &w, &fmt, d);
    if (w > 0) {
        mkdir(CACHE_DIR, 0777);
        FILE* f = fopen(path, "wb");
        if (f) fwrite(d, 1, w, f), fclose(f);
    }
    free(d);
}
#else  /* vpc: no program binaries */
static void cache_path(char* out, const char* vs, const char* ps) { (void)vs, (void)ps, *out = 0; }
static int cache_load(GLuint pr, const char* path) { (void)pr, (void)path; return 0; }
static void cache_save(GLuint pr, const char* path) { (void)pr, (void)path; }
#endif
static Prog* program(const Key* k) {
    uint32_t h = fnv(k, sizeof *k);
    Prog* p = NULL;
    for (int i = 0; i < 1024; i++) {
        p = &g_progs[(h + i) & 1023];
        if (!p->used || !memcmp(&p->k, k, sizeof *k)) break;
    }
    if (p->used) return p->prog ? p : NULL;
    p->used = 1, p->k = *k;
    uint64_t t0 = sceKernelGetProcessTimeWide();  /* a program's first use stalls the render thread: logged */
    char* vs = k->vs ? cg_vertex(obj_copy(k->vs, NULL), k->swz, k->sizes) : cg_ffp_vertex(k->flags & 1, k->flags >> 1 & 1, k->swz, k->sizes);
    static const char* DBG_PS[] = {  /* VPC_DBG modes 1-4: magenta, vertex color, texture 0 alone, texcoord 0 as RG */
        "", "gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);", "gl_FragColor = vec4(xD0.rgb, 1.0);",
        "gl_FragColor = vec4(texture2D(s0, xT0.xy).rgb, 1.0);", "gl_FragColor = vec4(fract(xT0.xy), 0.0, 1.0);"};
    char dbg_ps[512];
    if (k->flags >> 3) snprintf(dbg_ps, sizeof dbg_ps, "precision highp float;\nvarying vec4 xD0;\nvarying vec4 xT0;\nuniform sampler2D s0;\n"
                                "void main() { %s }\n", DBG_PS[k->flags >> 3 & 7]);
    char* ps = k->flags >> 3 ? strdup(dbg_ps)
             : k->ps ? cg_pixel(obj_copy(k->ps, NULL), k->afunc) : cg_ffp_pixel(&k->fp, k->afunc);
    static int nsrc;
    if (g_dump_every > 0) {  /* with frame dumps: each program's source, frames/progN.txt (N as in the reports) */
        char path[256];
        snprintf(path, sizeof path, USM_DIR "frames/prog%d.txt", ++nsrc);
        FILE* sf = fopen(path, "w");
        if (sf) fprintf(sf, "%s\n----\n%s\n", vs ? vs : "", ps ? ps : ""), fclose(sf);
    }
    GLuint pr = glCreateProgram();
    char bin[128] = "";
    if (vs && ps) cache_path(bin, vs, ps);
    if (!cache_load(pr, bin)) {
        GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, ps);
        if (!v || !f) {
            free(vs), free(ps);  /* the empty program leaks: only when a shader fails, which is logged */
            return NULL;
        }
        glAttachShader(pr, v), glAttachShader(pr, f);
        for (int i = 0; i < 16; i++) {
            char n[8];
            snprintf(n, sizeof n, "i%d", i);
            glBindAttribLocation(pr, i, n);
        }
        glLinkProgram(pr);
        cache_save(pr, bin);
    }
    free(vs), free(ps);
    p->prog = pr;
    glUseProgram(pr);
    for (int i = 0; i < 4; i++) {
        char n[8];
        snprintf(n, sizeof n, "s%d", i);
        GLint l = glGetUniformLocation(pr, n);
        if (l >= 0) glUniform1i(l, i);
    }
    p->vc = glGetUniformLocation(pr, "vc"), p->pc = glGetUniformLocation(pr, "pc");
    p->nvc = 0;
    if (p->vc < 0)
        for (int r = 0; r < 96; r++) {
            char n[8];
            snprintf(n, sizeof n, "vc_%d", r);
            GLint l = glGetUniformLocation(pr, n);
            if (l >= 0) p->vcl[p->nvc] = l, p->vcr[p->nvc++] = (uint8_t)r;
        }
    p->fix = glGetUniformLocation(pr, "u_fix"), p->m = glGetUniformLocation(pr, "m");
    p->mat = glGetUniformLocation(pr, "u_mat"), p->tf = glGetUniformLocation(pr, "u_tf");
    p->aref = glGetUniformLocation(pr, "u_aref"), p->flip = glGetUniformLocation(pr, "u_flip");
    p->fog = glGetUniformLocation(pr, "u_fog"), p->fogc = glGetUniformLocation(pr, "u_fogc");
    static int n;
    vlog("gl: program %d (vs %08x ps %08x flags %u) in %.1f ms\n", p->id = ++n, k->vs, k->ps, k->flags,
         (sceKernelGetProcessTimeWide() - t0) / 1000.0);
    return p;
}

static void ffp_pixel(FfpPixel* f) {
    for (f->n = 0; f->n < 4 && st.tss[f->n][1] != 1; f->n++) {
        const uint32_t* t = st.tss[f->n];
        f->s[f->n] = (FfpStage){t[1], t[2], t[3], t[26], t[4], t[5], t[6], t[27], t[11] & 3, t[28] == 5 ? 5 : 1};
    }
    f->specular = st.rs[29] != 0;
}

static void mat4(const float* a, const float* b, float* r) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
}
static void rgba(uint32_t c, float* o) { o[0] = (c >> 16 & 255) / 255.f, o[1] = (c >> 8 & 255) / 255.f, o[2] = (c & 255) / 255.f, o[3] = (c >> 24) / 255.f; }

static GLenum cmp(uint32_t f) { return f >= 1 && f <= 8 ? GL_NEVER + f - 1 : GL_ALWAYS; }
static GLenum blend(uint32_t b) {
    static const GLenum t[16] = {GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                                 GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                 GL_ONE, GL_ZERO};  /* BLENDFACTOR: no constant color in vitaGL */
    return b < 16 ? t[b] : GL_ONE;
}
static GLenum sop(uint32_t o) {
    static const GLenum t[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    return o < 9 ? t[o] : GL_KEEP;
}
static void apply_state(int rhw) {
    const uint32_t* rs = st.rs;
    GLint r[4];
    if (rhw) rect_gl(0, 0, (float)g_bw, (float)g_bh, r);
    else rect_gl((float)st.viewport[0], (float)st.viewport[1], (float)st.viewport[2], (float)st.viewport[3], r);
    glViewport(r[0], r[1], r[2], r[3]);
    glDepthRangef(vpf(4), vpf(5));
    if (rs[174]) {  /* scissor: RECT {left, top, right, bottom} */
        int32_t* s = (int32_t*)st.scissor;
        /* the minimap's box (a small one) is its map area: the frame, compass and edge markers drawn over its border
           were cut off, so they get 12 px more, and everything above it (the objective marker's pole pokes out of the
           top, as on PC); the map tiles (depth test on, func ALWAYS) keep the exact box */
        int g = (uint32_t)(s[2] - s[0]) < g_bw / 2 && !(rs[7] && rs[23] == 8) ? 12 : 0;
        int top = g ? 0 : s[1];
        rect_gl((float)(s[0] - g), (float)top, (float)(s[2] - s[0] + 2 * g), (float)(s[3] + g - top), r);
        glEnable(GL_SCISSOR_TEST);
        glScissor(r[0], r[1], r[2], r[3]);
    } else
        glDisable(GL_SCISSOR_TEST);
    if (rs[7]) glEnable(GL_DEPTH_TEST), glDepthFunc(cmp(rs[23]));
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[14] ? GL_TRUE : GL_FALSE);
    /* D3DRS_DEPTHBIAS (195, in depth units) and SLOPESCALEDEPTHBIAS (175): decals such as wall posters sit on their
       wall this way; without it they z-fought and flashed at a distance. One D24 step is 2^-24 */
    if (rs[195] || rs[175]) {
        float bias, slope;
        memcpy(&bias, &rs[195], 4), memcpy(&slope, &rs[175], 4);
        static int said;
        if (!said) said = 1, vlog("gl: depth bias in use (%g, slope %g)\n", bias, slope);
        glEnable(GL_POLYGON_OFFSET_FILL), glPolygonOffset(slope, bias * 16777216.f);
    } else
        glDisable(GL_POLYGON_OFFSET_FILL);
    /* D3D measures winding in screen coords (y down), GL in window coords (y up); a D3D-clockwise triangle has the
       same signed area as a GL-counterclockwise one when the rows line up (texture targets, u_flip), the opposite on
       the backbuffer. Checked on vpc: the outline hulls and rooftops only come out right this way. */
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else glEnable(GL_CULL_FACE), glCullFace(GL_BACK), glFrontFace((rs[22] == 3) == (g_flip > 0) ? GL_CW : GL_CCW);
    if (rs[27]) {
        glEnable(GL_BLEND);
        uint32_t s = rs[19], d = rs[20];
        if (s == 12) s = 5, d = 6;
        else if (s == 13) s = 6, d = 5;
        glBlendFunc(blend(s), blend(d));
        static const GLenum ops[6] = {GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
        glBlendEquation(rs[171] < 6 ? ops[rs[171]] : GL_FUNC_ADD);
    } else
        glDisable(GL_BLEND);
    uint32_t cw = rs[168];
    glColorMask(cw & 1, cw >> 1 & 1, cw >> 2 & 1, cw >> 3 & 1);
    if (rs[52]) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(cmp(rs[56]), (GLint)rs[57], rs[58]);
        glStencilOp(sop(rs[53]), sop(rs[54]), sop(rs[55]));
        glStencilMask(rs[59]);
    } else
        glDisable(GL_STENCIL_TEST);
}

static int count_of(uint32_t prim, uint32_t n, GLenum* mode) {
    switch (prim) {
    case 1: *mode = GL_POINTS; return (int)n;
    case 2: *mode = GL_LINES; return (int)n * 2;
    case 3: *mode = GL_LINE_STRIP; return (int)n + 1;
    case 4: *mode = GL_TRIANGLES; return (int)n * 3;
    case 5: *mode = GL_TRIANGLE_STRIP; return (int)n + 2;
    case 6: *mode = GL_TRIANGLE_FAN; return (int)n + 2;
    }
    return 0;
}

/* a draw as recorded: client mode carries its vertices (from the first one used) and rebased indices; GPU mode
   draws the bound static buffers, base vertex by attribute offset */
typedef struct {
    uint32_t prim, n, base;
    int32_t idx;                    /* -1: none; GPU: byte offset in the index buffer; client: offset in this command */
    uint8_t idx32, client, nofog, icon;  /* nofog: the sky dome (gl_draw); icon: a button icon glyph (icon_quad) */
    uint32_t data[16], stride[16];  /* client: offset in this command of each stream's vertices (0: none) */
} DrawCmd;

static void draw_(const DrawCmd* d) {
    GLenum mode;
    int count = count_of(d->prim, d->n, &mode);
    if (!count || !bind_target()) return;
    Elem e[32];
    int ne = elems_of(&st, e);
    /* pretransformed vertices (POSITIONT, FVF XYZRHW) skip the vertex shader even with one set (as D3D9, and DXVK's
       UseProgrammableVS); the options menu draws its panels that way with the 2D shader still set */
    int post = 0;
    for (int i = 0; i < ne; i++) post |= e[i].usage == 9;
    uint32_t vs = post ? 0 : st.vs;
    const Inputs* in = vs ? inputs_of(vs) : NULL;
    Key k;
    memset(&k, 0, sizeof k);
    k.vs = vs, k.ps = st.ps;
    uint32_t arrays = 0, has_d0 = 0;
    for (int i = 0; i < ne; i++) {
        int loc = -1;
        if (in) {
            for (int r = 0; r < 16; r++)
                if (in->mask >> r & 1 && in->usage[r] == e[i].usage && in->index[r] == e[i].index) { loc = r; break; }
        } else if (e[i].usage == 0 || e[i].usage == 9) {
            loc = FFP_POS;
            if (e[i].usage == 9) k.flags |= 1;
        } else if (e[i].usage == 10 && e[i].index < 2)
            loc = FFP_D0 + e[i].index, has_d0 |= e[i].index == 0;
        else if (e[i].usage == 5 && e[i].index < 4)
            loc = FFP_T0 + e[i].index;
        if (loc < 0 || arrays >> loc & 1 || e[i].type > 16 || !TYPES[e[i].type].n) continue;
        const uint8_t* p;
        uint32_t stride, sn = e[i].stream & 15;
        if (d->client) {
            if (!d->data[sn]) continue;
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            p = (const uint8_t*)d + d->data[sn], stride = d->stride[sn];
        } else {
            Res* vb = st.vb[sn] ? res(st.vb[sn]) : NULL;
            if (!vb || !vb->gl) continue;
            glBindBuffer(GL_ARRAY_BUFFER, vb->gl);
            stride = st.vbstride[sn], p = (const uint8_t*)(uintptr_t)(st.vboff[sn] + d->base * stride);
        }
        p += e[i].off;
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, TYPES[e[i].type].n, TYPES[e[i].type].gl, TYPES[e[i].type].norm, stride, p);
        arrays |= 1u << loc;
        if (e[i].type == 4) k.swz |= 1u << loc;
        k.sizes |= (4u - TYPES[e[i].type].n) << 2 * loc;
    }
    for (int l = 0; l < 16; l++)
        if (g_arrays >> l & 1 && !(arrays >> l & 1)) glDisableVertexAttribArray(l);
    g_arrays = arrays;
    uint32_t want = in ? in->mask : 0x7f;
    for (int l = 0; l < 16; l++)
        if (want >> l & 1 && !(arrays >> l & 1)) {
            float v = !in && l == FFP_D0 ? 1.f : 0.f;
            glVertexAttrib4f(l, v, v, v, !in && l == FFP_D1 ? 0.f : 1.f);
        }
    int rhw = k.flags & 1;
    if (!in && !rhw && st.rs[137] && !has_d0) k.flags |= 2;
    if (st.rs[15]) k.afunc = st.rs[25] ? st.rs[25] : 8;
    if (k.afunc == 8) k.afunc = 0;
    if (!st.ps) ffp_pixel(&k.fp);
#ifdef USM_DEBUG
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("VPC_DBG") ? atoi(getenv("VPC_DBG")) : 0;
    if (dbg >= 1 && dbg <= 4 && !to_backbuffer() && !rhw && d->n > 4) k.flags |= dbg << 3;  /* debug: 3D draws into targets */
#endif
    Prog* p = program(&k);
    /* the purple HUD messages ("NEW CITY GOALS!") never show on the Vita: they seem to go through small texture targets
       (387x143 ones appear and go around them). The first draw that samples each such target, for the log */
    if (log_on() && st.tex[0]) {
        Res* t = res(st.tex[0]);
        static Res* seen[16];
        static unsigned ns;
        int known = 0;
        for (int i = 0; i < 16; i++) known |= seen[i] == t;
        if (t->fbo && !known) {
            seen[ns++ & 15] = t;
            const uint32_t* rs = st.rs;
            vlog("gl: target %ux%u drawn to %s %ux%u: prog %d rhw %d z %u/%u blend %u %u,%u atest %u/%u ref %u fog %u "
                 "stencil %u vp %u,%u %ux%u scissor %u, %u prims\n", t->w, t->h, to_backbuffer() ? "bb" : "rt", g_bw, g_bh,
                 p ? p->id : -1, rhw, rs[7], rs[14], rs[27], rs[19], rs[20], rs[15], rs[25], rs[24] & 255, rs[28], rs[52],
                 st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3], rs[174], d->n);
        }
    }
    if (g_report) {
        const uint32_t* rs = st.rs;
        report("draw prim %u n %u %s to %s %x %ux%u ds %x prog %d vs %08x ps %08x fvf %x rhw %d z %u/%u/%u cull %u blend %u %u,%u op %u"
               " atest %u/%u ref %u stencil %u cw %x vp %u,%u %ux%u scissor %u %d,%d,%d,%d\n ",
               d->prim, d->n, d->client ? "client" : "gpu", to_backbuffer() ? "bb" : "rt", st.rt[0], g_bw, g_bh, st.ds, p ? p->id : -1, st.vs,
               st.ps, st.fvf, rhw, rs[7], rs[14], rs[23], rs[22], rs[27], rs[19], rs[20], rs[171], rs[15], rs[25], rs[24] & 255,
               rs[52], rs[168], st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3], rs[174], (int)st.scissor[0],
               (int)st.scissor[1], (int)st.scissor[2], (int)st.scissor[3]);
        for (int i = 0; i < 4; i++) {
            Res* r = st.tex[i] ? res(st.tex[i]) : NULL;
            if (r) report(" t%d %x %s%ux%u fmt %u lv %u up %u usage %x", i, st.tex[i], r->cube ? "cube " : "", r->w, r->h, r->fmt, r->levels, r->uploaded, r->usage);
        }
        if (!st.ps)
            for (int i = 0; i < k.fp.n; i++) {
                const FfpStage* s = &k.fp.s[i];
                report(" | s%d col %u(%u,%u) al %u(%u,%u)", i, s->cop, s->ca1, s->ca2, s->aop, s->aa1, s->aa2);
            }
        report("\n  decl");  /* stream:offset type usage.index */
        for (int i = 0; i < ne; i++) report(" %u:%u t%u u%u.%u", e[i].stream, e[i].off, e[i].type, e[i].usage, e[i].index);
        for (int i = 0; i < ne; i++)  /* the first vertices' positions (float3/float4 position elements) */
            if ((e[i].usage == 0 || e[i].usage == 9) && (e[i].type == 2 || e[i].type == 3)) {
                uint32_t sn = e[i].stream & 15, str;
                const uint8_t* q = NULL;
                if (d->client) q = d->data[sn] ? (const uint8_t*)d + d->data[sn] : NULL, str = d->stride[sn];
                else {
                    Res* vb = st.vb[sn] ? res(st.vb[sn]) : NULL;
                    str = st.vbstride[sn];
                    if (vb && vb->bits[0]) q = (const uint8_t*)vb->bits[0] + st.vboff[sn] + d->base * str;
                }
                if (q)
                    for (int v = 0; v < 4; v++) {
                        const float* f = (const float*)(q + v * str + e[i].off);
                        report(" | %.3f %.3f %.3f", f[0], f[1], f[2]);
                    }
                int row = st.vs ? cg_wrow(obj_copy(st.vs, NULL)) : -1;  /* the view depth fog reads, first vertex */
                if (q && row >= 0 && row < 256) {
                    const float *f = (const float*)(q + e[i].off), *w = (const float*)st.vsc[row];
                    report(" | depth %.1f zwrite %u fog %d", w[0] * f[0] + w[1] * f[1] + w[2] * f[2] + w[3], st.rs[14],
                           !rhw && !d->nofog && !st.rs[52] && main_scene(&st));
                }
                break;
            }
        report("\n");
    }
    if (!p) return;
#ifdef USM_DEBUG
    static int skip = -1;
    if (skip < 0) skip = getenv("VPC_SKIP") ? atoi(getenv("VPC_SKIP")) : 0;
    if (p->id == skip) return;  /* debug: leave one program's draws out */
#endif
    glUseProgram(p->prog);
    float v4[4];
    if (p->vc >= 0) glUniform4fv(p->vc, 96, (const float*)st.vsc);
    for (int i = 0; i < p->nvc; i++) glUniform4fv(p->vcl[i], 1, (const float*)st.vsc[p->vcr[i]]);
    if (p->pc >= 0) glUniform4fv(p->pc, 8, (const float*)st.psc);
    if (p->fix >= 0) {
        float vw = rhw ? (float)g_bw : (float)st.viewport[2], vh = rhw ? (float)g_bh : (float)st.viewport[3];
        v4[0] = 1.f / vw, v4[1] = -1.f / vh, v4[2] = 2.f / g_bw, v4[3] = 2.f / g_bh;
        glUniform4fv(p->fix, 1, v4);
    }
    if (p->m >= 0) {
        float wv[16], wvp[16];
        mat4(st.transform[256], st.transform[2], wv);
        mat4(wv, st.transform[3], wvp);
        glUniform4fv(p->m, 4, wvp);
    }
    if (p->flip >= 0) v4[0] = g_flip, v4[1] = v4[2] = v4[3] = 0, glUniform4fv(p->flip, 1, v4);
    if (p->mat >= 0) glUniform4fv(p->mat, 1, (const float*)st.material);
    if (p->tf >= 0) rgba(st.rs[60], v4), glUniform4fv(p->tf, 1, v4);
    if (p->aref >= 0) v4[0] = (float)(st.rs[24] & 255), v4[1] = v4[2] = v4[3] = 0, glUniform4fv(p->aref, 1, v4);
    if (p->fog >= 0) {  /* the 3D world view only; to the game's own fog color (D3DRS_FOGCOLOR, the sky's light blue in
                           free roam; a fixed blue-grey looked like grey slabs), black for an additive pass */
        float far = far_dist();
        v4[0] = far * 0.55f, v4[1] = far > 0 ? 1.f / (far * 0.45f) : 0, v4[2] = !rhw && far > 0 && !d->nofog && !st.rs[52] && main_scene(&st), v4[3] = 0;
        glUniform4fv(p->fog, 1, v4);
        int add = st.rs[27] && st.rs[20] == 2;
        if (st.rs[34]) rgba(st.rs[34], v4);
        else v4[0] = 0.42f, v4[1] = 0.47f, v4[2] = 0.54f;
        if (add) v4[0] = v4[1] = v4[2] = 0;
        glUniform4fv(p->fogc, 1, v4);
    }
    bind_textures();
    if (d->icon) glBindTexture(GL_TEXTURE_2D, g_glyphs);  /* unit 0 is active after bind_textures */
    apply_state(rhw);
    if (k.flags >> 3 == 1) glDisable(GL_DEPTH_TEST), glDisable(GL_BLEND), glDisable(GL_STENCIL_TEST), glColorMask(1, 1, 1, 1);
    else if (k.flags >> 3) glDisable(GL_BLEND), glColorMask(1, 1, 1, 1);
    static uint32_t timed;  /* the report's vitaGL time: every 8th draw is timed (a clock read costs) */
    uint64_t t = ++timed & 7 ? 0 : sceKernelGetProcessTimeWide();
    if (d->idx >= 0) {
        const void* ip = (const void*)(uintptr_t)d->idx;
        if (d->client) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0), ip = (const uint8_t*)d + d->idx;
        else glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, res(st.ib)->gl);
        glDrawElements(mode, count, d->idx32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, ip);
    } else
        glDrawArrays(mode, 0, count);
    if (t) g_glcall_us += 8 * (sceKernelGetProcessTimeWide() - t);
}

static void free_(Res* r) {
    if (r->fbo == g_fb && g_fb) glBindFramebuffer(GL_FRAMEBUFFER, g_fb = 0);  /* GL reuses the name: g_fb must not */
    if (r->fbo) glDeleteFramebuffers(1, &r->fbo);
    if (r->rb) glDeleteRenderbuffers(1, &r->rb);  /* comic cutscenes make a new target nearly every frame */
    if (r->gl) r->size ? glDeleteBuffers(1, &r->gl) : glDeleteTextures(1, &r->gl);
    r->fbo = r->rb = r->gl = r->uploaded = 0;
}

/* ---- the game thread: recording ----
   Commands go into one of two buffers; Present hands the full one to the render thread and the game goes on filling
   the other, so the game builds frame N while core 1 draws frame N-1. Anything the game may change right after the
   call (dynamic vertices, indices, rects, texture data) is copied into the command; resources are freed in order
   (C_FREE), so the render thread never sees freed memory. */
enum { C_INIT, C_PATCH, C_CLEAR, C_UPLOAD, C_BUFFER, C_DRAW, C_FREE, C_PRESENT };
typedef struct {
    uint32_t type, size;  /* size: of the payload that follows, 8-aligned */
} CmdHead;
typedef struct {
    uint8_t* p;
    uint32_t n, cap;
} Arena;
static Arena g_ar[2];
static int g_cur;  /* the buffer the game thread fills */
static Arena* g_job;  /* handed to the render thread */
/* the handoff, on kernel semaphores (a shared pthread condition variable deadlocked here): done = 1 while the render
   thread is idle, job wakes it */
static SceUID g_sem_done, g_sem_job, g_render;
static D3DState g_sent;  /* what the render thread's copy holds, as of the last command recorded */
static uint32_t g_rl_frame;  /* frames presented (early cull, below) */
uint32_t g_cmd_by[8];  /* command bytes recorded per type since the last report (d3d9_null.c) */
static uint32_t g_far_n, g_drawn_n, g_deep_n;  /* draws left out / recorded / left out with 25% to spare, ever */
#define FLUSH_AT (6u << 20)  /* hand a buffer over early past this, so long loads don't pile up uploads */

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

/* for the stall watchdog: what each thread is doing */
static volatile int g_rcmd = -1;                  /* command the render thread is running, -1 idle */
static volatile uint32_t g_jobs, g_presents, g_in_flush;

static void run(Arena* a) {
    for (uint32_t o = 0; o < a->n;) {
        CmdHead* h = (CmdHead*)(a->p + o);
        uint8_t* q = (uint8_t*)(h + 1);
        o += sizeof *h + h->size;
        g_rcmd = (int)h->type;
        switch (h->type) {
        case C_INIT: init_(((uint32_t*)q)[0], ((uint32_t*)q)[1]); break;
        case C_PATCH: memcpy((uint8_t*)&g_rst + ((uint32_t*)q)[0], q + 8, ((uint32_t*)q)[1]); break;
        case C_CLEAR: {
            uint32_t* c = (uint32_t*)q;
            float z;
            memcpy(&z, &c[3], 4);
            clear_(c[0], c[5] ? (const int32_t*)(c + 6) : NULL, c[1], c[2], z, c[4]);
            break;
        }
        case C_UPLOAD: upload_(*(Res**)q, q + 8); break;
        case C_BUFFER: buffer_(*(Res**)q, ((uint32_t*)q)[1], q + 8); break;
        case C_DRAW: draw_((const DrawCmd*)q); break;
        case C_FREE: free_(*(Res**)q); break;
        case C_PRESENT: present_(); break;
        }
    }
    g_rcmd = -1;
}
/* logs when 4 s pass without a Present: whether the game thread waits on the render thread or the other way round */
static void* watchdog(void* arg) {
    (void)arg;
    uint32_t last = 0, quiet = 0;
    for (;;) {
        sceKernelDelayThread(1000000);
        if (g_presents != last) { last = g_presents, quiet = 0; continue; }
        if (++quiet == 4 || quiet % 10 == 0)
            vlog("gl: no Present for %u s: %u frames, %u buffers handed over, game thread %s, render thread on command %d\n",
                 quiet, g_presents, g_jobs, g_in_flush ? "waiting for it" : "running", g_rcmd);
    }
    return NULL;
}
static int render_main(SceSize args, void* argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(g_sem_job, 1, NULL);
        uint64_t t = now_us();
        run(g_job);
        g_render_us += now_us() - t;
        sceKernelSignalSema(g_sem_done, 1);
    }
    return 0;
}
/* hand the filled buffer over once the render thread is done with the other one */
static void flush(void) {
    uint64_t t = now_us();
    g_in_flush = 1;
    sceKernelWaitSema(g_sem_done, 1, NULL);
    g_in_flush = 0, g_jobs++;
    g_job = &g_ar[g_cur];
    sceKernelSignalSema(g_sem_job, 1);
    g_cur ^= 1, g_ar[g_cur].n = 0;
    Arena* a = &g_ar[g_cur];
    if (a->cap > (8u << 20)) g_cmd_bytes -= a->cap, free(a->p), a->p = NULL, a->cap = 0;  /* one big upload: give it back */
    g_swap_us += now_us() - t;
}
static SceUID g_recorder;  /* the thread that called gl_init: the buffers have no lock, a second writer corrupts them */
/* the render pipeline (native.c) hands the D3D calls between threads, one at a time */
void gl_set_recorder(void) { g_recorder = sceKernelGetThreadId(); }
static void* push(uint32_t type, uint32_t size) {
    static int warned;
    /* checked on a buffer's first command only: a syscall on every one cost the worker per draw */
    if (!g_ar[g_cur].n && sceKernelGetThreadId() != g_recorder && !warned++) vlog("gl: command %u recorded from thread %x, not %x\n", type, sceKernelGetThreadId(), g_recorder);
    size = (size + 7) & ~7u;
    Arena* a = &g_ar[g_cur];
    if (a->n && a->n + size > FLUSH_AT) flush(), a = &g_ar[g_cur];
    uint32_t need = a->n + (uint32_t)sizeof(CmdHead) + size;
    if (need > a->cap) {
        uint32_t cap = a->cap ? a->cap : 1u << 20;
        while (cap < need) cap *= 2;
        uint8_t* p = realloc(a->p, cap);
        if (!p) { vlog("gl: out of memory for commands (%u bytes)\n", cap); abort(); }
        g_cmd_bytes += cap - a->cap, a->p = p, a->cap = cap;
    }
    CmdHead* h = (CmdHead*)(a->p + a->n);
    h->type = type, h->size = size;
    g_cmd_by[type & 7] += size + sizeof *h;
    a->n = need;
    return h + 1;
}

/* send the parts of the game's state the renderer reads that changed: only the 64-byte chunks the D3D setters marked
   (g_st_dirty) are compared, and a run of changed chunks becomes one patch */
static void send_patch(uint32_t a, uint32_t b) {
    const uint8_t* m = (const uint8_t*)g_main;
    uint32_t* q = push(C_PATCH, 8 + b - a);
    q[0] = a, q[1] = b - a;
    memcpy(q + 2, m + a, b - a);
    memcpy((uint8_t*)&g_sent + a, m + a, b - a);
}
static void patch_state(void) {
    static const struct { uint32_t off, len; } R[] = {
        {offsetof(D3DState, rs), sizeof g_sent.rs},
        {offsetof(D3DState, tss), 4 * sizeof g_sent.tss[0]},
        {offsetof(D3DState, ss), 4 * sizeof g_sent.ss[0]},
        {offsetof(D3DState, tex), offsetof(D3DState, material) + sizeof g_sent.material - offsetof(D3DState, tex)},
        {offsetof(D3DState, vsc), 96 * sizeof g_sent.vsc[0]},
        {offsetof(D3DState, psc), 8 * sizeof g_sent.psc[0]},
        {offsetof(D3DState, transform[2]), 2 * sizeof g_sent.transform[0]},
        {offsetof(D3DState, transform[256]), sizeof g_sent.transform[0]},
    };
    const uint8_t *m = (const uint8_t*)g_main, *s0 = (const uint8_t*)&g_sent;
    uint32_t ra = 0, rb = 0;  /* the open run [ra, rb), empty when equal */
    for (unsigned i = 0; i < sizeof R / sizeof *R; i++)
        for (uint32_t c = R[i].off / 64, end = R[i].off + R[i].len; c * 64 < end; c++) {
            if (!(g_st_dirty[c >> 5] >> (c & 31) & 1)) continue;
            uint32_t a = c * 64 > R[i].off ? c * 64 : R[i].off, b = c * 64 + 64 < end ? c * 64 + 64 : end;
            if (!memcmp(m + a, s0 + a, b - a)) continue;
            if (ra < rb && a == rb) rb = b;
            else {
                if (ra < rb) send_patch(ra, rb);
                ra = a, rb = b;
            }
        }
    if (ra < rb) send_patch(ra, rb);
    memset(g_st_dirty, 0, sizeof g_st_dirty);
#ifdef USM_VPC
    /* self-check: a write the setters did not mark leaves g_sent behind; name it once, and send it anyway */
    for (unsigned i = 0; i < sizeof R / sizeof *R; i++)
        for (uint32_t o = R[i].off, end = o + R[i].len; o < end; o += 4)
            if (memcmp(m + o, s0 + o, 4)) {
                static int told;
                if (!told++) vlog("gl: state at offset %u changed without being marked dirty\n", o);
                send_patch(o, o + 4);
            }
#endif
}

void gl_init(uint32_t bw, uint32_t bh) {
    if (!g_render) {
        g_recorder = sceKernelGetThreadId();
        g_sem_done = sceKernelCreateSema("usm_gl_done", 0, 1, 1, NULL);
        g_sem_job = sceKernelCreateSema("usm_gl_job", 0, 0, 1, NULL);
        /* core 1, the game's own priority; vitaGL's shader compiler runs on it, so the game threads' 8 MB stack */
        g_render = sceKernelCreateThread("usm_render", render_main, 0x10000100, 8 << 20, 0, 0x20000, NULL);
        if (g_render < 0 || g_sem_done < 0 || g_sem_job < 0) vlog("gl: render thread not created (%x %x %x)\n", g_render, g_sem_done, g_sem_job);
        sceKernelStartThread(g_render, 0, NULL);
        pthread_t w;
        if (log_on()) pthread_create(&w, NULL, watchdog, NULL), pthread_detach(w);
    }
    uint32_t* q = push(C_INIT, 8);
    q[0] = bw, q[1] = bh;
}
void gl_present(void) {
    g_rl_frame++;
    int was = g_world_fog;
    g_world_fog = g_main->rs[28] && g_world_draws >= 24;  /* the map (game fog off) and panel cutscenes get none */
    if (g_world_fog && !was) g_world_back++;  /* the early cull forgets what it skipped before (gl_rl_before) */
    g_world_draws = 0;
    push(C_PRESENT, 0);
    flush();
    g_presents++;
}
void gl_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    patch_state();
    if (count > 64) count = 64;
    uint32_t nr = rects ? count : 0;
    uint32_t* q = push(C_CLEAR, 24 + 16 * nr);
    q[0] = count, q[1] = flags, q[2] = color, q[4] = stencil, q[5] = nr;
    memcpy(&q[3], &z, 4);
    if (nr) memcpy(q + 6, H(rects), 16 * nr);
}
/* Texture size cap (PS2-like; ux0:data/usm/texture_max.txt, default 256, 0 = full size): a texture the game gives
   smaller levels for (world textures; not single-level UI art, fonts, render targets or dynamic ones) goes up from
   the first level that fits, and that level becomes GL level 0. 4-16x less texture memory and GPU bandwidth for big
   ones. DXT textures also go through upload_dxt_small. */
static void texmax_load(void) {
    if (g_texmax >= 0) return;
    g_texmax = 256;
    FILE* f = fopen(USM_DIR "texture_max.txt", "r");
    if (f) {
        if (fscanf(f, "%d", &g_texmax) != 1) g_texmax = 256;
        fclose(f);
    }
    vlog("gl: textures capped at %d, DXT ones at half that (texture_max.txt; 0 = full size)\n", g_texmax);
}
static uint32_t base_of(const Res* r) {
    texmax_load();
    if (!g_texmax || r->cube || r->usage & 0x601 || r->levels <= 1) return 0;
    uint32_t b = 0;
    while (b + 1 < r->levels && b < 15 && ((r->w >> b) > (uint32_t)g_texmax || (r->h >> b) > (uint32_t)g_texmax)) b++;
    return b;
}
static void upload_level(Res* r, uint32_t b) {
    uint32_t w = r->w >> b, h = r->h >> b;
    uint64_t t = now_us();
    uint32_t n = pitch_of(r->fmt, w ? w : 1) * rows_of(r->fmt, h ? h : 1);
    r->base = b;
    Res** q = push(C_UPLOAD, 8 + n);
    q[0] = r;
    memcpy((uint8_t*)q + 8, r->bits[b], n);
    r->sent = 1;
    g_uploads++, g_upload_us += now_us() - t;
}
void gl_upload(Res* r, uint32_t level) {
    if (r->cube || level >= 16 || !r->bits[level] || level != base_of(r)) return;
    upload_level(r, level);
}
void gl_free(Res* r) { *(Res**)push(C_FREE, 8) = r; }
uint32_t gl_mem_free_mb(int ram) { return g_render ? (uint32_t)(vglMemFree(ram ? VGL_MEM_RAM : VGL_MEM_VRAM) >> 20) : 0; }

static void send_buffer(Res* b, GLenum target) {
    if (b->sent) return;
    Res** q = push(C_BUFFER, 8 + b->size);
    q[0] = b, ((uint32_t*)q)[1] = target;
    memcpy((uint8_t*)q + 8, b->bits[0], b->size);
    b->sent = 1, g_buf_uploads++, g_buf_bytes += b->size;
}
static int is_static(Res* b) { return b && b->bits[0] && !(b->usage & 0x200); }

/* ---- draw distance: an indexed draw from static buffers (the city, props) whose bounding sphere lies wholly beyond
   far_dist() is not recorded. Depth is the shader's own clip w row (cg_wrow); spheres are cached per mesh range ---- */
extern uint32_t g_culled;  /* d3d9_null.c: the report */
static int vs_wrow(uint32_t vs) {
    static uint32_t key[512];
    static int16_t row[512];
    for (uint32_t i = (vs >> 4) & 511, n = 0; n < 512; i = (i + 1) & 511, n++) {
        if (key[i] == vs) return row[i];
        if (!key[i]) return key[i] = vs, row[i] = (int16_t)cg_wrow(obj_copy(vs, NULL));
    }
    return -1;
}
typedef struct { const void* vb; uint32_t at, idx, count, gen; float c[3], r; } Sphere;  /* gen: vb + ib content versions */
static Sphere g_sph[8192];
/* 1: wholly beyond the draw distance; 2: the camera is inside it and it reaches past the distance (the sky dome: no fog) */
static int too_far(const D3DState* m, const Elem* e, int ne, Res* ib, const void* idx, int idx32, int count, int32_t base) {
    float far = far_dist();
    /* draws without depth writes are never left out: the sky's pieces lie past any distance, and culling them left
       holes showing the dark clear color (the grey slabs over the skyline) */
    if (far <= 0 || !m->vs || !idx || !ib || !main_scene(m) || !m->rs[14]) return 0;
    int row = vs_wrow(m->vs), pe = -1;
    if (row < 0 || row > 252) return 0;
    for (int i = 0; i < ne; i++)
        if (e[i].usage == 0 && e[i].index == 0 && e[i].type == 2) pe = i;  /* float3 position */
    if (pe < 0) return 0;
    uint32_t s = e[pe].stream & 15, str = m->vbstride[s];
    Res* vb = m->vb[s] ? res(m->vb[s]) : NULL;
    if (!vb || !vb->bits[0] || !str || !is_static(vb)) return 0;  /* dynamic ones change every frame */
    uint32_t at = m->vboff[s] + (uint32_t)base * str + e[pe].off, io = (uint32_t)((const uint8_t*)idx - (uint8_t*)ib->bits[0]);
    uint32_t gen = vb->gen * 31u + ib->gen;  /* a reloaded city chunk reuses buffers: a stale sphere hid buildings */
    uint32_t h = ((uintptr_t)vb >> 4) * 2654435761u ^ at * 40503u ^ io * 9973u ^ (uint32_t)count;
    /* 4-way sets: direct-mapped, two meshes on one slot pushed each other out every frame and walked all their indices */
    Sphere *set = &g_sph[(h & 2047) * 4], *sp = NULL;
    for (int w = 0; w < 4 && !sp; w++)
        if (set[w].vb == vb && set[w].at == at && set[w].idx == io && set[w].count == (uint32_t)count && set[w].gen == gen) sp = &set[w];
    if (!sp) {
        static uint32_t rr;
        sp = &set[rr++ & 3];
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (int i = 0; i < count; i++) {
            uint32_t v = idx32 ? ((const uint32_t*)idx)[i] : ((const uint16_t*)idx)[i];
            uint64_t o = (uint64_t)at + (uint64_t)v * str;
            if (o + 12 > vb->size) return 0;  /* indices past the buffer: leave it to the GPU */
            const float* q = (const float*)((const uint8_t*)vb->bits[0] + o);
            for (int k = 0; k < 3; k++) lo[k] = q[k] < lo[k] ? q[k] : lo[k], hi[k] = q[k] > hi[k] ? q[k] : hi[k];
        }
        sp->vb = vb, sp->at = at, sp->idx = io, sp->count = (uint32_t)count, sp->gen = gen;
        for (int k = 0; k < 3; k++) sp->c[k] = (lo[k] + hi[k]) * 0.5f;
        sp->r = sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])) * 0.5f;
    }
    float w[4];
    memcpy(w, m->vsc[row], 16);
    float d = w[0] * sp->c[0] + w[1] * sp->c[1] + w[2] * sp->c[2] + w[3], r = sp->r * sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (d - r > far * 1.25f) g_deep_n++;  /* early cull: far enough that a few frames' late recheck can't show */
    return d - r > far ? 1 : d - r < 0 && r > far ? 2 : 0;
}

/* ---- early cull: the render lists (77e190 / 77e220) call one draw callback per entry, ecx = the entry. An entry whose
   draws were all beyond the draw distance the last time is skipped whole for the next 3 frames (its game code, D3D
   state calls and draws), then runs again to check: the same decision as too_far, made before the work. Keyed by the
   callback and the entry's words 3..5 (for 4169e0: instance, mesh, material), not by the entry, which lives in the
   per-frame arena. Main scene only. Off: ux0:data/usm/early_cull.txt containing 0 ---- */
extern uint32_t g_early;  /* d3d9_null.c: the report */
typedef struct { uint32_t fn, w[3], frame; } Skip;
static Skip g_skip[4096];
static int early_on(void) {
    static int on = -1;
    if (on < 0) {
        on = 1;
        FILE* f = fopen(USM_DIR "early_cull.txt", "r");
        if (f) {
            if (fscanf(f, "%d", &on) != 1) on = 1;
            fclose(f);
        }
        if (getenv("VPC_EARLY")) on = atoi(getenv("VPC_EARLY"));  /* vpc tests */
        vlog("gl: early cull %s (early_cull.txt)\n", on ? "on" : "off");
    }
    return on;
}
static Skip* skip_slot(uint32_t entry, uint32_t fn, uint32_t* w) {
    for (int i = 0; i < 3; i++) w[i] = LD32(entry + 12 + 4 * i);
    return &g_skip[((fn >> 4) * 2654435761u ^ w[0] * 40503u ^ w[1] * 9973u ^ w[2] * 31u) >> 20];
}
/* rt_vita.c rc_call, around an entry's callback: 1 = skip it; else *mark is handed to gl_rl_after (0: not watched) */
int gl_rl_before(uint32_t entry, uint32_t fn, uint32_t* mark) {
    *mark = 0;
    if (!early_on() || far_dist() <= 0 || !main_scene(g_main)) return 0;
    /* a mission's end reloads the city: entries keyed by stale instance/mesh words would hide new buildings */
    extern uint32_t g_static_writes;  /* d3d9_null.c: static buffers rewritten = something (re)loaded */
    static uint32_t back, writes;
    if (back != g_world_back || writes != g_static_writes)
        back = g_world_back, writes = g_static_writes, memset(g_skip, 0, sizeof g_skip);
    uint32_t w[3];
    Skip* k = skip_slot(entry, fn, w);
    int hit = k->fn == fn && !memcmp(k->w, w, 12) && g_rl_frame - k->frame < 4;
    if (hit && early_on() != 2) return g_early++, 1;
    *mark = 1 + hit;  /* 2: check mode (early_cull.txt = 2) runs it anyway, to count skips that would have drawn */
    mark[1] = g_far_n, mark[2] = g_drawn_n, mark[3] = g_deep_n;
    return 0;
}
void gl_rl_after(uint32_t entry, uint32_t fn, const uint32_t* mark) {
    if (!mark[0]) return;
    if (mark[0] == 2) {
        static uint32_t n, bad;
        bad += g_drawn_n != mark[2], n++;
        if (!(n & 1023)) vlog("gl: early cull check: %u of %u skips would have drawn\n", bad, n);
    }
    uint32_t w[3];
    Skip* k = skip_slot(entry, fn, w);
    if (g_far_n != mark[1] && g_drawn_n == mark[2] && g_deep_n - mark[3] == g_far_n - mark[1]) k->fn = fn, memcpy(k->w, w, 12), k->frame = g_rl_frame;
    else if (k->fn == fn && !memcmp(k->w, w, 12)) k->fn = 0;
}

/* Button icons in text: native.c f_7791a0 points 6 font glyphs at texture coordinates u 1000+k..1001+k (k = icon slot,
   outside any real glyph). The text renderer draws each glyph as its own quad, so a client draw whose first texcoord
   is there gets the icon atlas instead: u to the slot's place in the atlas, v back to 0..1, the vertex colour white
   (keeping alpha), so the icon keeps its own colours whatever the text colour. */
static int icon_quad(const Elem* e, int ne, uint8_t* q, const DrawCmd* d, uint32_t nv) {
    const Elem *tc = NULL, *col = NULL;
    for (int i = 0; i < ne; i++) {
        if (e[i].usage == 5 && e[i].index == 0 && e[i].type == 1) tc = &e[i];
        if (e[i].usage == 10 && e[i].index == 0 && e[i].type == 4) col = &e[i];
    }
    if (!tc || !d->data[tc->stream] || nv > 6) return 0;
#define TC(v) ((float*)(q + d->data[tc->stream] + (v) * d->stride[tc->stream] + tc->off))
    float umin = 1e9f;  /* every corner at u 1000+k or 1001+k, v -1000 or -999 (repeating textures, e.g. the minimap's,
                           reach u 16 and more, so the old marker there caught them) */
    for (uint32_t v = 0; v < nv; v++) {
        float u = TC(v)[0], w = TC(v)[1];
        if (!(u > 999.5f && u < 1009.5f && w > -1000.5f && w < -998.5f)) return 0;
        if (u < umin) umin = u;
    }
    int slot = (int)(umin + 0.5f) - 1000;
    if (slot < 0 || slot > 7) return 0;
    for (uint32_t v = 0; v < nv; v++) {
        float* t = TC(v);
        t[0] = (slot + (t[0] - umin > 0.5f)) * (1.f / 8), t[1] = t[1] + 1000 > 0.5f;
        if (col && d->data[col->stream]) {
            uint8_t* c = q + d->data[col->stream] + v * d->stride[col->stream] + col->off;
            c[0] = c[1] = c[2] = 0xff;
        }
    }
#undef TC
    return 1;
}

void gl_draw(uint32_t prim, uint32_t n, const uint8_t* up, uint32_t up_stride, const void* idx, int idx32, int32_t base,
             uint32_t minv, uint32_t numv) {
    GLenum mode;
    int count = count_of(prim, n, &mode);
    if (!count) return;
    /* "record" in the report: every 8th draw is timed and counts 8 times (two clock syscalls per draw cost the worker) */
    static uint32_t timed;
    uint64_t t = ++timed & 7 ? 0 : now_us();
    patch_state();
    /* 3D comic panels draw their world in the full view too, clipped by stencil masks or a panel scissor: not world */
    const int32_t* sc = (const int32_t*)g_main->scissor;
    if (g_main->rs[28] && g_main->rs[14] && g_main->vs && !g_main->rs[52] && main_view(g_main) &&
        (!g_main->rs[174] || (sc[0] <= 0 && sc[1] <= 0 && sc[2] >= (int32_t)g_main->viewport[2] && sc[3] >= (int32_t)g_main->viewport[3])))
        g_world_draws++;
    Elem e[32];
    int ne = elems_of(g_main, e);
    uint32_t used = 0;
    for (int i = 0; i < ne; i++) used |= 1u << (e[i].stream & 15);
    const D3DState* m = g_main;
    Res* ib = idx && !up && m->ib ? res(m->ib) : NULL;
    uint32_t isz = idx32 ? 4 : 2;
    int gpu = !up && (!idx || (is_static(ib) && (const uint8_t*)idx >= (uint8_t*)ib->bits[0] &&
                                (const uint8_t*)idx + count * isz <= (uint8_t*)ib->bits[0] + ib->size));
    for (uint32_t s = 0; s < 16; s++)
        if (used >> s & 1 && !is_static(m->vb[s] ? res(m->vb[s]) : NULL)) gpu = 0;
    int far = gpu ? too_far(m, e, ne, ib, idx, idx32, count, base) : 0;
    if (far == 1) {
        g_culled++, g_far_n++;
        if (t) g_draw_us += 8 * (now_us() - t);
        return;
    }
    if (gpu) {
        for (uint32_t s = 0; s < 16; s++)
            if (used >> s & 1) send_buffer(res(m->vb[s]), GL_ARRAY_BUFFER);
        if (ib) send_buffer(ib, GL_ELEMENT_ARRAY_BUFFER);
        DrawCmd* d = push(C_DRAW, sizeof *d);
        memset(d, 0, sizeof *d);
        d->prim = prim, d->n = n, d->base = (uint32_t)base, d->idx32 = (uint8_t)idx32;
        /* no fog for the sky only: a mesh around the camera reaching past the distance, drawn without depth writes. The
           far city skyline (same shape, depth writes on) and far windows/decals (no depth writes) are fogged: left
           out, they stood out as grey/brown slabs */
        d->nofog = far == 2 && !m->rs[14];
        d->idx = idx ? (int32_t)((const uint8_t*)idx - (uint8_t*)ib->bits[0]) : -1;
    } else {
        /* the vertices used: an indexed draw's from its indices (MinIndex/NumVertices are hints drivers ignore, and
           games get them wrong), others run from the first one */
        uint32_t lo = 0, nv = (uint32_t)count;
        if (idx) {
            uint32_t hi = 0;
            lo = ~0u;
            for (int i = 0; i < count; i++) {
                uint32_t v = idx32 ? ((const uint32_t*)idx)[i] : ((const uint16_t*)idx)[i];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            nv = hi - lo + 1;
        }
        (void)minv, (void)numv;
        uint32_t first = (uint32_t)base + lo;
        if (nv > 65536 * 4) nv = 65536 * 4;
        uint32_t size = sizeof(DrawCmd), off[16] = {0}, str[16] = {0};
        const uint8_t* src[16] = {0};
        uint32_t avail[16] = {0};
        for (uint32_t s = 0; s < 16; s++) {
            if (!(used >> s & 1)) continue;
            if (up && s == 0) src[s] = up, str[s] = up_stride, avail[s] = ~0u;
            else {
                Res* vb = m->vb[s] ? res(m->vb[s]) : NULL;
                if (!vb || !vb->bits[0]) continue;
                str[s] = m->vbstride[s], src[s] = (const uint8_t*)vb->bits[0] + m->vboff[s];
                avail[s] = vb->size > m->vboff[s] ? vb->size - m->vboff[s] : 0;
            }
            uint64_t from = (uint64_t)first * str[s];  /* avail: what the buffer has from there, the rest reads as 0 */
            uint64_t len = (uint64_t)nv * str[s], ok = from >= avail[s] ? 0 : avail[s] - from < len ? avail[s] - from : len;
            avail[s] = (uint32_t)ok;
            off[s] = size, size += ((uint32_t)len + 7) & ~7u;
        }
        uint32_t ioff = size;
        if (idx) size += count * isz;
        DrawCmd* d = push(C_DRAW, size);
        memset(d, 0, sizeof *d);
        d->prim = prim, d->n = n, d->client = 1, d->idx32 = (uint8_t)idx32, d->idx = idx ? (int32_t)ioff : -1;
        uint8_t* q = (uint8_t*)d;
        for (uint32_t s = 0; s < 16; s++) {
            if (!src[s]) continue;
            memcpy(q + off[s], src[s] + (uint64_t)first * str[s], avail[s]);
            memset(q + off[s] + avail[s], 0, nv * str[s] - avail[s]);
            d->data[s] = off[s], d->stride[s] = str[s];
            g_client_bytes += avail[s];
        }
        if (idx) {  /* rebased to the first vertex copied */
            if (idx32)
                for (int i = 0; i < count; i++) ((uint32_t*)(q + ioff))[i] = ((const uint32_t*)idx)[i] - lo;
            else
                for (int i = 0; i < count; i++) ((uint16_t*)(q + ioff))[i] = (uint16_t)(((const uint16_t*)idx)[i] - lo);
        }
        d->icon = icon_quad(e, ne, q, d, nv);
        g_client_draws++;
    }
    /* a managed texture drawn with is on the GPU: our copy only doubles its memory */
    for (int i = 0; i < 4; i++) {
        Res* r = m->tex[i] ? res(m->tex[i]) : NULL;
        if (r && !r->sent && !r->cube && r->bits[0] && !(r->usage & 1))  /* its capped level never came: full size */
            upload_level(r, 0);
        if (r && r->sent && !r->dropped && !r->cube && r->pool == 1 && !(r->usage & 0x201)) res_drop_bits(r);
    }
    g_draws++, g_drawn_n++;
    if (t) g_draw_us += 8 * (now_us() - t);
}
