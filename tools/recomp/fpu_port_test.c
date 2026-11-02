/* Checks src/recomp/fpu_port.c (the Vita's x87 helpers) against the real x87 on the PC.
   build: clang --target=i686-w64-mingw32 -O1 -DRC_PORTABLE -Isrc/recomp tools/recomp/fpu_port_test.c src/recomp/fpu_port.c */
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

static uint64_t rnd64(void) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r = r << 16 ^ (uint64_t)(rand() & 0xffff);
    return r;
}

static long double x87_ld(const uint8_t* b) { long double v = 0; memcpy(&v, b, 10); return v; }
static int16_t x87_fist16(long double v) { int16_t r; __asm__("fistps %0" : "=m"(r) : "t"(v) : "st"); return r; }
static int32_t x87_fist32(long double v) { int32_t r; __asm__("fistpl %0" : "=m"(r) : "t"(v) : "st"); return r; }
static int64_t x87_fist64(long double v) { int64_t r; __asm__("fistpll %0" : "=m"(r) : "t"(v) : "st"); return r; }

int main(void) {
    int bad = 0;
    Ctx c = {0};
    for (int i = 0; i < 2000000 && bad < 10; i++) {
        /* 80-bit -> double: random patterns, exponents biased toward the double range; unnormals skipped */
        uint8_t b[10];
        uint64_t m = rnd64() | 0x8000000000000000ull;
        uint16_t se = (uint16_t)((rand() & 1) << 15 | (i & 7 ? 16383 - 1100 + rand() % 2200 : rand() & 0x7fff));
        memcpy(b, &m, 8), memcpy(b + 8, &se, 2);
        double want = (double)x87_ld(b), got = rc_ld80(b);
        if (memcmp(&want, &got, 8) && !(isnan(want) && isnan(got)) && fabs(want) >= 0x1p-1022 /* see rc_ld80 */) printf("ld80 %04x:%016llx want %a got %a\n", se, (unsigned long long)m, want, got), bad++;

        /* double -> 80-bit */
        uint64_t db = rnd64();
        double d;
        memcpy(&d, &db, 8);
        uint8_t w[10] = {0}, g[10] = {0};
        long double ld = d;
        memcpy(w, &ld, 10);
        rc_st80(g, d);
        if (memcmp(w, g, 10) && !isnan(d)) printf("st80 %a differs\n", d), bad++;

        /* fist/frndint under each rounding mode, values around the integer ranges */
        uint16_t cw = (uint16_t)(0x027f | (i & 3) << 10);
        double v = ldexp((double)(int64_t)rnd64(), -(rand() % 64));
        rc_fldcw(&c, cw);
        __asm__ volatile("fldcw %0" : : "m"(cw));
        if (rc_fist16(v) != x87_fist16(v) || rc_fist32(v) != x87_fist32(v) || rc_fist64(v) != x87_fist64(v))
            printf("fist mode %d %a: 16 %d/%d 32 %d/%d\n", i & 3, v, rc_fist16(v), x87_fist16(v), rc_fist32(v), x87_fist32(v)), bad++;
    }
    printf(bad ? "FAILED\n" : "ok\n");
    return bad != 0;
}
