/* x87 helpers without x87 (RC_PORTABLE, see rt.h): the Vita build and the PC build that rehearses it.
   Registers are doubles. Rounding control from the game's control word goes to the host FPU with fesetround, so
   fist/frndint and the arithmetic itself round the way the game asked; precision control is not applied (53-bit). */
#include <fenv.h>
#include <math.h>

#include "rt.h"

#ifdef RC_PORTABLE

double rc_ld80(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, b, 8);
    memcpy(&se, b + 8, 2);
    int e = se & 0x7fff;
    double s = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0x7fff) return (mant << 1) ? NAN : s * INFINITY;
    /* two exact signed parts: their sum is the one rounding to 53 bits, in the current mode and with the sign
       (rounding the magnitude first gets directed modes wrong for negatives); ldexp does overflow and denormals.
       shortcut: a result below 2^-1022 (double denormal) is rounded twice and can be 1 ulp off; fix if one matters */
    double r = s * (double)(mant >> 11) * 2048.0 + s * (double)(mant & 0x7ff);
    return ldexp(r, (e ? e : 1) - 16383 - 63);
}

void rc_st80(void* p, double v) {
    uint8_t* b = (uint8_t*)p;
    uint64_t mant = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) mant = 0xc000000000000000ull, se |= 0x7fff;
    else if (isinf(v)) mant = 0x8000000000000000ull, se |= 0x7fff;
    else if (v != 0) {
        int e;
        double m = frexp(fabs(v), &e);  /* [0.5, 1): every double, denormals too, is normal in 80 bits */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(b, &mant, 8);
    memcpy(b + 8, &se, 2);
}

void rc_fldcw(Ctx* c, uint16_t cw) {
    static const int mode[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    c->fcw = cw;
    fesetround(mode[(cw >> 10) & 3]);
    /* every distinct control word, once: precision control 24-bit ((cw >> 8 & 3) == 0) is not applied here */
    static uint16_t seen[32];
    static unsigned nseen;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == cw) return;
    if (nseen < 32) seen[nseen++] = cw, rc_log("fldcw %04x (rounding %u, precision %u)\n", cw, (cw >> 10) & 3, (cw >> 8) & 3);
}

/* out of range or NaN stores the "integer indefinite" value, like the x87 with the invalid exception masked */
int16_t rc_fist16(f80 v) { double r = rint(v); return r >= -32768.0 && r <= 32767.0 ? (int16_t)r : (int16_t)0x8000; }
int32_t rc_fist32(f80 v) { double r = rint(v); return r >= -2147483648.0 && r <= 2147483647.0 ? (int32_t)r : (int32_t)0x80000000; }
int64_t rc_fist64(f80 v) { double r = rint(v); return r >= -0x1p63 && r < 0x1p63 ? (int64_t)r : (int64_t)0x8000000000000000ull; }
f80 rc_frndint(f80 v) { return rint(v); }
f80 rc_fsin(f80 v) { return sin(v); }
f80 rc_fcos(f80 v) { return cos(v); }
f80 rc_fsqrt(f80 v) { return sqrt(v); }
f80 rc_fptan(f80 v) { return tan(v); }
f80 rc_fpatan(f80 y, f80 x) { return atan2(y, x); }
f80 rc_fyl2x(f80 y, f80 x) { return y * log2(x); }
f80 rc_fconst(int which) {
    static const double k[7] = {1.0, 0.0, 3.14159265358979323846, 1.44269504088896340736, 3.32192809488736234787,
                                0.301029995663981195214, 0.693147180559945309417};
    return k[which < 0 || which > 6 ? 6 : which];
}

/* a plain Pentium Pro-class CPU (FPU, TSC, CMOV; no MMX/SSE/3DNow!), the same paths the PC build steers the game to */
void rc_cpuid(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    uint32_t leaf = *a;
    *a = *b = *c = *d = 0;
    if (leaf == 0) {
        *a = 1;
        memcpy(b, "Genu", 4), memcpy(d, "ineI", 4), memcpy(c, "ntel", 4);
    } else if (leaf == 1) {
        *a = 0x0663;  /* family 6 */
        *d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15);
    } else if (leaf == 0x80000000) {
        *a = 0x80000000;
    }
}

#endif
