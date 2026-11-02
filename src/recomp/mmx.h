/* MMX in portable C: each mm register is a uint64_t (Ctx.mm), each op works lane by lane.
   Checked against the real instructions by tools/recomp/mmx_test.c. */
#pragma once
#include <stdint.h>

static inline int64_t mmx_sat(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* name(a, b): lanes of W bits, x and y the signed lanes of a and b, ux and uy unsigned */
#define MMX_OP(name, W, expr)                                                         \
    static inline uint64_t name(uint64_t a, uint64_t b) {                             \
        uint64_t r = 0, m = W == 64 ? ~0ull : (1ull << W) - 1;                         \
        for (int i = 0; i < 64; i += W) {                                             \
            uint64_t ux = a >> i & m, uy = b >> i & m;                                \
            int64_t x = (int64_t)(ux << (64 - W)) >> (64 - W), y = (int64_t)(uy << (64 - W)) >> (64 - W); \
            (void)x, (void)y, (void)ux, (void)uy;                                     \
            r |= ((uint64_t)(expr) & m) << i;                                         \
        }                                                                             \
        return r;                                                                     \
    }
MMX_OP(mmx_paddb, 8, ux + uy)
MMX_OP(mmx_paddw, 16, ux + uy)
MMX_OP(mmx_paddd, 32, ux + uy)
MMX_OP(mmx_psubb, 8, ux - uy)
MMX_OP(mmx_psubw, 16, ux - uy)
MMX_OP(mmx_psubd, 32, ux - uy)
MMX_OP(mmx_paddsb, 8, mmx_sat(x + y, -128, 127))
MMX_OP(mmx_paddsw, 16, mmx_sat(x + y, -32768, 32767))
MMX_OP(mmx_psubsb, 8, mmx_sat(x - y, -128, 127))
MMX_OP(mmx_psubsw, 16, mmx_sat(x - y, -32768, 32767))
MMX_OP(mmx_paddusb, 8, mmx_sat((int64_t)(ux + uy), 0, 255))
MMX_OP(mmx_paddusw, 16, mmx_sat((int64_t)(ux + uy), 0, 65535))
MMX_OP(mmx_psubusb, 8, mmx_sat((int64_t)ux - (int64_t)uy, 0, 255))
MMX_OP(mmx_psubusw, 16, mmx_sat((int64_t)ux - (int64_t)uy, 0, 65535))
MMX_OP(mmx_pmullw, 16, x * y)
MMX_OP(mmx_pmulhw, 16, (x * y) >> 16)
MMX_OP(mmx_pcmpeqb, 8, ux == uy ? ~0ull : 0)
MMX_OP(mmx_pcmpeqw, 16, ux == uy ? ~0ull : 0)
MMX_OP(mmx_pcmpeqd, 32, ux == uy ? ~0ull : 0)
MMX_OP(mmx_pcmpgtb, 8, x > y ? ~0ull : 0)
MMX_OP(mmx_pcmpgtw, 16, x > y ? ~0ull : 0)
MMX_OP(mmx_pcmpgtd, 32, x > y ? ~0ull : 0)
/* shifts: b is the count; past the lane width logical shifts give 0, arithmetic ones the sign */
MMX_OP(mmx_psllw, 16, b > 15 ? 0 : ux << b)
MMX_OP(mmx_pslld, 32, b > 31 ? 0 : ux << b)
MMX_OP(mmx_psllq, 64, b > 63 ? 0 : ux << b)
MMX_OP(mmx_psrlw, 16, b > 15 ? 0 : ux >> b)
MMX_OP(mmx_psrld, 32, b > 31 ? 0 : ux >> b)
MMX_OP(mmx_psrlq, 64, b > 63 ? 0 : ux >> b)
MMX_OP(mmx_psraw, 16, x >> (b > 15 ? 15 : b))
MMX_OP(mmx_psrad, 32, x >> (b > 31 ? 31 : b))
#undef MMX_OP

static inline uint64_t mmx_pand(uint64_t a, uint64_t b) { return a & b; }
static inline uint64_t mmx_pandn(uint64_t a, uint64_t b) { return ~a & b; }
static inline uint64_t mmx_por(uint64_t a, uint64_t b) { return a | b; }
static inline uint64_t mmx_pxor(uint64_t a, uint64_t b) { return a ^ b; }

static inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 2; i++) {
        int32_t s = 0;
        for (int j = 0; j < 2; j++) s += (int32_t)(int16_t)(a >> (32 * i + 16 * j)) * (int16_t)(b >> (32 * i + 16 * j));
        r |= (uint64_t)(uint32_t)s << 32 * i;
    }
    return r;
}
/* unpack: interleave the low (or high) halves of a and b, a's lane first */
static inline uint64_t mmx_unpack(uint64_t a, uint64_t b, int w, int high) {
    uint64_t r = 0, m = (1ull << w) - 1;
    for (int i = 0, n = 32 / w; i < n; i++) {
        int s = (high ? 32 : 0) + i * w;
        r |= (a >> s & m) << (2 * i * w) | (b >> s & m) << ((2 * i + 1) * w);
    }
    return r;
}
static inline uint64_t mmx_punpcklbw(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 8, 0); }
static inline uint64_t mmx_punpcklwd(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 16, 0); }
static inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 32, 0); }
static inline uint64_t mmx_punpckhbw(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 8, 1); }
static inline uint64_t mmx_punpckhwd(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 16, 1); }
static inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b) { return mmx_unpack(a, b, 32, 1); }
/* pack: the wide signed lanes of a then b, saturated to half width */
static inline uint64_t mmx_pack(uint64_t a, uint64_t b, int w, int64_t lo, int64_t hi) {
    uint64_t r = 0, hm = (1ull << (w / 2)) - 1;
    for (int i = 0, n = 64 / w; i < 2 * n; i++) {
        uint64_t s = i < n ? a : b;
        int k = i % n;
        int64_t v = (int64_t)((s >> (k * w)) << (64 - w)) >> (64 - w);
        r |= ((uint64_t)mmx_sat(v, lo, hi) & hm) << (i * w / 2);
    }
    return r;
}
static inline uint64_t mmx_packsswb(uint64_t a, uint64_t b) { return mmx_pack(a, b, 16, -128, 127); }
static inline uint64_t mmx_packuswb(uint64_t a, uint64_t b) { return mmx_pack(a, b, 16, 0, 255); }
static inline uint64_t mmx_packssdw(uint64_t a, uint64_t b) { return mmx_pack(a, b, 32, -32768, 32767); }
