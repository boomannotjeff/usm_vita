// translate the shaders the Vita dumped (outs/shaders) to CG on the host:
// gcc -Isrc/vita tools/recomp/cg_test.c src/vita/d3d_cg.c -o cg_test && cg_test outs/shaders/*.vs outs/shaders/*.ps
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d_cg.h"

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

int main(int argc, char** argv) {
    int bad = 0;
    cg_log = say;
    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) continue;
        static uint32_t tok[65536];
        size_t n = fread(tok, 4, 65535, f);
        fclose(f);
        tok[n] = 0x0000ffffu;
        int ps = (tok[0] >> 16) == 0xffff;
        char* s = ps ? cg_pixel(tok, 5) : cg_vertex(tok, 0x4, 0);
        printf("// ==== %s\n%s\n", argv[i], s ? s : "// FAILED");
        bad += !s;
        free(s);
    }
    FfpPixel fp = {.n = 2, .s = {{4, 2, 0, 1, 4, 2, 0, 1, 0, 1}, {13, 2, 1, 1, 2, 1, 1, 1, 1, 1}}};
    char* s = cg_ffp_pixel(&fp, 0);
    printf("// ==== ffp pixel\n%s\n", s);
    free(s);
    s = cg_ffp_vertex(1, 0, 2, 1);
    printf("// ==== ffp vertex rhw\n%s\n", s);
    free(s);
    fprintf(stderr, "%d failed\n", bad);
    return bad != 0;
}
