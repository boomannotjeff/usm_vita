#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vita.h"
#include "vita_util.h"

char* gstrdup(const char* s) {
    char* d = guest_alloc((uint32_t)strlen(s) + 1);
    strcpy(d, s);
    return d;
}

int gformat(char* out, uint32_t n, const char* fmt, uint32_t ap) {
    uint32_t len = 0;
    char spec[32], tmp[512];
#define PUT(s, k) do { uint32_t k_ = (uint32_t)(k); if (len < n) memcpy(out + len, s, len + k_ < n ? k_ : n - len); len += k_; } while (0)
    while (*fmt) {
        if (*fmt != '%') { PUT(fmt, 1); fmt++; continue; }
        if (fmt[1] == '%') { PUT("%", 1); fmt += 2; continue; }
        /* copy flags, width, precision; '*' takes an int argument */
        unsigned k = 0;
        spec[k++] = *fmt++;
        int star[2], nstar = 0, wide64 = 0;
        while (*fmt && strchr("-+ #0123456789.*", *fmt) && k < 20) {
            if (*fmt == '*' && nstar < 2) star[nstar++] = (int)LD32(ap), ap += 4;
            spec[k++] = *fmt++;
        }
        if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') wide64 = 1, fmt += 3;
        else if (fmt[0] == 'l' && fmt[1] == 'l') wide64 = 1, fmt += 2;
        else while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L') fmt++;
        char conv = *fmt ? *fmt++ : 'd';
        int w;
        if (strchr("diouxXc", conv)) {
            if (wide64) {
                spec[k++] = 'l', spec[k++] = 'l';
            }
            spec[k++] = conv, spec[k] = 0;
            if (wide64) {
                uint64_t v = LD32(ap) | (uint64_t)LD32(ap + 4) << 32;
                ap += 8;
                w = nstar == 2 ? snprintf(tmp, sizeof tmp, spec, star[0], star[1], v) : nstar ? snprintf(tmp, sizeof tmp, spec, star[0], v) : snprintf(tmp, sizeof tmp, spec, v);
            } else {
                uint32_t v = LD32(ap);
                ap += 4;
                w = nstar == 2 ? snprintf(tmp, sizeof tmp, spec, star[0], star[1], v) : nstar ? snprintf(tmp, sizeof tmp, spec, star[0], v) : snprintf(tmp, sizeof tmp, spec, v);
            }
        } else if (strchr("eEfgGaA", conv)) {
            spec[k++] = conv, spec[k] = 0;
            uint64_t b = LD32(ap) | (uint64_t)LD32(ap + 4) << 32;
            double d;
            memcpy(&d, &b, 8);
            ap += 8;
            w = nstar == 2 ? snprintf(tmp, sizeof tmp, spec, star[0], star[1], d) : nstar ? snprintf(tmp, sizeof tmp, spec, star[0], d) : snprintf(tmp, sizeof tmp, spec, d);
        } else if (conv == 's') {
            spec[k++] = 's', spec[k] = 0;
            uint32_t a = LD32(ap);
            ap += 4;
            const char* s = a ? (const char*)H(a) : "(null)";
            w = nstar == 2 ? snprintf(tmp, sizeof tmp, spec, star[0], star[1], s) : nstar ? snprintf(tmp, sizeof tmp, spec, star[0], s) : snprintf(tmp, sizeof tmp, spec, s);
        } else if (conv == 'p') {
            w = snprintf(tmp, sizeof tmp, "%08X", LD32(ap));
            ap += 4;
        } else if (conv == 'n') {
            ST32(LD32(ap), len);
            ap += 4;
            continue;
        } else {
            PUT(&conv, 1);
            continue;
        }
        if (w > 0) PUT(tmp, w < (int)sizeof tmp ? w : (int)sizeof tmp - 1);
    }
    if (n) out[len < n ? len : n - 1] = 0;
    return (int)len;
#undef PUT
}

void map_path(const char* game, char* out, unsigned n) {
    char p[512];
    unsigned k = 0;
    for (; game[k] && k < sizeof p - 1; k++) p[k] = game[k] == '\\' ? '/' : game[k];
    p[k] = 0;
    const char* s = p;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') s += 2;  /* drive */
    while (*s == '/') s++;
    if (!strncasecmp(s, "USM/", 4)) s += 4;                    /* GAME_CWD */
    while (s[0] == '.' && s[1] == '/') s += 2;
    snprintf(out, n, USM_DIR "%s", s);
}
