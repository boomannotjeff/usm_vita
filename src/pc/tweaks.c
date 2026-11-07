/* usm_pc.exe only: push people/traffic and the object distance past the PC game's own limits.
   Two one-number text files next to usm_pc.exe (missing = 1 = the game as shipped):
     pc_density.txt   multiplies pedestrian (0x937ff0) and traffic (0x937ff8) density. The game clamps both to 0..1;
                      cars allowed = 30 x traffic density, parked cars = that x 0x937ffc, peds = 10 ped groups x density.
     pc_distance.txt  multiplies the entity build distances 540380 uses (0x921f0c 3000 m high camera, 0x921f10 1700 m,
                      both stored squared). The camera far plane is already 10000 with Settings DistanceClipping off.
   Both hook functions are in tools/recomp/native.txt, so these strong definitions replace the weak pass-throughs. */
#include <stdio.h>

#include "rt.h"
#include "recomp.h"

static float read_mult(const char* name) {
    float v = 1;
    FILE* f = fopen(name, "r");
    if (f) {
        if (fscanf(f, "%f", &v) != 1 || v <= 0) v = 1;
        fclose(f);
    }
    return v;
}
static float density_mult(void) {
    static float m = -1;
    if (m < 0) m = read_mult("pc_density.txt");
    return m;
}

/* 6c2e10 (cdecl float): the pedestrian density setter. The traffic manager 6d8610 rewrites 0x937ff8 every logic tick
   and passes it here, so traffic is scaled here too; `last` keeps a value already scaled from being scaled again. */
void f_6c2e10(Ctx* c) {
    static float last = -1;
    float m = density_mult();
    fo_6c2e10(c);
    if (m == 1) return;
    STF32(0x937ff0, LDF32(0x937ff0) * m);
    float t = LDF32(0x937ff8);
    if (t != last) STF32(0x937ff8, last = t * m);
}

/* 73ea70: the render frame, once per world frame on the main thread */
void f_73ea70(Ctx* c) {
    static float m = -1, hi, lo;
    if (m < 0) m = read_mult("pc_distance.txt"), hi = LDF32(0x921f0c), lo = LDF32(0x921f10);
    if (m != 1) STF32(0x921f0c, hi * m * m), STF32(0x921f10, lo * m * m);  /* squared distances */
    fo_73ea70(c);
}
