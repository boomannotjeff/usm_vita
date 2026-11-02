#include <stdio.h>
#include <stdlib.h>

#include "com.h"

static void AddRef_(Ctx* c) { c->eax = __atomic_add_fetch((uint32_t*)H(ARG(0) + 4), 1, __ATOMIC_SEQ_CST); }
static void Release_(Ctx* c) {
    uint32_t* rc = H(ARG(0) + 4);
    c->eax = *rc ? __atomic_sub_fetch(rc, 1, __ATOMIC_SEQ_CST) : 0;  /* never freed: the game may still hold it */
}
/* hands back the same object for any interface; each new (interface, IID) pair is logged, so an interface the game
   expects to be a different object shows up in the log instead of as a wild call */
static void QueryInterface_(Ctx* c) {
    static uint32_t seen[256];
    static unsigned n;
    uint32_t vt = LD32(ARG(0)), iid = LD32(ARG(1)), key = vt * 31 ^ iid, k = 0;
    while (k < n && seen[k] != key) k++;
    if (k == n && n < 256) {
        seen[n++] = key;
        vlog("%s::QueryInterface(%08x-%04x-%04x): the same object\n", (const char*)H(LD32(vt - 4)), iid,
             LD16(ARG(1) + 4), LD16(ARG(1) + 6));
    }
    ST32(ARG(2), ARG(0));
    AddRef_(c);
    c->eax = 0;
}

uint32_t com_vtable(const char* iface, const ShimDef* impl) { return com_vtable_ext(iface, impl, NULL); }

uint32_t com_vtable_ext(const char* iface, const ShimDef* impl, const ShimDef* more) {
    const ComIface* f = g_ifaces;
    while (f->name && strcmp(f->name, iface)) f++;
    if (!f->name) { vlog("unknown COM interface %s\n", iface); abort(); }
    uint32_t* vt = (uint32_t*)guest_alloc(4 * (uint32_t)f->n + 4) + 1;  /* vt[-1]: the interface name */
    vt[-1] = G(f->name);
    for (int i = 0; i < f->n; i++) {
        shim_fn fn = !strcmp(f->m[i].name, "AddRef") ? AddRef_ : !strcmp(f->m[i].name, "Release") ? Release_
                   : !strcmp(f->m[i].name, "QueryInterface") ? QueryInterface_ : NULL;
        for (const ShimDef* d = impl; d && d->name; d++)
            if (!strcmp(d->name, f->m[i].name)) fn = d->fn;
        for (const ShimDef* d = more; d && d->name; d++)
            if (!strcmp(d->name, f->m[i].name)) fn = d->fn;
        char* full = malloc(strlen(iface) + strlen(f->m[i].name) + 3);
        sprintf(full, "%s::%s", iface, f->m[i].name);
        vt[i] = shim_new(full, fn, f->m[i].pops);
    }
    return G(vt);
}

uint32_t com_new(uint32_t vtable, uint32_t size) {
    uint32_t o = G(guest_alloc(size < 8 ? 8 : size));
    ST32(o, vtable);
    ST32(o + 4, 1);
    return o;
}
