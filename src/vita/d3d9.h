/* Direct3D 9 state shared by d3d9_null.c (objects, state store) and gl.c (the vitaGL renderer, USM_VITAGL builds) */
#pragma once
#include "com.h"

#define W 640
#define HGT 480
#define FMT_X8R8G8B8 22
#define FMT_D24S8 75

/* host side of a resource: guest object word 2 points here */
typedef struct {
    uint32_t w, h, levels, fmt, usage, pool, size;  /* size: buffers */
    void* bits[16];                                 /* per level (or the buffer), allocated on first lock */
    uint32_t surf[16];                              /* surface objects of the levels */
    uint32_t parent, plevel;                        /* a texture level's surface: its texture object and level */
    uint32_t gl, uploaded, cube;                    /* gl.c: texture name, mask of levels sent; cube texture */
    uint32_t fbo, rb;                               /* gl.c: framebuffer + its depth renderbuffer for a render target */
    uint32_t dropped;                               /* the level copies were freed once on the GPU */
    uint32_t sent;                                  /* gl.c, game thread: level 0 / the buffer is in a command */
    uint32_t gen;                                   /* a buffer's content version: new on every write Lock */
    uint32_t base;                                  /* gl.c: the level uploaded as GL level 0 (texture size cap) */
} Res;
static inline Res* res(uint32_t obj) { return H(LD32(obj + 8)); }

#define FMT_DXT1 0x31545844u
static inline int is_dxt(uint32_t f) { return f >= FMT_DXT1 && f <= 0x35545844u && (f & 0xffffff) == 0x545844; }
static inline uint32_t pitch_of(uint32_t fmt, uint32_t w) {  /* bytes per row (DXT: per row of 4x4 blocks) */
    if (is_dxt(fmt)) return ((w + 3) / 4 ? (w + 3) / 4 : 1) * (fmt == FMT_DXT1 ? 8 : 16);
    switch (fmt) {
    case 23: case 24: case 25: case 26: case 29: case 30: case 51: case 60: case 77: return w * 2;  /* 16-bit formats */
    case 28: case 50: return w;                                                                      /* A8, L8 */
    case 36: case 113: return w * 8;                                                                 /* 64-bit */
    default: return w * 4;
    }
}
static inline uint32_t rows_of(uint32_t fmt, uint32_t h) { return is_dxt(fmt) ? ((h + 3) / 4 ? (h + 3) / 4 : 1) : h; }
static inline uint32_t lvl(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }

/* the device state: every Set* lands here and every Get* answers from it */
typedef struct {
    uint32_t rs[256], tss[16][33], ss[16][14], tex[16], vb[16], vboff[16], vbstride[16], vbfreq[16], ib, vs, ps, decl,
        fvf, rt[4], ds, palette, swvp, npatch, scissor[4], viewport[6], material[17], clip[6][4], vsb[16], psb[16],
        lighton[8], clipstatus[2];
    uint32_t vsc[256][4], psc[32][4], vsi[16][4], psi[16][4];
    uint8_t light[8][104];
    float transform[512][16];
} D3DState;
extern D3DState st;
/* 64-byte chunks of st written since gl.c last sent them: every writer marks what it wrote, so the per-draw state
   patch compares only those (pointers outside st are ignored, so helpers can mark whatever slot they get) */
extern uint32_t g_st_dirty[(sizeof(D3DState) / 64 + 32) / 32];
static inline void st_dirty(const void* p, uint32_t n) {
    uintptr_t o = (uintptr_t)p - (uintptr_t)&st;
    if (o >= sizeof st || !n) return;
    for (uintptr_t c = o / 64, e = (o + n - 1) / 64; c <= e; c++) g_st_dirty[c >> 5] |= 1u << (c & 31);
}
#define ST_DIRTY(f) st_dirty(&(f), sizeof(f))
extern uint32_t g_backbuffer;
/* speed report (Present logs it): draw calls and texture uploads, and the time gl.c spends on them and on swapping */
extern uint32_t g_draws, g_uploads, g_buf_uploads, g_buf_bytes;  /* buffer: static vertex/index data sent again */
extern uint64_t g_draw_us, g_upload_us, g_swap_us, g_glcall_us, g_render_us;  /* see the report in d3d9_null.c */
/* heap held in texture level copies, vertex/index buffer copies and gl.c's command buffers */
extern uint32_t g_tex_bytes, g_vbuf_bytes, g_cmd_bytes;
extern uint32_t g_client_draws, g_client_bytes;  /* draws whose vertices were copied into the command, and the bytes */
void res_drop_bits(Res* r);

/* shaders and vertex declarations keep a copy of their bytecode / elements: word 2 guest address, word 3 bytes */
static inline const uint32_t* obj_copy(uint32_t obj, uint32_t* bytes) {
    if (bytes) *bytes = LD32(obj + 12);
    return H(LD32(obj + 8));
}

#ifdef USM_VITAGL
void gl_init(uint32_t bw, uint32_t bh);
void gl_present(void);
void gl_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);  /* rects: guest */
void gl_upload(Res* r, uint32_t level);
void gl_free(Res* r);  /* a released texture or buffer: its GL objects go */
uint32_t gl_mem_free_mb(int ram);  /* vitaGL's free pool memory: VRAM, or RAM (ram = 1) */
float gl_world_dist(void);  /* the draw distance while the fogged world view is up, else 0 (map, cutscenes, off) */
/* prim: D3DPRIMITIVETYPE, n: primitives; streams from st unless up (DrawPrimitiveUP data, stride);
   idx: index data (NULL: none), idx32: 32-bit indices; base: vertex added to every index / first vertex */
void gl_draw(uint32_t prim, uint32_t n, const uint8_t* up, uint32_t up_stride, const void* idx, int idx32, int32_t base,
             uint32_t minv, uint32_t numv);  /* indexed: vertices base+minv .. +numv are the ones used */
#else
static inline void gl_init(uint32_t bw, uint32_t bh) { (void)bw, (void)bh; }
static inline void gl_present(void) {}
static inline void gl_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    (void)count, (void)rects, (void)flags, (void)color, (void)z, (void)stencil;
}
static inline void gl_upload(Res* r, uint32_t level) { (void)r, (void)level; }
static inline void gl_free(Res* r) { (void)r; }
static inline uint32_t gl_mem_free_mb(int ram) { (void)ram; return 0; }
static inline float gl_world_dist(void) { return 0; }
static inline void gl_draw(uint32_t prim, uint32_t n, const uint8_t* up, uint32_t up_stride, const void* idx, int idx32,
                           int32_t base, uint32_t minv, uint32_t numv) {
    (void)prim, (void)n, (void)up, (void)up_stride, (void)idx, (void)idx32, (void)base, (void)minv, (void)numv;
}
#endif
