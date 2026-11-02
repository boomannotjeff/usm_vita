/* D3D9 vs_1_x/ps_1_x bytecode and fixed-function state to CG (see d3d_cg.h) */
#include "d3d_cg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_none(const char* fmt, ...) { (void)fmt; }
void (*cg_log)(const char* fmt, ...) = log_none;

typedef struct {
    char* p;
    size_t n, cap;
} Sb;
static void sb(Sb* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->n + k + 1 > s->cap) {
        s->cap = (s->n + k + 1) * 2;
        s->p = realloc(s->p, s->cap);
    }
    va_start(ap, fmt);
    vsnprintf(s->p + s->n, k + 1, fmt, ap);
    va_end(ap);
    s->n += k;
}
static char* sb_join(Sb* head, Sb* body) {
    sb(head, "%s", body->p ? body->p : "");
    free(body->p);
    return head->p;
}

/* ---- bytecode ---- */
enum { MOV = 1, ADD, SUB, MAD, MUL, RCP, RSQ, DP3, DP4, MIN, MAX, SLT, SGE, EXP, LOG, LIT, DST, LRP, FRC, M4x4, M4x3,
       M3x4, M3x3, M3x2, DCL = 31, TEXCOORD = 64, TEXKILL, TEX, TEXBEM, TEXBEML, TEXREG2AR, TEXREG2GB, EXPP = 78, LOGP,
       CND, DEF, TEXREG2RGB, CMP = 88 };
/* parameter tokens after the instruction token in 1.x shaders (they carry no length): -1 unknown */
static int params_of(uint32_t op) {
    static const int8_t t[90] = {0, 2, 3, 3, 4, 3, 2, 2, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3, 4, 2, 3, 3, 3, 3, 3,
                                 [31] = 2, [64] = 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, -1, 3, 2, 2, 2, 4, 5, 2, 2, 2, 2, 2, 1, 4, 3};
    return op < 90 ? t[op] : -1;
}
static uint32_t rtype(uint32_t p) { return (p >> 28 & 7) | (p >> 8 & 0x18); }
static uint32_t rnum(uint32_t p) { return p & 0x7ff; }

typedef struct {
    int ps;
    uint32_t used;          /* ps: samplers read */
    float def[8][4];        /* ps: def constants */
    uint32_t defined;
    int bad;
    uint32_t vcused[3];     /* vs: constants read (bits); vitaGL copies a program's whole uniform buffer per draw */
    int rel;                /* vs: reads vc[a0 + n] (skinning), so all 96 */
} Tr;

static const char* swz_str(uint32_t p, char* b) {
    uint32_t s = p >> 16 & 0xff;
    if (s == 0xe4) return "";
    b[0] = '.';
    for (int i = 0; i < 4; i++) b[1 + i] = "xyzw"[s >> 2 * i & 3];
    b[5] = 0;
    return b;
}
static const char* mask_str(uint32_t p, char* b) {
    uint32_t m = p >> 16 & 15;
    if (m == 15) return "";
    char* o = b;
    *o++ = '.';
    for (int i = 0; i < 4; i++)
        if (m >> i & 1) *o++ = "xyzw"[i];
    *o = 0;
    return b;
}

static void reg_name(Tr* t, uint32_t p, int dst, char* out, size_t n) {
    uint32_t ty = rtype(p), r = rnum(p);
    switch (ty) {
    case 0: snprintf(out, n, "r%u", r); return;
    case 1: snprintf(out, n, "v%u", r); return;
    case 2:
        if (t->ps) {
            if (r < 8 && t->defined >> r & 1)
                snprintf(out, n, "float4(%.9g, %.9g, %.9g, %.9g)", t->def[r][0], t->def[r][1], t->def[r][2], t->def[r][3]);
            else
                snprintf(out, n, "pc[%u]", r & 7);
        } else if (!dst && p & 0x2000)
            snprintf(out, n, "vc[_ai + %u]", r), t->rel = 1;
        else {
            snprintf(out, n, "vc[%u]", r);
            if (r < 96) t->vcused[r >> 5] |= 1u << (r & 31);
        }
        return;
    case 3: if (t->ps) snprintf(out, n, "t%u", r & 3); else snprintf(out, n, "a0"); return;
    case 4: snprintf(out, n, "%s", r == 0 ? "oPos" : r == 1 ? "oFogx" : "oPtsx"); return;
    case 5: snprintf(out, n, "oD%u", r & 1); return;
    case 6: if (r < 4) snprintf(out, n, "oT%u", r); else snprintf(out, n, "oTx"); return;
    }
    cg_log("cg: register type %u\n", ty);
    t->bad = 1;
    snprintf(out, n, "r0");
}

static void src(Tr* t, uint32_t p, char* out, size_t n) {
    char base[96], sw[8], tmp[160];
    reg_name(t, p, 0, base, sizeof base);
    snprintf(tmp, sizeof tmp, "%s%s%s%s", base[0] == 'f' ? "(" : "", base, base[0] == 'f' ? ")" : "", swz_str(p, sw));
    switch (p >> 24 & 15) {
    case 0: snprintf(out, n, "%s", tmp); return;
    case 1: snprintf(out, n, "(-%s)", tmp); return;
    case 2: snprintf(out, n, "(%s - 0.5)", tmp); return;
    case 3: snprintf(out, n, "(0.5 - %s)", tmp); return;
    case 4: snprintf(out, n, "(%s * 2.0 - 1.0)", tmp); return;
    case 5: snprintf(out, n, "(1.0 - %s * 2.0)", tmp); return;
    case 6: snprintf(out, n, "(1.0 - %s)", tmp); return;
    case 7: snprintf(out, n, "(%s * 2.0)", tmp); return;
    case 8: snprintf(out, n, "(%s * -2.0)", tmp); return;
    case 11: snprintf(out, n, "abs(%s)", tmp); return;
    case 12: snprintf(out, n, "(-abs(%s))", tmp); return;
    }
    cg_log("cg: source modifier %u\n", p >> 24 & 15);
    t->bad = 1;
    snprintf(out, n, "%s", tmp);
}

/* the w lane of a source: what scalar ops read (replicate swizzles make every lane the same) */
static void scalar(const char* s, char* out, size_t n) { snprintf(out, n, "(%s).w", s); }

/* one row of a matrix op: dp<k>(s0, c[n + row]) with the constant's register advanced by row */
static void mrow(Tr* t, uint32_t p1, int row, int k, const char* s0, char* out, size_t n) {
    char c[160];
    src(t, p1 + (uint32_t)row, c, sizeof c);
    if (k == 4) snprintf(out, n, "dot(%s, %s)", s0, c);
    else snprintf(out, n, "dot((%s).xyz, (%s).xyz)", s0, c);
}

static void store(Tr* t, Sb* b, uint32_t d, const char* val) {
    char name[64], m[8], v[640];
    reg_name(t, d, 1, name, sizeof name);
    int shift = d >> 24 & 15;
    const char* k = shift == 1 ? " * 2.0" : shift == 2 ? " * 4.0" : shift == 3 ? " * 8.0" : shift == 15 ? " * 0.5"
                  : shift == 14 ? " * 0.25" : shift == 13 ? " * 0.125" : "";
    snprintf(v, sizeof v, "%s%s%s%s", d >> 20 & 1 ? "saturate(" : "", val, k, d >> 20 & 1 ? ")" : "");
    if (!t->ps && rtype(d) == 3) {  /* a0: integer index, vs_1_1 rounds down */
        sb(b, "  a0 = floor(%s + 0.0001);\n  _ai = int(a0.x);\n", v);
        return;
    }
    const char* mm = mask_str(d, m);  /* "" for a full mask, m left unwritten */
    if (*mm) sb(b, "  q_ = %s;\n  %s%s = q_%s;\n", v, name, mm, mm);
    else sb(b, "  %s = %s;\n", name, v);
}

static char* translate(const uint32_t* tok, Tr* t, Sb* body) {
    const uint32_t* p = tok + 1;
    int major = tok[0] >> 8 & 0xff;
    while (*p != 0x0000ffffu) {
        uint32_t ins = *p++, op = ins & 0xffff;
        if (op == 0xfffe) { p += ins >> 16 & 0x7fff; continue; }  /* comment */
        int np = major >= 2 ? (int)(ins >> 24 & 15) : params_of(op);
        if (np < 0) { cg_log("cg: opcode %u\n", op); return NULL; }
        const uint32_t* a = p;
        p += np;
        if (op == 0) continue;
        if (op == DCL) continue;  /* read up front by cg_inputs */
        if (op == DEF) {
            uint32_t r = rnum(a[0]);
            if (r < 8) memcpy(t->def[r], a + 1, 16), t->defined |= 1u << r;
            continue;
        }
        char s[3][160], e[640], x[160];
        for (int i = 1; i < np && i < 4; i++) src(t, a[i], s[i - 1], sizeof s[0]);
        switch (op) {
        case MOV: snprintf(e, sizeof e, "%s", s[0]); break;
        case ADD: snprintf(e, sizeof e, "%s + %s", s[0], s[1]); break;
        case SUB: snprintf(e, sizeof e, "%s - %s", s[0], s[1]); break;
        case MAD: snprintf(e, sizeof e, "%s * %s + %s", s[0], s[1], s[2]); break;
        case MUL: snprintf(e, sizeof e, "%s * %s", s[0], s[1]); break;
        /* rcp/rsq of 0 and log of 0 are infinite, and the next multiply by 0 makes a NaN that shows as a colored glow
           on the Vita. D3D9-era games count on 0 * that = 0 (what DXVK's float emulation is for): clamp to +-1e38 */
        case RCP: scalar(s[0], x, sizeof x); snprintf(e, sizeof e, "float4(1,1,1,1) * clamp(1.0 / %s, -1e38, 1e38)", x); break;
        case RSQ: scalar(s[0], x, sizeof x); snprintf(e, sizeof e, "float4(1,1,1,1) * min(rsqrt(abs(%s)), 1e38)", x); break;
        case DP3: snprintf(e, sizeof e, "float4(1,1,1,1) * dot((%s).xyz, (%s).xyz)", s[0], s[1]); break;
        case DP4: snprintf(e, sizeof e, "float4(1,1,1,1) * dot(%s, %s)", s[0], s[1]); break;
        case MIN: snprintf(e, sizeof e, "min(%s, %s)", s[0], s[1]); break;
        case MAX: snprintf(e, sizeof e, "max(%s, %s)", s[0], s[1]); break;
        case SLT: snprintf(e, sizeof e, "(1.0 - step(%s, %s))", s[1], s[0]); break;
        case SGE: snprintf(e, sizeof e, "step(%s, %s)", s[1], s[0]); break;
        case EXP: scalar(s[0], x, sizeof x); snprintf(e, sizeof e, "float4(1,1,1,1) * min(exp2(%s), 1e38)", x); break;
        case EXPP: scalar(s[0], x, sizeof x); snprintf(e, sizeof e, "float4(exp2(floor(%s)), frac(%s), exp2(%s), 1.0)", x, x, x); break;
        case LOG: case LOGP: scalar(s[0], x, sizeof x); snprintf(e, sizeof e, "float4(1,1,1,1) * max(log2(abs(%s)), -1e38)", x); break;
        case LIT: snprintf(e, sizeof e, "lit((%s).x, (%s).y, clamp((%s).w, -127.9961, 127.9961))", s[0], s[0], s[0]); break;
        case DST: snprintf(e, sizeof e, "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", s[0], s[1], s[0], s[1]); break;
        case LRP: snprintf(e, sizeof e, "lerp(%s, %s, %s)", s[2], s[1], s[0]); break;
        case FRC: snprintf(e, sizeof e, "frac(%s)", s[0]); break;
        case CND: snprintf(e, sizeof e, "lerp(%s, %s, step(0.50001, %s))", s[2], s[1], s[0]); break;
        case CMP: snprintf(e, sizeof e, "lerp(%s, %s, step(0.0, %s))", s[2], s[1], s[0]); break;
        case M4x4: case M4x3: case M3x4: case M3x3: case M3x2: {
            int k = op <= M4x3 ? 4 : 3, rows = op == M4x4 || op == M3x4 ? 4 : op == M3x2 ? 2 : 3;
            char r[4][200];
            for (int i = 0; i < 4; i++) {
                if (i < rows) mrow(t, a[2], i, k, s[0], r[i], sizeof r[i]);
                else snprintf(r[i], sizeof r[i], "0.0");
            }
            snprintf(e, sizeof e, "float4(%s, %s, %s, %s)", r[0], r[1], r[2], r[3]);
            break;
        }
        case TEX: {
            uint32_t r = rnum(a[0]) & 3;
            t->used |= 1u << r;
            sb(body, "  t%u = tex2D(s%u, vT%u.xy);\n", r, r, r);
            continue;
        }
        case TEXCOORD: sb(body, "  t%u = saturate(vT%u);\n", rnum(a[0]) & 3, rnum(a[0]) & 3); continue;
        case TEXKILL: {
            uint32_t r = rnum(a[0]) & 3;
            sb(body, "  if (vT%u.x < 0.0 || vT%u.y < 0.0 || vT%u.z < 0.0) discard;\n", r, r, r);
            continue;
        }
        case TEXREG2AR: case TEXREG2GB: case TEXREG2RGB: {
            uint32_t d = rnum(a[0]) & 3, sr = rnum(a[1]) & 3;
            t->used |= 1u << d;
            sb(body, "  t%u = tex2D(s%u, t%u.%s);\n", d, d, sr, op == TEXREG2AR ? "wx" : op == TEXREG2GB ? "yz" : "xy");
            continue;
        }
        default: cg_log("cg: unsupported opcode %u\n", op); return NULL;
        }
        store(t, body, a[0], e);
        if (t->bad) return NULL;
    }
    return body->p ? body->p : "";
}

uint32_t cg_inputs(const uint32_t* tok, uint8_t usage[16], uint8_t index[16]) {
    uint32_t mask = 0;
    const uint32_t* p = tok + 1;
    while (*p != 0x0000ffffu) {
        uint32_t ins = *p++, op = ins & 0xffff;
        if (op == 0xfffe) { p += ins >> 16 & 0x7fff; continue; }
        int np = (tok[0] >> 8 & 0xff) >= 2 ? (int)(ins >> 24 & 15) : params_of(op);
        if (np < 0) break;
        if (op == DCL && rtype(p[1]) == 1) {
            uint32_t r = rnum(p[1]) & 15;
            usage[r] = p[0] & 31, index[r] = p[0] >> 16 & 15, mask |= 1u << r;
        }
        p += np;
    }
    return mask;
}

/* the constant register a vertex shader's clip w (view depth) comes from: `dp4 oPos.w, v0, cN` gives N, else -1 */
int cg_wrow(const uint32_t* tok) {
    const uint32_t* p = tok + 1;
    while (*p != 0x0000ffffu) {
        uint32_t ins = *p++, op = ins & 0xffff;
        if (op == 0xfffe) { p += ins >> 16 & 0x7fff; continue; }
        int np = (tok[0] >> 8 & 0xff) >= 2 ? (int)(ins >> 24 & 15) : params_of(op);
        if (np < 0) break;
        if (op == 9 && np >= 3 && rtype(p[0]) == 4 && rnum(p[0]) == 0 && p[0] >> 19 & 1 && rtype(p[1]) == 1 &&
            rnum(p[1]) == 0 && rtype(p[2]) == 2)
            return (int)rnum(p[2]);
        p += np;
    }
    return -1;
}

static void prologue(Sb* b, uint32_t inputs, uint32_t swz, uint32_t sizes) {
    for (int i = 0; i < 16; i++) {
        if (!(inputs >> i & 1)) continue;
        if (swz >> i & 1) sb(b, "  v%d = v%d.zyxw;\n", i, i);
        switch (sizes >> 2 * i & 3) {
        case 1: sb(b, "  v%d.w = 1.0;\n", i); break;
        case 2: sb(b, "  v%d.zw = float2(0.0, 1.0);\n", i); break;
        case 3: sb(b, "  v%d.yzw = float3(0.0, 0.0, 1.0);\n", i); break;
        }
    }
}
/* The source is written in CG spelling, then glsl() respells it: vitaGL's GLSL translator rejects CG */
static char* glsl(char* s) {
    static const char* const map[][2] = {
        {"float4", "vec4"}, {"float3", "vec3"}, {"float2", "vec2"}, {"lerp(", "mix("}, {"frac(", "fract("},
        {"rsqrt(", "inversesqrt("}, {"tex2D(", "texture2D("}, {"lit(", "lit_("}};
    for (size_t k = 0; s && k < sizeof map / sizeof *map; k++) {
        size_t la = strlen(map[k][0]), lb = strlen(map[k][1]);
        Sb o = {0};
        sb(&o, "%s", "");
        for (char *p = s, *q; *p; p = q + la) {
            q = strstr(p, map[k][0]);
            if (!q) { sb(&o, "%s", p); break; }
            sb(&o, "%.*s%.*s", (int)(q - p), p, (int)lb, map[k][1]);
        }
        free(s);
        s = o.p;
    }
    return s;
}
#define SATURATE_MACRO "#define saturate(x) clamp(x, 0.0, 1.0)\n"
static const char* LIT_HELPER =
    "float4 lit_(float n, float h, float m) {\n"
    "  return float4(1.0, max(n, 0.0), n > 0.0 && h > 0.0 ? pow(h, m) : 0.0, 1.0);\n}\n";
static const char* VARYINGS = "varying float4 xD0;\nvarying float4 xD1;\nvarying float4 xT0;\nvarying float4 xT1;\n"
                              "varying float4 xT2;\nvarying float4 xT3;\nvarying float4 xF;\n";
/* attributes are read-only in GLSL: i<n> is copied to the register v<n> */
static void vs_head(Sb* h, uint32_t inputs, const char* uniforms) {
    for (int i = 0; i < 16; i++)
        if (inputs >> i & 1) sb(h, "attribute float4 i%d;\n", i);
    sb(h, "%s%s%s%s"
          "uniform float4 u_flip;\n"
          "void main() {\n"
          "  int _ai = 0;\n"
          "  float4 oPos = float4(0, 0, 0, 1), oD0 = float4(1, 1, 1, 1), oD1 = float4(0, 0, 0, 0);\n"
          "  float4 oT0 = float4(0, 0, 0, 1), oT1 = oT0, oT2 = oT0, oT3 = oT0;\n",
       SATURATE_MACRO, uniforms, VARYINGS, LIT_HELPER);
    for (int i = 0; i < 16; i++)
        if (inputs >> i & 1) sb(h, "  float4 v%d = i%d;\n", i, i);
}
/* D3D clip depth 0..w to GL -w..w; u_fix.xy moves D3D's pixel centers onto GL's */
/* u_flip.x = -1 when drawing into a texture: GL fills it bottom row first, D3D reads it top row first */
static const char* VS_END = "  xF = float4(oPos.w, 0, 0, 0);\n  oPos.z = oPos.z * 2.0 - oPos.w;\n  oPos.xy += u_fix.xy * oPos.w;\n  oPos.y *= u_flip.x;\n"
                            "  gl_Position = oPos;\n  xD0 = oD0; xD1 = oD1; xT0 = oT0; xT1 = oT1; xT2 = oT2; xT3 = oT3;\n}\n";

char* cg_vertex(const uint32_t* tok, uint32_t swz, uint32_t sizes) {
    uint8_t u[16], ix[16];
    uint32_t inputs = cg_inputs(tok, u, ix);
    Tr t = {0};
    Sb body = {0}, h = {0};
    sb(&body, "  float4 r0 = float4(0, 0, 0, 0), r1 = r0, r2 = r0, r3 = r0, r4 = r0, r5 = r0, r6 = r0, r7 = r0, r8 = r0,"
              " r9 = r0, r10 = r0, r11 = r0, q_ = r0, oFogx = r0, oPtsx = r0, oTx = r0, a0 = r0;\n");
    prologue(&body, inputs, swz, sizes);
    if (!translate(tok, &t, &body)) { free(body.p); return NULL; }
    sb(&body, "%s", VS_END);
    /* vitaGL copies a program's whole uniform buffer on every draw, so declare only the constants the shader reads:
       one uniform vc_N each (gl.c finds them by name), or the vc[96] array when it indexes with a0 (skinning) */
    Sb uni = {0};
    if (t.rel) {
        sb(&uni, "uniform float4 vc[96];\n");
    } else {
        for (uint32_t r = 0; r < 96; r++)
            if (t.vcused[r >> 5] >> (r & 31) & 1) sb(&uni, "uniform float4 vc_%u;\n", r);
        char* o = body.p;  /* vc[N] -> vc_N, in place (never longer) */
        for (const char* q = body.p; *q;) {
            if (q[0] == 'v' && q[1] == 'c' && q[2] == '[' && q[3] >= '0' && q[3] <= '9') {
                *o++ = 'v', *o++ = 'c', *o++ = '_', q += 3;
                while (*q >= '0' && *q <= '9') *o++ = *q++;
                if (*q == ']') q++;
            } else {
                *o++ = *q++;
            }
        }
        *o = 0, body.n = (size_t)(o - body.p);
    }
    sb(&uni, "uniform float4 u_fix;\n");
    vs_head(&h, inputs, uni.p);
    free(uni.p);
    return glsl(sb_join(&h, &body));
}

static void alpha_test(Sb* b, uint32_t f) {
    static const char* ops[9] = {NULL, NULL, "<", "==", "<=", ">", "!=", ">=", NULL};
    if (f == 1) sb(b, "  discard;\n");
    else if (f < 9 && ops[f]) sb(b, "  if (!(floor(oCol.a * 255.0 + 0.5) %s u_aref.x)) discard;\n", ops[f]);
}
static void ps_head(Sb* h, uint32_t samplers, const char* uniforms) {
    sb(h, "precision highp float;\n%s", VARYINGS);
    for (int i = 0; i < 4; i++)
        if (samplers >> i & 1) sb(h, "uniform sampler2D s%d;\n", i);
    sb(h, "%s%suniform float4 u_aref;\nuniform float4 u_fog;\nuniform float4 u_fogc;\nvoid main() {\n"
          "  float4 vD0 = xD0, vD1 = xD1, vT0 = xT0, vT1 = xT1, vT2 = xT2, vT3 = xT3, oCol;\n",
       SATURATE_MACRO, uniforms);
}
/* distance fog (u_fog: start, 1 / (end - start), on), by view depth: hides where far geometry stops being drawn */
static const char* PS_END = "  oCol.rgb = mix(oCol.rgb, u_fogc.rgb, saturate((xF.x - u_fog.x) * u_fog.y) * u_fog.z);\n"
                            "  gl_FragColor = oCol;\n}\n";

char* cg_pixel(const uint32_t* tok, uint32_t afunc) {
    Tr t = {.ps = 1};
    Sb body = {0}, h = {0};
    sb(&body, "  float4 r0 = float4(0, 0, 0, 0), r1 = r0, t0 = r0, t1 = r0, t2 = r0, t3 = r0, q_ = r0;\n"
              "  float4 v0 = saturate(vD0), v1 = saturate(vD1);\n");
    if (!translate(tok, &t, &body)) { free(body.p); return NULL; }
    sb(&body, "  oCol = saturate(r0);\n");
    alpha_test(&body, afunc);
    sb(&body, "%s", PS_END);
    ps_head(&h, t.used, "uniform float4 pc[8];\n");
    return glsl(sb_join(&h, &body));
}

/* ---- fixed function ---- */
char* cg_ffp_vertex(int rhw, int matcolor, uint32_t swz, uint32_t sizes) {
    Sb b = {0};
    vs_head(&b, 0x7f, "uniform float4 m[4];\nuniform float4 u_fix;\nuniform float4 u_mat;\n");
    prologue(&b, 0x7f, swz, sizes);
    if (rhw)  /* screen position, depth 0..1 and 1/w: u_fix.zw = 2 / backbuffer size */
        sb(&b, "  float w = v0.w != 0.0 ? 1.0 / v0.w : 1.0;\n"
               "  oPos = float4((v0.x * u_fix.z - 1.0) * w, (1.0 - v0.y * u_fix.w) * w, v0.z * w, w);\n");
    else
        sb(&b, "  oPos = v0.x * m[0] + v0.y * m[1] + v0.z * m[2] + v0.w * m[3];\n");
    sb(&b, "  oD0 = %s; oD1 = v2; oT0 = v3; oT1 = v4; oT2 = v5; oT3 = v6;\n%s", matcolor ? "u_mat" : "v1", VS_END);
    return glsl(b.p);
}

static void ffp_arg(uint32_t a, char* out, size_t n) {
    static const char* base[8] = {"vD0", "cur", "tex", "u_tf", "vD1", "tmp", "u_tf", "cur"};
    char t[48];
    snprintf(t, sizeof t, "%s", base[a & 7]);
    if (a & 0x20) snprintf(t, sizeof t, "%s.wwww", base[a & 7]);
    if (a & 0x10) snprintf(out, n, "(1.0 - %s)", t);
    else snprintf(out, n, "%s", t);
}
static int ffp_op(uint32_t op, uint32_t a1, uint32_t a2, uint32_t a0, char* e, size_t n) {
    char x[48], y[48], z[48];
    ffp_arg(a1, x, sizeof x), ffp_arg(a2, y, sizeof y), ffp_arg(a0, z, sizeof z);
    switch (op) {
    case 2: snprintf(e, n, "%s", x); break;
    case 3: snprintf(e, n, "%s", y); break;
    case 4: snprintf(e, n, "%s * %s", x, y); break;
    case 5: snprintf(e, n, "%s * %s * 2.0", x, y); break;
    case 6: snprintf(e, n, "%s * %s * 4.0", x, y); break;
    case 7: snprintf(e, n, "%s + %s", x, y); break;
    case 8: snprintf(e, n, "%s + %s - 0.5", x, y); break;
    case 9: snprintf(e, n, "(%s + %s - 0.5) * 2.0", x, y); break;
    case 10: snprintf(e, n, "%s - %s", x, y); break;
    case 11: snprintf(e, n, "%s + %s - %s * %s", x, y, x, y); break;
    case 12: snprintf(e, n, "lerp(%s, %s, vD0.w)", y, x); break;
    case 13: snprintf(e, n, "lerp(%s, %s, tex.w)", y, x); break;
    case 14: snprintf(e, n, "lerp(%s, %s, u_tf.w)", y, x); break;
    case 15: snprintf(e, n, "%s + %s * (1.0 - tex.w)", x, y); break;
    case 16: snprintf(e, n, "lerp(%s, %s, cur.w)", y, x); break;
    case 18: snprintf(e, n, "%s + %s.wwww * %s", x, x, y); break;
    case 19: snprintf(e, n, "%s * %s + %s.wwww", x, y, x); break;
    case 20: snprintf(e, n, "(1.0 - %s.wwww) * %s + %s", x, y, x); break;
    case 21: snprintf(e, n, "(1.0 - %s) * %s + %s.wwww", x, y, x); break;
    case 24: snprintf(e, n, "float4(1,1,1,1) * saturate(dot(%s.xyz - 0.5, %s.xyz - 0.5) * 4.0)", x, y); break;
    case 25: snprintf(e, n, "%s + %s * %s", z, x, y); break;
    case 26: snprintf(e, n, "lerp(%s, %s, %s)", y, x, z); break;
    default: snprintf(e, n, "%s", x); return 0;  /* PREMODULATE, BUMPENVMAP*: not emulated */
    }
    return 1;
}
static int uses_tex(uint32_t op, uint32_t a1, uint32_t a2, uint32_t a0) {
    if (op == 13 || op == 15) return 1;
    int t1 = (a1 & 7) == 2, t2 = (a2 & 7) == 2, t0 = (a0 & 7) == 2;
    if (op == 2) return t1;
    if (op == 3) return t2;
    return t1 || t2 || ((op == 25 || op == 26) && t0);
}

char* cg_ffp_pixel(const FfpPixel* f, uint32_t afunc) {
    Sb body = {0}, h = {0};
    uint32_t samplers = 0;
    sb(&body, "  float4 cur = vD0, tmp = float4(0, 0, 0, 0), tex = tmp, nc;\n");
    for (uint32_t i = 0; i < f->n && i < 4; i++) {
        const FfpStage* s = &f->s[i];
        int at = s->aop != 1;
        if (uses_tex(s->cop, s->ca1, s->ca2, s->ca0) || (at && uses_tex(s->aop, s->aa1, s->aa2, s->aa0))) {
            samplers |= 1u << i;
            sb(&body, "  tex = tex2D(s%u, vT%u.xy);\n", i, s->tci & 3);
        }
        char e[256];
        const char* dst = s->result == 5 ? "tmp" : "cur";
        sb(&body, "  nc = %s;\n", dst);
        ffp_op(s->cop, s->ca1, s->ca2, s->ca0, e, sizeof e);
        sb(&body, "  nc.xyz = saturate(%s).xyz;\n", e);
        if (at) {
            ffp_op(s->aop, s->aa1, s->aa2, s->aa0, e, sizeof e);
            sb(&body, "  nc.w = saturate(%s).w;\n", e);
        }
        sb(&body, "  %s = nc;\n", dst);
    }
    if (f->specular) sb(&body, "  cur.xyz = saturate(cur.xyz + vD1.xyz);\n");
    sb(&body, "  oCol = cur;\n");
    alpha_test(&body, afunc);
    sb(&body, "%s", PS_END);
    ps_head(&h, samplers, "uniform float4 u_tf;\n");
    return glsl(sb_join(&h, &body));
}
