/* Direct3D 9 objects (device, textures, buffers, surfaces, shaders) with lockable memory and a full state store, so
   the game runs its whole frame. Without USM_VITAGL nothing reaches the GPU: Present logs the frame rate, which is
   then the game's own CPU cost on the Vita. With USM_VITAGL, gl.c draws from this state.
   Unlisted methods are survey stubs returning D3D_OK. */
#include <malloc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "d3d9.h"

static uint32_t vt_d3d, vt_dev, vt_tex, vt_cube, vt_surf, vt_vb, vt_ib, vt_vs, vt_ps, vt_decl, vt_query, vt_sb, vt_swap;

static uint32_t new_res(uint32_t vt, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt) {
    uint32_t o = com_new(vt, 16);
    Res* r = guest_alloc(sizeof(Res));
    r->w = w, r->h = h, r->levels = levels ? levels : 1, r->fmt = fmt;
    ST32(o + 8, G(r));
    return o;
}

uint32_t g_tex_bytes, g_vbuf_bytes, g_cmd_bytes, g_client_draws, g_client_bytes;
static uint32_t level_size(Res* r, uint32_t l) { return pitch_of(r->fmt, lvl(r->w, l)) * rows_of(r->fmt, lvl(r->h, l)); }
static void* level_bits(Res* r, uint32_t l) {
    if (!r->bits[l]) r->bits[l] = guest_alloc(level_size(r, l)), g_tex_bytes += level_size(r, l);
    return r->bits[l];
}
static void free_levels(Res* r) {
    for (int l = 0; l < 16; l++) {
        if (r->surf[l]) res(r->surf[l])->bits[0] = NULL;
        if (r->bits[l]) free(r->bits[l]), r->bits[l] = NULL, g_tex_bytes -= level_size(r, l);
    }
}
/* A managed texture the game loaded and drew with: vitaGL holds it now, so our copy only doubles its memory.
   shortcut: a later partial lock sees garbage outside what it writes; the log says if the game ever locks again */
void res_drop_bits(Res* r) { free_levels(r), r->dropped = 1; }
uint32_t g_static_writes;  /* write Locks of static vertex/index buffers (loading), for gl.c's early cull */
/* ---- lifetime: a texture or buffer is freed when its count reaches 0. As in D3D9, the device's Set* hold a
   reference and Get* hand one out, so nothing still bound is freed under the renderer. The object itself stays
   (16 bytes), so a stale pointer reads an empty resource instead of freed memory ---- */
static void res_free(uint32_t o) {
    Res* r = res(o);
    if (r->usage & 1) vlog("render target %ux%u freed (thread %x)\n", r->w, r->h, sceKernelGetThreadId());
    gl_free(r);
    for (int l = 0; l < 16; l++)
        if (r->surf[l]) res(r->surf[l])->parent = 0;
    if (r->size) {  /* a buffer */
        if (r->bits[0]) free(r->bits[0]), r->bits[0] = NULL, g_vbuf_bytes -= r->size;
    }
    else free_levels(r);
}
static void ref(uint32_t o) { if (o) __atomic_add_fetch((uint32_t*)H(o + 4), 1, __ATOMIC_SEQ_CST); }
static uint32_t unref(uint32_t o) {
    uint32_t* rc = H(o + 4);
    if (!*rc) return 0;  /* already gone: an extra Release frees nothing twice */
    uint32_t n = __atomic_sub_fetch(rc, 1, __ATOMIC_SEQ_CST);
    if (!n) res_free(o);
    return n;
}
static void res_Release(Ctx* c) { c->eax = unref(ARG(0)); }
/* a device slot taking a new resource: the new one gains a reference, the old one loses its */
static void rebind(uint32_t* slot, uint32_t o) {
    if (*slot == o) return;
    ref(o);
    uint32_t old = *slot;
    *slot = o;
    st_dirty(slot, 4);
    if (old) unref(old);
}

/* D3DLOCKED_RECT {Pitch, pBits} for level l */
static void lock_level(Res* r, uint32_t l, uint32_t out) {
    if (l > 15) l = 15;
    if (r->dropped == 1) r->dropped = 2, vlog("texture %ux%u format %u locked again after its copy was dropped\n", r->w, r->h, r->fmt);
    ST32(out, pitch_of(r->fmt, lvl(r->w, l)));
    ST32(out + 4, G(level_bits(r, l)));
}
/* D3DSURFACE_DESC {Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height} */
static void desc(Res* r, uint32_t l, uint32_t type, uint32_t out) {
    uint32_t d[8] = {r->fmt, type, r->usage, r->pool, 0, 0, lvl(r->w, l), lvl(r->h, l)};
    memcpy(H(out), d, 32);
}

/* ---- surfaces ---- */
static void surf_LockRect(Ctx* c) { lock_level(res(ARG(0)), 0, ARG(1)); c->eax = 0; }
static void surf_GetDesc(Ctx* c) { desc(res(ARG(0)), 0, 1, ARG(1)); c->eax = 0; }  /* D3DRTYPE_SURFACE */
static void res_Unlock(Ctx* c) { c->eax = 0; }
/* a texture level's surface shares the level's memory: unlocking it updates the texture */
static void surf_UnlockRect(Ctx* c) {
    Res* r = res(ARG(0));
    if (r->parent) gl_upload(res(r->parent), r->plevel);
    c->eax = 0;
}
static const ShimDef surf_impl[] = {{"LockRect", surf_LockRect}, {"UnlockRect", surf_UnlockRect},
                                    {"GetDesc", surf_GetDesc}, {NULL, NULL}};
static uint32_t new_surface(uint32_t w, uint32_t h, uint32_t fmt) { return new_res(vt_surf, w, h, 1, fmt); }

/* ---- textures ---- */
static void tex_LockRect(Ctx* c) { lock_level(res(ARG(0)), ARG(1), ARG(2)); c->eax = 0; }
static void tex_GetLevelDesc(Ctx* c) { desc(res(ARG(0)), ARG(1), 1, ARG(2)); c->eax = 0; }
static void tex_GetLevelCount(Ctx* c) { c->eax = res(ARG(0))->levels; }
static void tex_GetSurfaceLevel(Ctx* c) {
    Res* r = res(ARG(0));
    uint32_t l = ARG(1) > 15 ? 15 : ARG(1);
    if (!r->surf[l]) {
        r->surf[l] = new_surface(lvl(r->w, l), lvl(r->h, l), r->fmt);
        Res* s = res(r->surf[l]);
        s->bits[0] = level_bits(r, l), s->parent = ARG(0), s->plevel = l, s->usage = r->usage;
    }
    ST32(ARG(2), r->surf[l]);
    c->eax = 0;
}
static void tex_UnlockRect(Ctx* c) { gl_upload(res(ARG(0)), ARG(1) > 15 ? 15 : ARG(1)); c->eax = 0; }
static const ShimDef tex_impl[] = {{"LockRect", tex_LockRect}, {"UnlockRect", tex_UnlockRect},
                                   {"GetLevelDesc", tex_GetLevelDesc}, {"GetLevelCount", tex_GetLevelCount},
                                   {"GetSurfaceLevel", tex_GetSurfaceLevel}, {NULL, NULL}};
/* cube: LockRect(face, level, ...), GetCubeMapSurface(face, level, out) */
static void cube_LockRect(Ctx* c) { lock_level(res(ARG(0)), ARG(2), ARG(3)); c->eax = 0; }
static void cube_GetCubeMapSurface(Ctx* c) { ST32(ARG(3), new_surface(res(ARG(0))->w, res(ARG(0))->h, res(ARG(0))->fmt)); c->eax = 0; }
static const ShimDef cube_impl[] = {{"LockRect", cube_LockRect}, {"UnlockRect", res_Unlock}, {"GetLevelDesc", tex_GetLevelDesc},
                                    {"GetLevelCount", tex_GetLevelCount}, {"GetCubeMapSurface", cube_GetCubeMapSurface},
                                    {NULL, NULL}};

/* ---- vertex and index buffers: Lock(offset, size, ppData, flags) ---- */
static void buf_Lock(Ctx* c) {
    Res* r = res(ARG(0));
    if (!r->bits[0]) r->bits[0] = guest_alloc(r->size ? r->size : 16), g_vbuf_bytes += r->size;
    static uint32_t gen;
    if (!(ARG(4) & 0x10)) {  /* not D3DLOCK_READONLY: gl.c sends it again; its spheres go stale */
        r->sent = 0, r->gen = ++gen;
        if (!(r->usage & 0x200)) g_static_writes++;  /* loading: gl.c's early cull forgets its skips */
    }
    ST32(ARG(3), G(r->bits[0]) + ARG(1));
    c->eax = 0;
}
static void buf_GetDesc(Ctx* c) {  /* D3DVERTEXBUFFER_DESC {Format, Type, Usage, Pool, Size, FVF} */
    Res* r = res(ARG(0));
    uint32_t d[6] = {r->fmt, 3, r->usage, r->pool, r->size, 0};
    memcpy(H(ARG(1)), d, 24);
    c->eax = 0;
}
static const ShimDef buf_impl[] = {{"Lock", buf_Lock}, {"Unlock", res_Unlock}, {"GetDesc", buf_GetDesc}, {NULL, NULL}};

D3DState st;  /* the device state (see the state section below) */
uint32_t g_st_dirty[(sizeof(D3DState) / 64 + 32) / 32];

/* a Set* that changes nothing returns at once (nothing marked dirty, nothing recorded); profile builds count it
   against its shim (shims.c shim_dump) */
#ifdef RC_PROFILE
void shim_same(void);
#define SAME_HIT shim_same()
#else
#define SAME_HIT (void)0
#endif
#define SAME(cond) do { if (cond) { SAME_HIT; c->eax = 0; return; } } while (0)
static int same_rows(uint32_t (*tab)[4], uint32_t rows, uint32_t start, uint32_t data, uint32_t count) {
    return start < rows && count && !memcmp(tab[start], H(data), 16 * (count < rows - start ? count : rows - start));
}

/* ---- what the game asks of the renderer, each distinct thing logged once (the vitaGL renderer's to-do list) ---- */
static int first_time(uint32_t kind, uint32_t key) {
    if (!log_on()) return 0;  /* nothing to log to: skip the lookup (and the shader dumps) */
    static uint32_t seen[4096];
    uint32_t h = (key * 2654435761u ^ kind * 40503u) | 1, i = h & 4095;
    for (unsigned n = 0; n < 4096; n++, i = (i + 1) & 4095) {
        if (seen[i] == h) return 0;
        if (!seen[i]) { seen[i] = h; return 1; }
    }
    return 0;
}
static uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 0x811c9dc5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}
/* shader bytecode to ux0:data/usm/shaders/<hash>.vs|ps: the exact set the translator must handle */
static void dump_shader(uint32_t code, const char* ext) {
    uint32_t n = 0;
    while (n < 0x10000 && LD32(code + 4 * n) != 0x0000ffffu) n++;  /* end token */
    n = 4 * (n + 1);
    uint32_t h = fnv(H(code), n);
    if (!first_time(1, h)) return;
    char path[96];
    mkdir(USM_DIR "shaders", 0777);
    snprintf(path, sizeof path, USM_DIR "shaders/%08x.%s", h, ext);
    FILE* f = fopen(path, "wb");
    if (f) fwrite(H(code), 1, n, f), fclose(f);
    vlog("shader %08x.%s: version %08x, %u bytes\n", h, ext, LD32(code), n);
}

/* ---- device ---- */
uint32_t g_backbuffer;
static uint32_t g_depth, g_frames;
static uint64_t g_t0, g_last;
uint32_t g_draws, g_uploads, g_buf_uploads, g_buf_bytes;
uint64_t g_draw_us, g_upload_us, g_swap_us, g_glcall_us, g_render_us;

static void dev_CreateTexture(Ctx* c) {  /* (w, h, levels, usage, format, pool, ppTex, shared) */
    uint32_t levels = ARG(3);
    if (!levels) for (uint32_t m = ARG(1) > ARG(2) ? ARG(1) : ARG(2); m; m >>= 1) levels++;
    uint32_t t = new_res(vt_tex, ARG(1), ARG(2), levels, ARG(5));
    res(t)->usage = ARG(4), res(t)->pool = ARG(6);
    if (first_time(2, ARG(5) << 8 ^ ARG(4) << 4 ^ ARG(6)))
        vlog("texture format %u usage %x pool %u (first %ux%u, %u levels)\n", ARG(5), ARG(4), ARG(6), ARG(1), ARG(2), levels);
    ST32(ARG(7), t);
    c->eax = 0;
}
static void dev_CreateCubeTexture(Ctx* c) {  /* (edge, levels, usage, format, pool, pp, shared) */
    uint32_t t = new_res(vt_cube, ARG(1), ARG(1), ARG(2) ? ARG(2) : 1, ARG(4));
    res(t)->cube = 1;
    ST32(ARG(6), t);
    c->eax = 0;
}
static void dev_CreateVolumeTexture(Ctx* c) { ST32(ARG(8), new_res(vt_tex, ARG(1), ARG(2), ARG(4) ? ARG(4) : 1, ARG(6))); c->eax = 0; }
static void new_buffer(Ctx* c, uint32_t vt, uint32_t pp) {  /* (length, usage, fvf/format, pool, pp, shared) */
    uint32_t b = new_res(vt, 0, 0, 1, ARG(3));
    res(b)->size = ARG(1), res(b)->usage = ARG(2), res(b)->pool = ARG(4);
    ST32(pp, b);
    c->eax = 0;
}
static void dev_CreateVertexBuffer(Ctx* c) { new_buffer(c, vt_vb, ARG(5)); }
static void dev_CreateIndexBuffer(Ctx* c) { new_buffer(c, vt_ib, ARG(5)); }
static void dev_CreateRenderTarget(Ctx* c) { ST32(ARG(7), new_surface(ARG(1), ARG(2), ARG(3))); c->eax = 0; }
static void dev_CreateDepthStencilSurface(Ctx* c) { ST32(ARG(7), new_surface(ARG(1), ARG(2), ARG(3))); c->eax = 0; }
static void dev_CreateOffscreenPlainSurface(Ctx* c) { ST32(ARG(5), new_surface(ARG(1), ARG(2), ARG(3))); c->eax = 0; }
static void dev_GetBackBuffer(Ctx* c) { ST32(ARG(4), g_backbuffer); c->eax = 0; }
static void dev_GetRenderTarget(Ctx* c) { ST32(ARG(2), g_backbuffer); c->eax = 0; }
static void dev_GetDepthStencilSurface(Ctx* c) { ST32(ARG(1), g_depth); c->eax = 0; }
/* shaders and declarations keep a copy of what made them (word 2: guest copy, word 3: bytes) for Get*: the renderer's
   input too */
static uint32_t with_copy(uint32_t vt, uint32_t src, uint32_t n) {
    uint32_t o = com_new(vt, 16), p = G(guest_alloc(n));
    memcpy(H(p), H(src), n);
    ST32(o + 8, p), ST32(o + 12, n);
    return o;
}
static uint32_t shader_bytes(uint32_t code) {
    uint32_t n = 0;
    while (n < 0x10000 && LD32(code + 4 * n) != 0x0000ffffu) n++;
    return 4 * (n + 1);
}
static void dev_CreateVertexShader(Ctx* c) { dump_shader(ARG(1), "vs"); ST32(ARG(2), with_copy(vt_vs, ARG(1), shader_bytes(ARG(1)))); c->eax = 0; }
static void dev_CreatePixelShader(Ctx* c) { dump_shader(ARG(1), "ps"); ST32(ARG(2), with_copy(vt_ps, ARG(1), shader_bytes(ARG(1)))); c->eax = 0; }
static void shader_GetFunction(Ctx* c) {  /* (data, size*): data NULL asks for the size */
    uint32_t o = ARG(0), n = LD32(o + 12);
    if (ARG(1)) memcpy(H(ARG(1)), H(LD32(o + 8)), n);
    ST32(ARG(2), n);
    c->eax = 0;
}
static void decl_GetDeclaration(Ctx* c) {  /* (elements, count*): count includes D3DDECL_END */
    uint32_t o = ARG(0), n = LD32(o + 12);
    if (ARG(1)) memcpy(H(ARG(1)), H(LD32(o + 8)), n);
    ST32(ARG(2), n / 8);
    c->eax = 0;
}
static void dev_CreateVertexDeclaration(Ctx* c) {  /* D3DVERTEXELEMENT9 {stream, offset, type, method, usage, index} */
    uint32_t e = ARG(1), n = 0;
    while (n < 64 && LD16(e + 8 * n) != 0xff) n++;
    uint32_t decl = with_copy(vt_decl, e, 8 * (n + 1));
    if (first_time(3, fnv(H(e), 8 * n))) {
        char b[512];
        int k = snprintf(b, sizeof b, "vertex declaration:");
        for (uint32_t i = 0; i < n && k < 480; i++, e += 8)
            k += snprintf(b + k, sizeof b - k, " [s%u +%u type %u usage %u.%u]", LD16(e), LD16(e + 2), LD8(e + 4), LD8(e + 6), LD8(e + 7));
        vlog("%s\n", b);
    }
    ST32(ARG(2), decl);
    c->eax = 0;
}
static void dev_UpdateTexture(Ctx* c) {  /* (src, dst): dst's levels match src's from the one of the same size */
    Res *s = res(ARG(1)), *d = res(ARG(2));
    uint32_t k = 0;
    while (k < s->levels && lvl(s->w, k) > d->w) k++;
    for (uint32_t l = 0; l < d->levels && k + l < s->levels && k + l < 16 && !d->cube; l++)
        if (s->bits[k + l])
            memcpy(level_bits(d, l), s->bits[k + l], pitch_of(d->fmt, lvl(d->w, l)) * rows_of(d->fmt, lvl(d->h, l)));
    if (!d->cube) gl_upload(d, 0);
    c->eax = 0;
}
/* SetFVF and SetVertexDeclaration replace each other (D3D9 makes a declaration from the FVF) */
static void dev_SetFVF(Ctx* c) { SAME(st.fvf == ARG(1) && !st.decl); st.fvf = ARG(1), st.decl = 0, ST_DIRTY(st.fvf), ST_DIRTY(st.decl); if (first_time(4, ARG(1))) vlog("FVF %08x\n", ARG(1)); c->eax = 0; }
static void note_prim(uint32_t type, const char* how) { if (first_time(5, type << 4 | (uint32_t)how[0])) vlog("draw %s, primitive type %u\n", how, type); }
static void dev_DrawPrimitive(Ctx* c) {  /* (type, startVertex, primCount) */
    note_prim(ARG(1), "DrawPrimitive");
    gl_draw(ARG(1), ARG(3), NULL, 0, NULL, 0, (int32_t)ARG(2), 0, 0);
    c->eax = 0;
}
static void dev_DrawIndexedPrimitive(Ctx* c) {  /* (type, baseVertex, minIndex, numVertices, startIndex, primCount) */
    note_prim(ARG(1), "IndexedPrimitive");
    if (st.ib) {
        Res* ib = res(st.ib);
        int i32 = ib->fmt == 102;  /* D3DFMT_INDEX32 */
        if (ib->bits[0]) gl_draw(ARG(1), ARG(6), NULL, 0, (uint8_t*)ib->bits[0] + ARG(5) * (i32 ? 4 : 2), i32, (int32_t)ARG(2), ARG(3), ARG(4));
    }
    c->eax = 0;
}
/* the UP draws leave stream 0 (and the indices) unset, as D3D does */
static void dev_DrawPrimitiveUP(Ctx* c) {  /* (type, primCount, data, stride) */
    note_prim(ARG(1), "UP");
    RC_RD(ARG(3), ARG(2) * 3 * ARG(4)) (void)0;  /* hazard tracer: the vertices it copies (at most 3 per primitive) */
    gl_draw(ARG(1), ARG(2), H(ARG(3)), ARG(4), NULL, 0, 0, 0, 0);
    st.vb[0] = 0, st.vboff[0] = 0, st.vbstride[0] = 0, ST_DIRTY(st.vb[0]), ST_DIRTY(st.vboff[0]), ST_DIRTY(st.vbstride[0]);
    c->eax = 0;
}
static void dev_DrawIndexedPrimitiveUP(Ctx* c) {  /* (type, minIndex, numVertices, primCount, indices, fmt, data, stride) */
    note_prim(ARG(1), "xIndexedUP");
    RC_RD(ARG(7), (ARG(2) + ARG(3)) * ARG(8)) RC_RD(ARG(5), ARG(4) * 3 * (ARG(6) == 102 ? 4 : 2)) (void)0;
    gl_draw(ARG(1), ARG(4), H(ARG(7)), ARG(8), H(ARG(5)), ARG(6) == 102, 0, ARG(2), ARG(3));
    st.vb[0] = 0, st.vboff[0] = 0, st.vbstride[0] = 0, st.ib = 0;
    ST_DIRTY(st.vb[0]), ST_DIRTY(st.vboff[0]), ST_DIRTY(st.vbstride[0]), ST_DIRTY(st.ib);
    c->eax = 0;
}
static void dev_Clear(Ctx* c) {  /* (count, rects, flags, color, z, stencil) */
    float z;
    uint32_t zb = ARG(5);
    memcpy(&z, &zb, 4);
    gl_clear(ARG(1), ARG(2), ARG(3), ARG(4), z, ARG(6));
    c->eax = 0;
}
static void dev_CreateQuery(Ctx* c) {  /* (type, pp): pp NULL asks whether the type is supported */
    if (ARG(2)) {
        uint32_t q = com_new(vt_query, 16);
        ST32(q + 8, ARG(1));
        ST32(ARG(2), q);
    }
    c->eax = 0;
}
static void dev_CreateAdditionalSwapChain(Ctx* c) { ST32(ARG(2), com_new(vt_swap, 16)); c->eax = 0; }
/* ---- state blocks: a snapshot of the whole device state (any type is treated as D3DSBT_ALL). Capture takes it
   again, Apply puts it back except the render target and depth surface, which D3D9 state blocks don't hold. A block
   holds references to its textures and buffers, as in D3D9. The game saves and restores around the minimap. ---- */
static void sb_hold(D3DState* s, int on) {
    uint32_t r[33];
    memcpy(r, s->tex, 64), memcpy(r + 16, s->vb, 64), r[32] = s->ib;
    for (int i = 0; i < 33; i++)
        if (r[i]) on ? ref(r[i]) : (void)unref(r[i]);
}
static D3DState* sb(uint32_t o) { return LD32(o + 8) ? H(LD32(o + 8)) : NULL; }
static void sb_capture(D3DState* s) {
    if (s) sb_hold(s, 0), *s = st, sb_hold(s, 1);
}
static uint32_t new_sb(void) {
    uint32_t o = com_new(vt_sb, 16);
    D3DState* s = calloc(1, sizeof *s);
    ST32(o + 8, G(s));
    sb_capture(s);
    return o;
}
static void dev_CreateStateBlock(Ctx* c) { ST32(ARG(2), new_sb()); c->eax = 0; }
static void dev_EndStateBlock(Ctx* c) { ST32(ARG(1), new_sb()); c->eax = 0; }
static void sb_Capture(Ctx* c) { sb_capture(sb(ARG(0))); c->eax = 0; }
static void sb_Apply(Ctx* c) {
    const D3DState* s = sb(ARG(0));
    c->eax = 0;
    if (!s) return;
    uint32_t rt[4], ds = st.ds, tex[16], vb[16], ib = st.ib;
    memcpy(rt, st.rt, sizeof rt), memcpy(tex, st.tex, sizeof tex), memcpy(vb, st.vb, sizeof vb);
    st = *s;
    memcpy(st.rt, rt, sizeof rt), st.ds = ds;
    /* the bound resources change through rebind, so their counts stay right */
    memcpy(st.tex, tex, sizeof tex), memcpy(st.vb, vb, sizeof vb), st.ib = ib;
    for (int i = 0; i < 16; i++) rebind(&st.tex[i], s->tex[i]), rebind(&st.vb[i], s->vb[i]);
    rebind(&st.ib, s->ib);
    ST_DIRTY(st);
}
static void sb_Release(Ctx* c) {
    uint32_t* rc = H(ARG(0) + 4);
    if (!*rc) { c->eax = 0; return; }
    c->eax = __atomic_sub_fetch(rc, 1, __ATOMIC_SEQ_CST);
    D3DState* s = sb(ARG(0));
    if (!c->eax && s) sb_hold(s, 0), free(s), ST32(ARG(0) + 8, 0);
}
static void dev_GetSwapChain(Ctx* c) { ST32(ARG(2), com_new(vt_swap, 16)); c->eax = 0; }
static void dev_GetDirect3D(Ctx* c) { ST32(ARG(1), com_new(vt_d3d, 16)); c->eax = 0; }
static void dev_GetAvailableTextureMem(Ctx* c) { c->eax = 128u << 20; }
static void dev_GetDisplayMode(Ctx* c) { uint32_t m[4] = {W, HGT, 60, FMT_X8R8G8B8}; memcpy(H(ARG(2)), m, 16); c->eax = 0; }
/* Vita speed: the script commands set_pedestrian_density / set_traffic_density / set_parking_density store 0..1 floats
   (FUN_006c2e10, 6bcf30, 6bcf40) that the spawners read; capping them means fewer people and cars to simulate and draw.
   Checked once a frame: the scripts only set them now and then. The cap: ux0:data/usm/density.txt (0..1, default
   0.15; 1 = as the game sets them). */
float density_cap(void) {  /* also native.c f_6c2e10 */
    static float cap = -1;
    if (cap < 0) {
        cap = 0.15f;
        FILE* f = fopen(USM_DIR "density.txt", "r");
        if (f) {
            if (fscanf(f, "%f", &cap) != 1 || cap < 0) cap = 0.15f;
            fclose(f);
        }
        if (getenv("VPC_DENSITY")) cap = (float)atof(getenv("VPC_DENSITY"));  /* vpc tests */
        vlog("density cap %.2f (density.txt)\n", cap);
    }
    return cap;
}
static void cap_density(void) {
    static const uint32_t at[] = {0x937ff0u, 0x937ff8u, 0x937ffcu};  /* pedestrians, traffic, parked cars */
    float cap = density_cap();
    for (unsigned i = 0; i < 3; i++)
        if (LDF32(at[i]) > cap) STF32(at[i], cap);
}
/* 16:9 without stretching: the game's 3D camera works on a fixed 640x480 virtual screen (0x76e020/0x76e030), so its
   aspect is always 4:3 times the pixel-aspect setting at 0x921e20 (1.0; setter 0x540070, which flags the camera for a
   recompute with 0x95c1ea). That setting corrects it for the real backbuffer: same horizontal field of view, a little
   less vertically (less to draw, too). The 2D layout keeps its 640x480 coordinates. */
static uint32_t g_bbw, g_bbh;
uint32_t g_culled;  /* draws gl.c left out, beyond the draw distance (the report) */
uint32_t g_early;   /* render-list entries gl.c skipped before their draws (early cull) */
static void fix_aspect(void) {
    if (!g_bbw || !g_bbh) return;
    float want = ((float)g_bbw / g_bbh) / (4.0f / 3.0f);
    if (LDF32(0x921e20) != want) STF32(0x921e20, want), ST8(0x95c1ea, 1);
}
/* Build-time cull: the entity pass 540380 (render-list build, core 0) keeps entities within sqrt(0x921f0c) = 3000 m
   (camera above 50 m) or sqrt(0x921f10) = 1700 m of the camera (xz distance to the entity origin), far past the fog.
   While the fogged world view is up both are capped at draw distance x ux0:data/usm/build_cull.txt (default 2; 0 =
   off); the map and cutscenes get the game's values back. Only 540380 reads them. */
static void cap_build_dist(void) {
    static float mult = -1, hi, lo;
    if (mult < 0) {
        mult = 2;
        FILE* f = fopen(USM_DIR "build_cull.txt", "r");
        if (f) {
            if (fscanf(f, "%f", &mult) != 1 || mult < 0) mult = 2;
            fclose(f);
        }
        if (getenv("VPC_BUILD_CULL")) mult = (float)atof(getenv("VPC_BUILD_CULL"));  /* vpc tests */
        hi = LDF32(0x921f0c), lo = LDF32(0x921f10);
        vlog("build cull x%.2f of the draw distance (build_cull.txt)\n", mult);
    }
    float d = gl_world_dist() * mult, d2 = d * d;
    float h = d > 0 && d2 < hi ? d2 : hi, l = d > 0 && d2 < lo ? d2 : lo;
    if (LDF32(0x921f0c) != h) STF32(0x921f0c, h);
    if (LDF32(0x921f10) != l) STF32(0x921f10, l);
}
/* once per world frame, on the main thread (native.c f_73ea70): Present may run on the pipeline's worker */
void d3d_frame_fixups(void) { cap_density(), fix_aspect(), cap_build_dist(); }
static void dev_Present(Ctx* c) {
    uint64_t t = sceKernelGetProcessTimeWide();
    if (!g_t0) g_t0 = g_last = t;
    extern int g_shim_verbose;
    if (g_frames == 3 && g_shim_verbose) g_shim_verbose = 0, vlog("call trace off after 3 frames\n");
#ifdef USM_DEBUG
    static int trace_at = -1;  /* vpc: VPC_TRACE_AT=<frame> logs every shim call of that one frame */
    if (trace_at < 0) trace_at = getenv("VPC_TRACE_AT") ? atoi(getenv("VPC_TRACE_AT")) : 0;
    if (trace_at && g_frames == (uint32_t)trace_at) g_shim_verbose = 1, vlog("call trace: frame %u\n", g_frames);
    if (trace_at && g_frames == (uint32_t)trace_at + 1) g_shim_verbose = 0;
#endif
    static uint32_t n;
    static uint64_t prev, worst;  /* the longest frame of the report's 2 s: the stutters the average hides */
    static uint32_t up0, buf0, dr0, w_up, w_buf, w_dr;  /* counters at the last Present; the worst frame's share */
    if (prev && t - prev > worst) worst = t - prev, w_up = g_uploads - up0, w_buf = g_buf_bytes - buf0, w_dr = g_draws - dr0;
    prev = t, up0 = g_uploads, buf0 = g_buf_bytes, dr0 = g_draws;
    n++, g_frames++;
    if (t - g_last >= 2000000 && log_on()) {  /* every 2 s, per frame: the renderer's share, the rest is the game's CPU time */
        double f = n, ms = (double)(t - g_last) / 1000.0 / f;
        /* game thread: recording draws and uploads, waiting for the render thread, the rest is the game; the render
           thread's busy time and its share inside vitaGL's glDraw* */

        vlog("frame %u: %.1f fps (%.1f ms, worst %.0f: record %.1f, upload %.1f, wait %.1f, game %.1f; render thread %.1f, vitaGL %.1f), %.0f draws (%.0f beyond the draw distance, %.0f entries skipped early), %u people %u cars, %.1f uploads, %.1f buffers (%.0f KB), heap %u MB (game %u, textures %u, buffers %u, commands %u, other %d), copied %.0f draws (%.0f KB), vitaGL free %u MB VRAM %u MB RAM\n",
             g_frames, 1000.0 / ms, ms, worst / 1000.0, g_draw_us / 1000.0 / f, g_upload_us / 1000.0 / f, g_swap_us / 1000.0 / f,
             ms - (g_draw_us + g_upload_us + g_swap_us) / 1000.0 / f, g_render_us / 1000.0 / f, g_glcall_us / 1000.0 / f, g_draws / f, g_culled / f, g_early / f, LD32(0x96c9cc), LD32(0x96c9ec) + LD32(0x96c9f0), g_uploads / f,
             g_buf_uploads / f, g_buf_bytes / 1024.0 / f, (unsigned)(mallinfo().uordblks >> 20), g_game_bytes >> 20, g_tex_bytes >> 20, g_vbuf_bytes >> 20, g_cmd_bytes >> 20,
             (int)((mallinfo().uordblks - g_game_bytes - g_tex_bytes - g_vbuf_bytes - g_cmd_bytes) >> 20),
             g_client_draws / f, g_client_bytes / 1024.0 / f, gl_mem_free_mb(0), gl_mem_free_mb(1));
#ifdef USM_VITAGL
        extern uint32_t g_cmd_by[8];  /* gl.c: command bytes by type (C_PATCH 1, C_UPLOAD 3, C_BUFFER 4, C_DRAW 5) */
        vlog("commands per frame: state %.0f KB, draws %.0f KB, textures %.0f KB, buffers %.0f KB; game fog %08x (on %u, start %.0f end %.0f)\n",
             g_cmd_by[1] / 1024.0 / f, g_cmd_by[5] / 1024.0 / f, g_cmd_by[3] / 1024.0 / f, g_cmd_by[4] / 1024.0 / f, st.rs[34], st.rs[28],
             *(float*)&st.rs[36], *(float*)&st.rs[37]);
        memset(g_cmd_by, 0, sizeof g_cmd_by);
#endif
        if (worst > 100000) vlog("worst frame %.0f ms: %u texture uploads, %u KB buffers, %u draws\n", worst / 1000.0, w_up, w_buf >> 10, w_dr);
        up0 = buf0 = dr0 = 0;
        g_last = t, n = 0, worst = 0, g_culled = 0, g_early = 0, g_draws = g_uploads = g_buf_uploads = g_buf_bytes = g_client_draws = g_client_bytes = 0, g_draw_us = g_upload_us = g_swap_us = g_glcall_us = g_render_us = 0;
    }
    gl_present();
    c->eax = 0;
}
/* ---- device state: every Set* is recorded and every Get* answers from it (survey stubs would leave the game's
   output variables uninitialised); the vitaGL renderer draws from this same state ---- */
static uint32_t g_device;

static void state_defaults(void) {
    static const uint32_t rs[][2] = {{7, 1}, {8, 3}, {9, 2}, {14, 1}, {19, 2}, {20, 1}, {22, 3}, {23, 4}, {25, 8},
                                     {137, 1}, {168, 0xf}, {145, 1}, {174, 0}, {181, 0xffffffffu}, {56, 8}, {53, 1},
                                     {54, 1}, {55, 1}, {58, 0xffffffffu}, {59, 0xffffffffu}, {60, 0xffffffffu}, {171, 1}};
    for (unsigned i = 0; i < sizeof rs / sizeof *rs; i++) st.rs[rs[i][0]] = rs[i][1];
    for (int s = 0; s < 16; s++) {
        st.tss[s][1] = s ? 1 : 4, st.tss[s][4] = s ? 1 : 2, st.tss[s][11] = (uint32_t)s;
        st.tss[s][2] = st.tss[s][5] = 2, st.tss[s][3] = st.tss[s][6] = 1;  /* args: TEXTURE, CURRENT */
        st.ss[s][1] = st.ss[s][2] = st.ss[s][3] = 1, st.ss[s][5] = st.ss[s][6] = 1;
    }
    for (int i = 0; i < 512; i++) st.transform[i][0] = st.transform[i][5] = st.transform[i][10] = st.transform[i][15] = 1.f;
    st.viewport[2] = W, st.viewport[3] = HGT;
    float one = 1.f;
    memcpy(&st.viewport[5], &one, 4);  /* MaxZ */
}

uint32_t d3d_frame_count(void) { return g_frames; }

#define IDX(v, n) ((v) < (n) ? (v) : (n) - 1)
static void dev_SetRenderState(Ctx* c) { SAME(st.rs[IDX(ARG(1), 256)] == ARG(2)); st.rs[IDX(ARG(1), 256)] = ARG(2), ST_DIRTY(st.rs[IDX(ARG(1), 256)]); c->eax = 0; }
static void dev_GetRenderState(Ctx* c) { ST32(ARG(2), st.rs[IDX(ARG(1), 256)]); c->eax = 0; }
static void dev_SetTextureStageState(Ctx* c) { SAME(st.tss[IDX(ARG(1), 16)][IDX(ARG(2), 33)] == ARG(3)); st.tss[IDX(ARG(1), 16)][IDX(ARG(2), 33)] = ARG(3), ST_DIRTY(st.tss[IDX(ARG(1), 16)][IDX(ARG(2), 33)]); c->eax = 0; }
static void dev_GetTextureStageState(Ctx* c) { ST32(ARG(3), st.tss[IDX(ARG(1), 16)][IDX(ARG(2), 33)]); c->eax = 0; }
static void dev_SetSamplerState(Ctx* c) { SAME(st.ss[IDX(ARG(1) & 15, 16)][IDX(ARG(2), 14)] == ARG(3)); st.ss[IDX(ARG(1) & 15, 16)][IDX(ARG(2), 14)] = ARG(3), ST_DIRTY(st.ss[IDX(ARG(1) & 15, 16)][IDX(ARG(2), 14)]); c->eax = 0; }
static void dev_GetSamplerState(Ctx* c) { ST32(ARG(3), st.ss[IDX(ARG(1) & 15, 16)][IDX(ARG(2), 14)]); c->eax = 0; }
static void dev_SetTexture(Ctx* c) { SAME(st.tex[IDX(ARG(1) & 15, 16)] == ARG(2)); rebind(&st.tex[IDX(ARG(1) & 15, 16)], ARG(2)); c->eax = 0; }
static void dev_GetTexture(Ctx* c) { uint32_t t = st.tex[IDX(ARG(1) & 15, 16)]; ref(t), ST32(ARG(2), t); c->eax = 0; }
static void dev_SetTransform(Ctx* c) { SAME(!memcmp(st.transform[IDX(ARG(1), 512)], H(ARG(2)), 64)); RC_RD(ARG(2), 64) memcpy(st.transform[IDX(ARG(1), 512)], H(ARG(2)), 64), ST_DIRTY(st.transform[IDX(ARG(1), 512)]); c->eax = 0; }
static void dev_GetTransform(Ctx* c) { memcpy(H(ARG(2)), st.transform[IDX(ARG(1), 512)], 64); c->eax = 0; }
static void dev_MultiplyTransform(Ctx* c) {
    float* a = st.transform[IDX(ARG(1), 512)], b[16], r[16];
    memcpy(b, H(ARG(2)), 64);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
    memcpy(a, r, 64), st_dirty(a, 64);
    c->eax = 0;
}
static void dev_SetViewport(Ctx* c) { memcpy(st.viewport, H(ARG(1)), 24), ST_DIRTY(st.viewport); c->eax = 0; }
static void dev_GetViewport(Ctx* c) { memcpy(H(ARG(1)), st.viewport, 24); c->eax = 0; }
static void dev_SetMaterial(Ctx* c) { memcpy(st.material, H(ARG(1)), 68), ST_DIRTY(st.material); c->eax = 0; }
static void dev_GetMaterial(Ctx* c) { memcpy(H(ARG(1)), st.material, 68); c->eax = 0; }
static void dev_SetLight(Ctx* c) { memcpy(st.light[IDX(ARG(1), 8)], H(ARG(2)), 104), ST_DIRTY(st.light[IDX(ARG(1), 8)]); c->eax = 0; }
static void dev_GetLight(Ctx* c) { memcpy(H(ARG(2)), st.light[IDX(ARG(1), 8)], 104); c->eax = 0; }
static void dev_LightEnable(Ctx* c) { st.lighton[IDX(ARG(1), 8)] = ARG(2), ST_DIRTY(st.lighton); c->eax = 0; }
static void dev_GetLightEnable(Ctx* c) { ST32(ARG(2), st.lighton[IDX(ARG(1), 8)]); c->eax = 0; }
static void dev_SetClipPlane(Ctx* c) { memcpy(st.clip[IDX(ARG(1), 6)], H(ARG(2)), 16), ST_DIRTY(st.clip); c->eax = 0; }
static void dev_GetClipPlane(Ctx* c) { memcpy(H(ARG(2)), st.clip[IDX(ARG(1), 6)], 16); c->eax = 0; }
static void dev_SetClipStatus(Ctx* c) { memcpy(st.clipstatus, H(ARG(1)), 8), ST_DIRTY(st.clipstatus); c->eax = 0; }
static void dev_GetClipStatus(Ctx* c) { memcpy(H(ARG(1)), st.clipstatus, 8); c->eax = 0; }
static void dev_SetScissorRect(Ctx* c) { memcpy(st.scissor, H(ARG(1)), 16), ST_DIRTY(st.scissor); c->eax = 0; }
static void dev_GetScissorRect(Ctx* c) { memcpy(H(ARG(1)), st.scissor, 16); c->eax = 0; }
/* the world frame's last call after its render lists (73ea70 tail): the pipeline's worker does it after them */
void d3d_unbind_stream0(void) { rebind(&st.vb[0], 0), st.vboff[0] = 0, st.vbstride[0] = 0, ST_DIRTY(st.vboff[0]), ST_DIRTY(st.vbstride[0]); }
static void dev_SetStreamSource(Ctx* c) {  /* (stream, vb, offset, stride) */
    extern int pipe_skip_unbind(void);
    if (ARG(1) == 0 && ARG(2) == 0 && pipe_skip_unbind()) { c->eax = 0; return; }
    uint32_t s = IDX(ARG(1), 16);
    SAME(st.vb[s] == ARG(2) && st.vboff[s] == ARG(3) && st.vbstride[s] == ARG(4));
    rebind(&st.vb[s], ARG(2)), st.vboff[s] = ARG(3), st.vbstride[s] = ARG(4), ST_DIRTY(st.vboff[s]), ST_DIRTY(st.vbstride[s]);
    c->eax = 0;
}
static void dev_GetStreamSource(Ctx* c) {
    uint32_t s = IDX(ARG(1), 16);
    ref(st.vb[s]), ST32(ARG(2), st.vb[s]), ST32(ARG(3), st.vboff[s]), ST32(ARG(4), st.vbstride[s]);
    c->eax = 0;
}
static void dev_SetStreamSourceFreq(Ctx* c) { st.vbfreq[IDX(ARG(1), 16)] = ARG(2), ST_DIRTY(st.vbfreq); c->eax = 0; }
static void dev_GetStreamSourceFreq(Ctx* c) { ST32(ARG(2), st.vbfreq[IDX(ARG(1), 16)]); c->eax = 0; }
static void dev_SetIndices(Ctx* c) { SAME(st.ib == ARG(1)); rebind(&st.ib, ARG(1)); c->eax = 0; }
static void dev_GetIndices(Ctx* c) { ref(st.ib), ST32(ARG(1), st.ib); c->eax = 0; }
static void dev_SetVertexShader(Ctx* c) { SAME(st.vs == ARG(1)); st.vs = ARG(1), ST_DIRTY(st.vs); c->eax = 0; }
static void dev_GetVertexShader(Ctx* c) { ST32(ARG(1), st.vs); c->eax = 0; }
static void dev_SetPixelShader(Ctx* c) { SAME(st.ps == ARG(1)); st.ps = ARG(1), ST_DIRTY(st.ps); c->eax = 0; }
static void dev_GetPixelShader(Ctx* c) { ST32(ARG(1), st.ps); c->eax = 0; }
static void dev_SetVertexDeclaration(Ctx* c) { SAME(st.decl == ARG(1) && !st.fvf); st.decl = ARG(1), st.fvf = 0, ST_DIRTY(st.decl), ST_DIRTY(st.fvf); c->eax = 0; }
static void dev_GetVertexDeclaration(Ctx* c) { ST32(ARG(1), st.decl); c->eax = 0; }
static void dev_GetFVF(Ctx* c) { ST32(ARG(1), st.fvf); c->eax = 0; }
/* shader constants: (start, data, count); F rows are 4 floats, I rows 4 ints, B one BOOL each */
static void consts(uint32_t (*tab)[4], uint32_t rows, uint32_t start, uint32_t data, uint32_t count, int get) {
    for (uint32_t i = 0; i < count && start + i < rows; i++)
        get ? memcpy(H(data + 16 * i), tab[start + i], 16) : (RC_RD(data + 16 * i, 16) memcpy(tab[start + i], H(data + 16 * i), 16), st_dirty(tab[start + i], 16));
}
static void dev_SetVertexShaderConstantF(Ctx* c) { SAME(same_rows(st.vsc, 256, ARG(1), ARG(2), ARG(3))); consts(st.vsc, 256, ARG(1), ARG(2), ARG(3), 0); c->eax = 0; }
static void dev_GetVertexShaderConstantF(Ctx* c) { consts(st.vsc, 256, ARG(1), ARG(2), ARG(3), 1); c->eax = 0; }
static void dev_SetPixelShaderConstantF(Ctx* c) { SAME(same_rows(st.psc, 32, ARG(1), ARG(2), ARG(3))); consts(st.psc, 32, ARG(1), ARG(2), ARG(3), 0); c->eax = 0; }
static void dev_GetPixelShaderConstantF(Ctx* c) { consts(st.psc, 32, ARG(1), ARG(2), ARG(3), 1); c->eax = 0; }
static void dev_SetVertexShaderConstantI(Ctx* c) { consts(st.vsi, 16, ARG(1), ARG(2), ARG(3), 0); c->eax = 0; }
static void dev_GetVertexShaderConstantI(Ctx* c) { consts(st.vsi, 16, ARG(1), ARG(2), ARG(3), 1); c->eax = 0; }
static void dev_SetPixelShaderConstantI(Ctx* c) { consts(st.psi, 16, ARG(1), ARG(2), ARG(3), 0); c->eax = 0; }
static void dev_GetPixelShaderConstantI(Ctx* c) { consts(st.psi, 16, ARG(1), ARG(2), ARG(3), 1); c->eax = 0; }
static void bools(uint32_t* tab, uint32_t start, uint32_t data, uint32_t count, int get) {
    for (uint32_t i = 0; i < count && start + i < 16; i++) get ? ST32(data + 4 * i, tab[start + i]) : (void)(tab[start + i] = LD32(data + 4 * i), st_dirty(&tab[start + i], 4));
}
static void dev_SetVertexShaderConstantB(Ctx* c) { bools(st.vsb, ARG(1), ARG(2), ARG(3), 0); c->eax = 0; }
static void dev_GetVertexShaderConstantB(Ctx* c) { bools(st.vsb, ARG(1), ARG(2), ARG(3), 1); c->eax = 0; }
static void dev_SetPixelShaderConstantB(Ctx* c) { bools(st.psb, ARG(1), ARG(2), ARG(3), 0); c->eax = 0; }
static void dev_GetPixelShaderConstantB(Ctx* c) { bools(st.psb, ARG(1), ARG(2), ARG(3), 1); c->eax = 0; }
/* D3D9: setting render target 0 also resets the viewport and the scissor rect to the whole new target. Without that the
   minimap's scissor (515,286-639,362) stayed on for the HUD after it, and the purple hint boxes ("AREA UNAVAILABLE",
   "NEW CITY GOALS!") were drawn clipped to the minimap: invisible */
static void dev_SetRenderTarget(Ctx* c) {
    uint32_t i = IDX(ARG(1), 4), s = ARG(2);
    st.rt[i] = s, ST_DIRTY(st.rt);
    if (i == 0 && s) {
        Res* r = res(s);
        float zn = 0, zf = 1;
        st.viewport[0] = st.viewport[1] = 0, st.viewport[2] = r->w, st.viewport[3] = r->h;
        memcpy(&st.viewport[4], &zn, 4), memcpy(&st.viewport[5], &zf, 4), ST_DIRTY(st.viewport);
        st.scissor[0] = st.scissor[1] = 0, st.scissor[2] = r->w, st.scissor[3] = r->h, ST_DIRTY(st.scissor);
    }
    c->eax = 0;
}
static void dev_GetRenderTarget2(Ctx* c) { uint32_t s = st.rt[IDX(ARG(1), 4)]; ST32(ARG(2), s ? s : ARG(1) ? 0 : g_backbuffer); c->eax = s || !ARG(1) ? 0 : 0x8876086cu; }
static void dev_SetDepthStencilSurface(Ctx* c) { st.ds = ARG(1), ST_DIRTY(st.ds); c->eax = 0; }
static void dev_GetDepthStencilSurface2(Ctx* c) { ST32(ARG(1), st.ds ? st.ds : g_depth); c->eax = 0; }
static void dev_SetCurrentTexturePalette(Ctx* c) { st.palette = ARG(1), ST_DIRTY(st.palette); c->eax = 0; }
static void dev_GetCurrentTexturePalette(Ctx* c) { ST32(ARG(1), st.palette); c->eax = 0; }
static void dev_SetSoftwareVertexProcessing(Ctx* c) { st.swvp = ARG(1), ST_DIRTY(st.swvp); c->eax = 0; }
static void dev_GetSoftwareVertexProcessing(Ctx* c) { c->eax = st.swvp; }
static void dev_SetNPatchMode(Ctx* c) { st.npatch = ARG(1), ST_DIRTY(st.npatch); c->eax = 0; }
static void dev_GetNPatchMode(Ctx* c) { f80 v; float f; memcpy(&f, &st.npatch, 4); v = f; FPUSH(v); }  /* FLOAT: st0 */
static void dev_GetDeviceCaps(Ctx* c);
static void dev_GetCreationParameters(Ctx* c) { uint32_t v[4] = {0, 1, 0x2000, 0x40}; memcpy(H(ARG(1)), v, 16); c->eax = 0; }
static void dev_GetNumberOfSwapChains(Ctx* c) { c->eax = 1; }
static void dev_GetRasterStatus(Ctx* c) { ST32(ARG(2), 1), ST32(ARG(2) + 4, 0); c->eax = 0; }  /* in vblank: waits end */
static void dev_GetGammaRamp(Ctx* c) {
    for (int ch = 0; ch < 3; ch++)
        for (int i = 0; i < 256; i++) ST16(ARG(2) + 512 * ch + 2 * i, (uint16_t)(i * 257));
}
static void dev_ValidateDevice(Ctx* c) { ST32(ARG(1), 1); c->eax = 0; }
static void dev_TestCooperativeLevel(Ctx* c) { c->eax = 0; }
static void dev_GetPaletteEntries(Ctx* c) { memset(H(ARG(2)), 0xff, 1024); c->eax = 0; }

/* every resource: GetDevice and GetType (D3DRESOURCETYPE) */
static void res_GetDevice(Ctx* c) { ST32(ARG(1), g_device); c->eax = 0; }
static void type_surface(Ctx* c) { c->eax = 1; }
static void type_texture(Ctx* c) { c->eax = 3; }
static void type_cube(Ctx* c) { c->eax = 5; }
static void type_vb(Ctx* c) { c->eax = 6; }
static void type_ib(Ctx* c) { c->eax = 7; }
static void surf_GetContainer(Ctx* c) { ST32(ARG(2), g_device); c->eax = 0; }
/* queries: events are always done; occlusion queries report the object as visible */
static void query_GetData(Ctx* c) {  /* (data, size, flags) */
    if (ARG(1) && ARG(2) >= 4) ST32(ARG(1), 1000);
    if (ARG(1) && ARG(2) == 1) ST8(ARG(1), 1);
    c->eax = 0;
}
static void query_GetDataSize(Ctx* c) { c->eax = 4; }
static void query_GetType(Ctx* c) { c->eax = LD32(ARG(0) + 8); }
static void swap_GetDisplayMode(Ctx* c) { uint32_t m[4] = {W, HGT, 60, FMT_X8R8G8B8}; memcpy(H(ARG(1)), m, 16); c->eax = 0; }
static void res_GetPrivateData(Ctx* c) { c->eax = 0x88760866u; }  /* D3DERR_NOTFOUND: the game never stored any */
static void res_Zero(Ctx* c) { c->eax = 0; }                         /* GetPriority, GetLOD */
static void tex_GetAutoGenFilterType(Ctx* c) { c->eax = 2; }         /* D3DTEXF_LINEAR */
static void surf_GetDC(Ctx* c) { c->eax = 0x8876086cu; }             /* D3DERR_INVALIDCALL: no GDI */
static void swap_GetBackBuffer(Ctx* c) { ST32(ARG(3), g_backbuffer); c->eax = 0; }
static void swap_GetPresentParameters(Ctx* c) { memset(H(ARG(1)), 0, 56); ST32(ARG(1), W), ST32(ARG(1) + 4, HGT), ST32(ARG(1) + 8, FMT_X8R8G8B8); c->eax = 0; }
static void swap_GetRasterStatus(Ctx* c) { ST32(ARG(1), 1), ST32(ARG(1) + 4, 0); c->eax = 0; }

static const ShimDef dev_state_impl[] = {
    {"SetRenderState", dev_SetRenderState}, {"GetRenderState", dev_GetRenderState},
    {"SetTextureStageState", dev_SetTextureStageState}, {"GetTextureStageState", dev_GetTextureStageState},
    {"SetSamplerState", dev_SetSamplerState}, {"GetSamplerState", dev_GetSamplerState},
    {"SetTexture", dev_SetTexture}, {"GetTexture", dev_GetTexture}, {"SetTransform", dev_SetTransform},
    {"GetTransform", dev_GetTransform}, {"MultiplyTransform", dev_MultiplyTransform}, {"SetViewport", dev_SetViewport},
    {"GetViewport", dev_GetViewport}, {"SetMaterial", dev_SetMaterial}, {"GetMaterial", dev_GetMaterial},
    {"SetLight", dev_SetLight}, {"GetLight", dev_GetLight}, {"LightEnable", dev_LightEnable},
    {"GetLightEnable", dev_GetLightEnable}, {"SetClipPlane", dev_SetClipPlane}, {"GetClipPlane", dev_GetClipPlane},
    {"SetClipStatus", dev_SetClipStatus}, {"GetClipStatus", dev_GetClipStatus}, {"SetScissorRect", dev_SetScissorRect},
    {"GetScissorRect", dev_GetScissorRect}, {"SetStreamSource", dev_SetStreamSource},
    {"GetStreamSource", dev_GetStreamSource}, {"SetStreamSourceFreq", dev_SetStreamSourceFreq},
    {"GetStreamSourceFreq", dev_GetStreamSourceFreq}, {"SetIndices", dev_SetIndices}, {"GetIndices", dev_GetIndices},
    {"SetVertexShader", dev_SetVertexShader}, {"GetVertexShader", dev_GetVertexShader},
    {"SetPixelShader", dev_SetPixelShader}, {"GetPixelShader", dev_GetPixelShader},
    {"SetVertexDeclaration", dev_SetVertexDeclaration}, {"GetVertexDeclaration", dev_GetVertexDeclaration},
    {"GetFVF", dev_GetFVF}, {"SetVertexShaderConstantF", dev_SetVertexShaderConstantF},
    {"GetVertexShaderConstantF", dev_GetVertexShaderConstantF}, {"SetPixelShaderConstantF", dev_SetPixelShaderConstantF},
    {"GetPixelShaderConstantF", dev_GetPixelShaderConstantF}, {"SetVertexShaderConstantI", dev_SetVertexShaderConstantI},
    {"GetVertexShaderConstantI", dev_GetVertexShaderConstantI}, {"SetPixelShaderConstantI", dev_SetPixelShaderConstantI},
    {"GetPixelShaderConstantI", dev_GetPixelShaderConstantI}, {"SetVertexShaderConstantB", dev_SetVertexShaderConstantB},
    {"GetVertexShaderConstantB", dev_GetVertexShaderConstantB}, {"SetPixelShaderConstantB", dev_SetPixelShaderConstantB},
    {"GetPixelShaderConstantB", dev_GetPixelShaderConstantB}, {"SetRenderTarget", dev_SetRenderTarget},
    {"GetRenderTarget", dev_GetRenderTarget2}, {"SetDepthStencilSurface", dev_SetDepthStencilSurface},
    {"GetDepthStencilSurface", dev_GetDepthStencilSurface2}, {"SetCurrentTexturePalette", dev_SetCurrentTexturePalette},
    {"GetCurrentTexturePalette", dev_GetCurrentTexturePalette},
    {"SetSoftwareVertexProcessing", dev_SetSoftwareVertexProcessing},
    {"GetSoftwareVertexProcessing", dev_GetSoftwareVertexProcessing}, {"SetNPatchMode", dev_SetNPatchMode},
    {"GetNPatchMode", dev_GetNPatchMode}, {"GetDeviceCaps", dev_GetDeviceCaps},
    {"GetCreationParameters", dev_GetCreationParameters}, {"GetNumberOfSwapChains", dev_GetNumberOfSwapChains},
    {"GetRasterStatus", dev_GetRasterStatus}, {"GetGammaRamp", dev_GetGammaRamp}, {"ValidateDevice", dev_ValidateDevice},
    {"TestCooperativeLevel", dev_TestCooperativeLevel}, {"GetPaletteEntries", dev_GetPaletteEntries},
    {NULL, NULL},
};

static const ShimDef dev_impl[] = {
    {"CreateTexture", dev_CreateTexture}, {"CreateCubeTexture", dev_CreateCubeTexture},
    {"CreateVolumeTexture", dev_CreateVolumeTexture}, {"CreateVertexBuffer", dev_CreateVertexBuffer},
    {"CreateIndexBuffer", dev_CreateIndexBuffer}, {"CreateRenderTarget", dev_CreateRenderTarget},
    {"CreateDepthStencilSurface", dev_CreateDepthStencilSurface},
    {"CreateOffscreenPlainSurface", dev_CreateOffscreenPlainSurface}, {"GetBackBuffer", dev_GetBackBuffer},
    {"GetRenderTarget", dev_GetRenderTarget}, {"GetDepthStencilSurface", dev_GetDepthStencilSurface},
    {"CreateVertexShader", dev_CreateVertexShader}, {"CreatePixelShader", dev_CreatePixelShader},
    {"CreateVertexDeclaration", dev_CreateVertexDeclaration}, {"CreateQuery", dev_CreateQuery},
    {"CreateStateBlock", dev_CreateStateBlock}, {"EndStateBlock", dev_EndStateBlock},
    {"GetSwapChain", dev_GetSwapChain}, {"GetDirect3D", dev_GetDirect3D},
    {"GetAvailableTextureMem", dev_GetAvailableTextureMem}, {"GetDisplayMode", dev_GetDisplayMode},
    {"Present", dev_Present}, {"SetFVF", dev_SetFVF}, {"DrawPrimitive", dev_DrawPrimitive},
    {"DrawIndexedPrimitive", dev_DrawIndexedPrimitive}, {"DrawPrimitiveUP", dev_DrawPrimitiveUP},
    {"DrawIndexedPrimitiveUP", dev_DrawIndexedPrimitiveUP}, {"Clear", dev_Clear}, {"BeginScene", res_Unlock},
    {"EndScene", res_Unlock}, {"CreateAdditionalSwapChain", dev_CreateAdditionalSwapChain},
    {"UpdateTexture", dev_UpdateTexture}, {"GetRenderTargetData", res_Unlock}, {"GetFrontBufferData", res_Unlock}, {NULL, NULL},
};

/* ---- IDirect3D9 ---- */
static void d3d_GetAdapterCount(Ctx* c) { c->eax = 1; }
static void d3d_GetAdapterIdentifier(Ctx* c) {  /* D3DADAPTER_IDENTIFIER9, 1100 bytes */
    uint8_t* p = H(ARG(3));
    memset(p, 0, 1100);
    strcpy((char*)p, "vitagl.dll");
    strcpy((char*)p + 512, "PS Vita SGX543MP4+");
    strcpy((char*)p + 1024, "\\\\.\\DISPLAY1");
    ST32(ARG(3) + 1064, 0x10de);  /* VendorId: NVIDIA, the vendor the game knows best */
    ST32(ARG(3) + 1068, 0x0322);
    c->eax = 0;
}
/* the options screen looks the running mode (800x600) up in this list, so it needs the usual PC modes */
uint32_t g_mode_extra[2];  /* resolution.txt's size (shims_misc.c), listed last */
static const uint32_t MODES[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 960}, {960, 544}, {720, 408}, {640, 362}, {480, 272}};
#define NMODES (sizeof MODES / sizeof *MODES + (g_mode_extra[0] != 0))
static void d3d_GetAdapterModeCount(Ctx* c) { c->eax = NMODES; }
static void d3d_EnumAdapterModes(Ctx* c) {  /* (adapter, format, mode, D3DDISPLAYMODE*) */
    if (ARG(3) >= NMODES) { c->eax = 0x8876086cu; return; }  /* D3DERR_INVALIDCALL */
    const uint32_t* wh = ARG(3) < sizeof MODES / sizeof *MODES ? MODES[ARG(3)] : g_mode_extra;
    uint32_t m[4] = {wh[0], wh[1], 60, ARG(2)};
    memcpy(H(ARG(4)), m, 16);
    c->eax = 0;
}
static void d3d_GetAdapterDisplayMode(Ctx* c) { uint32_t m[4] = {W, HGT, 60, FMT_X8R8G8B8}; memcpy(H(ARG(2)), m, 16); c->eax = 0; }
static void fill_caps(uint32_t p);
static void d3d_GetDeviceCaps(Ctx* c) { fill_caps(ARG(3)); c->eax = 0; }  /* (adapter, type, caps) */
static void dev_GetDeviceCaps(Ctx* c) { fill_caps(ARG(1)); c->eax = 0; }  /* (caps) */
static void fill_caps(uint32_t p) {  /* D3DCAPS9, 304 bytes: a GeForce 3/4-class profile */
    for (uint32_t o = 0; o < 304; o += 4) ST32(p + o, 0xffffffffu);
    uint32_t num[][2] = {{0, 1}, {4, 0}, {88, 2048}, {92, 2048}, {96, 256}, {100, 8192}, {104, 2048}, {108, 16},
                         {148, 8}, {152, 4}, {160, 8}, {164, 6}, {168, 4}, {172, 0}, {180, 0xfffff}, {184, 0xfffff},
                         {188, 16}, {192, 255}, {196, 0xfffe0101u}, {200, 96}, {204, 0xffff0104u}, {220, 0},
                         {224, 0}, {228, 0}, {232, 1}, {240, 1}, {248, 0}, {252, 0}, {256, 12}, {260, 0}, {264, 0},
                         {268, 0}, {272, 12}, {276, 0}, {280, 96}, {288, 0}, {292, 0}, {296, 0}, {300, 0}};
    for (unsigned i = 0; i < sizeof num / sizeof *num; i++) ST32(p + num[i][0], num[i][1]);
    STF32(p + 112, 1e10f), STF32(p + 116, -1e8f), STF32(p + 120, -1e8f), STF32(p + 124, 1e8f), STF32(p + 128, 1e8f);
    STF32(p + 132, 0.f), STF32(p + 176, 64.f), STF32(p + 208, 8.f), STF32(p + 216, 0.f);
    /* "all bits" would also claim the restrictions: pow2/square-only textures, a null reference device */
    ST32(p + 60, 0x34c05u);                 /* TextureCaps: perspective, alpha, projected, cube(+mip, pow2), mipmap */
    ST32(p + 32, 0xffffefffu);              /* PrimitiveMiscCaps without NULLREFERENCE */
    ST32(p + 28, 0xffffbfffu);              /* DevCaps without SEPARATETEXTUREMEMORIES */
    ST32(p + 140, 0x00080008u);             /* FVFCaps: 8 texture coordinates, point size */
    ST32(p + 204, 0xffff0101u);             /* ps_1_1: the GeForce 3/4 path, the smallest shader set to translate */
}
/* the formats the vitaGL renderer will support; anything else is D3DERR_NOTAVAILABLE so the game never picks it */
static void d3d_CheckDeviceFormat(Ctx* c) {  /* (adapter, type, adapterFormat, usage, rtype, format) */
    uint32_t usage = ARG(4), f = ARG(6);
    int color = f == 21 || f == 22 || f == 23, depth = f == 75 || f == 77 || f == 80;
    int tex = color || f == 24 || f == 25 || f == 26 || f == 28 || f == 50 || f == 51 || is_dxt(f);
    int ok = (usage & 1) ? color : (usage & 2) ? depth : tex || depth;  /* RENDERTARGET, DEPTHSTENCIL, plain */
    c->eax = ok ? 0 : 0x8876086au;
}
static void d3d_CheckDeviceMultiSampleType(Ctx* c) {  /* (adapter, type, format, windowed, mstype, quality*) */
    if (ARG(6)) ST32(ARG(6), 1);
    c->eax = ARG(5) == 0 ? 0 : 0x8876086au;
}
static void d3d_CreateDevice(Ctx* c) {  /* (adapter, type, hwnd, flags, pp, ppDevice) */
    uint32_t pp = ARG(5);
    vlog("CreateDevice %ux%u format %u, flags %x\n", LD32(pp), LD32(pp + 4), LD32(pp + 8), ARG(4));
    if (!LD32(pp)) ST32(pp, W), ST32(pp + 4, HGT);
    g_bbw = LD32(pp), g_bbh = LD32(pp + 4);
    g_backbuffer = new_surface(LD32(pp), LD32(pp + 4), FMT_X8R8G8B8);
    g_depth = new_surface(LD32(pp), LD32(pp + 4), FMT_D24S8);
    state_defaults();
    ST_DIRTY(st);
    gl_init(LD32(pp), LD32(pp + 4));
    if (!g_device) g_device = com_new(vt_dev, 16);
    ST32(ARG(6), g_device);
    c->eax = 0;
}
static void d3d_GetAdapterMonitor(Ctx* c) { c->eax = 0x3000; }
static void d3d_CheckDeviceType(Ctx* c) { c->eax = 0; }  /* (adapter, type, display, backbuffer, windowed): any */
static const ShimDef d3d_impl[] = {
    {"GetAdapterCount", d3d_GetAdapterCount}, {"GetAdapterIdentifier", d3d_GetAdapterIdentifier},
    {"GetAdapterModeCount", d3d_GetAdapterModeCount}, {"EnumAdapterModes", d3d_EnumAdapterModes},
    {"GetAdapterDisplayMode", d3d_GetAdapterDisplayMode}, {"GetDeviceCaps", d3d_GetDeviceCaps},
    {"CheckDeviceFormat", d3d_CheckDeviceFormat}, {"CheckDeviceMultiSampleType", d3d_CheckDeviceMultiSampleType},
    {"CreateDevice", d3d_CreateDevice}, {"GetAdapterMonitor", d3d_GetAdapterMonitor},
    {"CheckDeviceType", d3d_CheckDeviceType}, {NULL, NULL},
};

/* GetDevice/GetType for each resource kind, and the query and swap chain methods that return data */
#define RES_COMMON {"GetDevice", res_GetDevice}, {"GetPrivateData", res_GetPrivateData}, {"GetPriority", res_Zero}
#define TEX_COMMON RES_COMMON, {"GetLOD", res_Zero}, {"GetAutoGenFilterType", tex_GetAutoGenFilterType}
static const ShimDef surf_more[] = {RES_COMMON, {"GetType", type_surface}, {"GetContainer", surf_GetContainer}, {"GetDC", surf_GetDC}, {NULL, NULL}};
static const ShimDef tex_more[] = {TEX_COMMON, {"GetType", type_texture}, {"Release", res_Release}, {NULL, NULL}};
static const ShimDef cube_more[] = {TEX_COMMON, {"GetType", type_cube}, {"Release", res_Release}, {NULL, NULL}};
static const ShimDef vb_more[] = {RES_COMMON, {"GetType", type_vb}, {"Release", res_Release}, {NULL, NULL}};
static const ShimDef ib_more[] = {RES_COMMON, {"GetType", type_ib}, {"Release", res_Release}, {NULL, NULL}};
static const ShimDef dev_child[] = {{"GetDevice", res_GetDevice}, {NULL, NULL}};
static const ShimDef sb_impl[] = {{"GetDevice", res_GetDevice}, {"Capture", sb_Capture}, {"Apply", sb_Apply},
                                  {"Release", sb_Release}, {NULL, NULL}};
static const ShimDef shader_impl[] = {{"GetDevice", res_GetDevice}, {"GetFunction", shader_GetFunction}, {NULL, NULL}};
static const ShimDef decl_impl[] = {{"GetDevice", res_GetDevice}, {"GetDeclaration", decl_GetDeclaration}, {NULL, NULL}};
static const ShimDef query_impl[] = {{"GetData", query_GetData}, {"GetDataSize", query_GetDataSize}, {"GetType", query_GetType},
                                     {"GetDevice", res_GetDevice}, {NULL, NULL}};
static const ShimDef swap_impl[] = {{"GetBackBuffer", swap_GetBackBuffer}, {"GetPresentParameters", swap_GetPresentParameters},
                                    {"GetRasterStatus", swap_GetRasterStatus}, {"GetDisplayMode", swap_GetDisplayMode},
                                    {"GetFrontBufferData", res_Zero}, {"GetDevice", res_GetDevice}, {NULL, NULL}};

static void Direct3DCreate9_(Ctx* c) {
    if (!vt_d3d) {
        vt_d3d = com_vtable("IDirect3D9", d3d_impl);
        vt_dev = com_vtable_ext("IDirect3DDevice9", dev_impl, dev_state_impl);
        vt_tex = com_vtable_ext("IDirect3DTexture9", tex_impl, tex_more);
        vt_cube = com_vtable_ext("IDirect3DCubeTexture9", cube_impl, cube_more);
        vt_surf = com_vtable_ext("IDirect3DSurface9", surf_impl, surf_more);
        vt_vb = com_vtable_ext("IDirect3DVertexBuffer9", buf_impl, vb_more);
        vt_ib = com_vtable_ext("IDirect3DIndexBuffer9", buf_impl, ib_more);
        vt_vs = com_vtable("IDirect3DVertexShader9", shader_impl);
        vt_ps = com_vtable("IDirect3DPixelShader9", shader_impl);
        vt_decl = com_vtable("IDirect3DVertexDeclaration9", decl_impl);
        vt_query = com_vtable("IDirect3DQuery9", query_impl);
        vt_sb = com_vtable("IDirect3DStateBlock9", sb_impl);
        vt_swap = com_vtable("IDirect3DSwapChain9", swap_impl);
    }
    vlog("Direct3DCreate9(%u)\n", ARG(0));
    c->eax = com_new(vt_d3d, 16);
}

const ShimDef shims_d3d9[] = {{"Direct3DCreate9", Direct3DCreate9_}, {NULL, NULL}};
