/* D3D9 shaders and fixed-function state to CG source for vitaGL's runtime compiler (no vitaGL here: host-testable).
   Every vertex program writes the same varyings: POSITION, COLOR0/1 (d0, d1), TEXCOORD0-3 (t0-t3). Attributes are
   float4 inputs named v<location>; swz has a bit per location holding a D3DCOLOR (BGRA bytes), sizes 2 bits per
   location with 4 - components of the array (0: full or generic). afunc: D3DCMP alpha test, 0 off. Returns malloc'd
   source, NULL when the bytecode has something unsupported (logged through cg_log). */
#pragma once
#include <stdint.h>

extern void (*cg_log)(const char* fmt, ...);

/* declared vertex shader inputs: usage/index by register, returns the mask of registers */
uint32_t cg_inputs(const uint32_t* tok, uint8_t usage[16], uint8_t index[16]);
char* cg_vertex(const uint32_t* tok, uint32_t swz, uint32_t sizes);
int cg_wrow(const uint32_t* tok);
char* cg_pixel(const uint32_t* tok, uint32_t afunc);

/* fixed function. Locations: 0 position, 1 diffuse, 2 specular, 3-6 texcoords 0-3. rhw: pre-transformed position;
   matcolor: diffuse from the material (lighting on, no vertex color) */
enum { FFP_POS, FFP_D0, FFP_D1, FFP_T0 };
char* cg_ffp_vertex(int rhw, int matcolor, uint32_t swz, uint32_t sizes);

/* the texture stages that decide the fixed-function pixel program */
typedef struct {
    uint32_t cop, ca1, ca2, ca0, aop, aa1, aa2, aa0, tci, result;
} FfpStage;
typedef struct {
    FfpStage s[4];
    uint32_t n, specular;  /* n: stages before the first disabled one */
} FfpPixel;
char* cg_ffp_pixel(const FfpPixel* f, uint32_t afunc);
