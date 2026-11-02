// check src/recomp/mmx.h against the real MMX instructions, on random inputs and shift counts:
// clang -O1 -mmmx -Isrc/recomp tools/recomp/mmx_test.c -o mmx_test && mmx_test
#include <mmintrin.h>
#include <stdio.h>
#include <stdlib.h>

#include "mmx.h"

static uint64_t rnd(void) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r = r << 8 | (uint64_t)(rand() & 255);
    switch (rand() % 4) {  /* lanes at the edges, where saturation and signs matter */
    case 0: r |= 0x8080808080808080ull; break;
    case 1: r &= 0x7f7f7f7f7f7f7f7full; break;
    }
    return r;
}
static __m64 M(uint64_t v) { return _mm_cvtsi64_m64((long long)v); }
static uint64_t U(__m64 v) { return (uint64_t)_mm_cvtm64_si64(v); }

int main(void) {
    int bad = 0;
#define T(fn, ref)                                                                                       \
    for (int i = 0; i < 20000; i++) {                                                                    \
        uint64_t a = rnd(), b = rnd();                                                                   \
        uint64_t want = U(ref(M(a), M(b))), got = fn(a, b);                                              \
        if (want != got && bad++ < 20) printf(#fn " %016llx %016llx: %016llx, want %016llx\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)got, (unsigned long long)want); \
    }
#define S(fn, ref)                                                                                       \
    for (int i = 0; i < 20000; i++) {                                                                    \
        uint64_t a = rnd(), b = (uint64_t)(rand() % 70);                                                 \
        if (i % 50 == 0) b = rnd();                                                                      \
        uint64_t want = U(ref(M(a), M(b))), got = fn(a, b);                                              \
        if (want != got && bad++ < 20) printf(#fn " %016llx by %llu: %016llx, want %016llx\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)got, (unsigned long long)want); \
    }
    T(mmx_paddb, _mm_add_pi8) T(mmx_paddw, _mm_add_pi16) T(mmx_paddd, _mm_add_pi32)
    T(mmx_psubb, _mm_sub_pi8) T(mmx_psubw, _mm_sub_pi16) T(mmx_psubd, _mm_sub_pi32)
    T(mmx_paddsb, _mm_adds_pi8) T(mmx_paddsw, _mm_adds_pi16) T(mmx_psubsb, _mm_subs_pi8) T(mmx_psubsw, _mm_subs_pi16)
    T(mmx_paddusb, _mm_adds_pu8) T(mmx_paddusw, _mm_adds_pu16) T(mmx_psubusb, _mm_subs_pu8) T(mmx_psubusw, _mm_subs_pu16)
    T(mmx_pmullw, _mm_mullo_pi16) T(mmx_pmulhw, _mm_mulhi_pi16) T(mmx_pmaddwd, _mm_madd_pi16)
    T(mmx_pcmpeqb, _mm_cmpeq_pi8) T(mmx_pcmpeqw, _mm_cmpeq_pi16) T(mmx_pcmpeqd, _mm_cmpeq_pi32)
    T(mmx_pcmpgtb, _mm_cmpgt_pi8) T(mmx_pcmpgtw, _mm_cmpgt_pi16) T(mmx_pcmpgtd, _mm_cmpgt_pi32)
    T(mmx_pand, _mm_and_si64) T(mmx_pandn, _mm_andnot_si64) T(mmx_por, _mm_or_si64) T(mmx_pxor, _mm_xor_si64)
    T(mmx_punpcklbw, _mm_unpacklo_pi8) T(mmx_punpcklwd, _mm_unpacklo_pi16) T(mmx_punpckldq, _mm_unpacklo_pi32)
    T(mmx_punpckhbw, _mm_unpackhi_pi8) T(mmx_punpckhwd, _mm_unpackhi_pi16) T(mmx_punpckhdq, _mm_unpackhi_pi32)
    T(mmx_packsswb, _mm_packs_pi16) T(mmx_packuswb, _mm_packs_pu16) T(mmx_packssdw, _mm_packs_pi32)
    S(mmx_psllw, _mm_sll_pi16) S(mmx_pslld, _mm_sll_pi32) S(mmx_psllq, _mm_sll_si64)
    S(mmx_psrlw, _mm_srl_pi16) S(mmx_psrld, _mm_srl_pi32) S(mmx_psrlq, _mm_srl_si64)
    S(mmx_psraw, _mm_sra_pi16) S(mmx_psrad, _mm_sra_pi32)
    printf("%d failed\n", bad);
    return bad != 0;
}
