/* DirectSound 8 on SceAudio: every secondary buffer is guest memory the game Locks and fills; a mixer thread plays
   them (8/16-bit, mono/stereo, any rate, volume, pan, frequency, looping) into one 48 kHz stereo port and moves
   their play cursors, which the game polls to stream. The game uses the 3D interfaces without checking that it got
   them, so the listener, 3D buffers and notify exist: they keep what the game sets and hand it back, but don't
   position anything yet. */
#include <math.h>
#include <pthread.h>
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>

#include "com.h"

#define RATE 48000
#define GRAIN 512  /* samples per mixer step: ~10.7 ms */
#define MAX_BUFS 256

typedef struct {
    uint8_t* data;
    uint32_t bytes, freq, base_freq, channels, bits;
    double pos;           /* play position in frames */
    int playing, looping;
    float gl, gr;         /* volume * pan */
    int32_t vol, pan;
    uint32_t obj;
    uint32_t obj3d;       /* its IDirectSound3DBuffer, made on first QueryInterface */
    int* dref;            /* buffers sharing data (DuplicateSoundBuffer): the last Release frees it */
    /* streams (music, voice: refilled while they play): bytes written and not yet played. At a low frame rate the
       game refills late; rather than wrap around and replay the old half (a repeating line), the mixer waits */
    int stream;
    int64_t fresh;
} Buf;
static Buf* g_bufs[MAX_BUFS];
static uint32_t g_waits;  /* mixer steps a stream had to wait for the game (the log) */
static pthread_mutex_t g_mix = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vt_ds, vt_buf, vt_listener, vt_3dbuf, vt_notify;
static uint32_t g_listener;

static Buf* buf(Ctx* c) { return H(LD32(ARG(0) + 8)); }
static uint32_t frame_bytes(Buf* b) { return b->channels * b->bits / 8; }

static void gains(Buf* b) {
    float v = b->vol <= -10000 ? 0.f : powf(10.f, (float)b->vol / 2000.f);
    float l = b->pan > 0 ? powf(10.f, (float)-b->pan / 2000.f) : 1.f;
    float r = b->pan < 0 ? powf(10.f, (float)b->pan / 2000.f) : 1.f;
    b->gl = v * l, b->gr = v * r;
}

/* ---- the mixer ---- */
static int mixer(SceSize args, void* argp) {
    (void)args, (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { vlog("sceAudioOutOpenPort failed: %08x\n", port); return 0; }
    static float acc[GRAIN * 2];
    static int16_t out[GRAIN * 2];
    for (;;) {
        memset(acc, 0, sizeof acc);
        pthread_mutex_lock(&g_mix);
        for (int i = 0; i < MAX_BUFS; i++) {
            Buf* b = g_bufs[i];
            if (!b || !b->playing || !b->data) continue;
            uint32_t fb = frame_bytes(b), frames = b->bytes / fb;
            double step = (double)b->freq / RATE;
            for (int s = 0; s < GRAIN && b->playing; s++) {
                if (b->stream && b->fresh <= 0) {  /* nothing new to play yet: silence, the position waits */
                    if (!(g_waits++ & 1023)) vlog("sound: stream ran dry (%u times), waiting for the game\n", g_waits);
                    break;
                }
                uint32_t f = (uint32_t)b->pos;
                const uint8_t* p = b->data + f * fb;
                float l, r;
                if (b->bits == 16) {
                    l = ((const int16_t*)p)[0] / 32768.f;
                    r = b->channels == 2 ? ((const int16_t*)p)[1] / 32768.f : l;
                } else {
                    l = (p[0] - 128) / 128.f;
                    r = b->channels == 2 ? (p[1] - 128) / 128.f : l;
                }
                acc[2 * s] += l * b->gl, acc[2 * s + 1] += r * b->gr;
                uint32_t before = (uint32_t)b->pos;
                b->pos += step;
                if (b->stream) b->fresh -= (int64_t)((uint32_t)b->pos - before) * fb;
                if (b->pos >= frames) {
                    if (b->looping) b->pos -= frames;
                    else b->playing = 0, b->pos = 0;
                }
            }
        }
        pthread_mutex_unlock(&g_mix);
        for (int s = 0; s < GRAIN * 2; s++) {
            float v = acc[s] * 32767.f;
            out[s] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : v);
        }
        sceAudioOutOutput(port, out);  /* blocks until the port takes it: paces the loop */
    }
    return 0;
}

/* ---- IDirectSoundBuffer8 ---- */
static void b_Lock(Ctx* c) {  /* (offset, bytes, p1, n1, p2, n2, flags) */
    Buf* b = buf(c);
    if (!b->data || !b->bytes) { c->eax = 0x88780032u; return; }  /* DSERR_INVALIDCALL: the primary buffer */
    uint32_t off = ARG(1), n = ARG(2), fl = ARG(7);
    if (fl & 2) off = 0, n = b->bytes;  /* DSBLOCK_ENTIREBUFFER */
    if (fl & 1) off = ((uint32_t)b->pos * frame_bytes(b) + 1024) % b->bytes;  /* DSBLOCK_FROMWRITECURSOR */
    if (n > b->bytes) n = b->bytes;
    off %= b->bytes ? b->bytes : 1;
    uint32_t first = off + n <= b->bytes ? n : b->bytes - off;
    ST32(ARG(3), G(b->data + off)), ST32(ARG(4), first);
    if (ARG(5)) ST32(ARG(5), first < n ? G(b->data) : 0);
    if (ARG(6)) ST32(ARG(6), n - first);
    c->eax = 0;
}
static void b_Unlock(Ctx* c) {  /* (p1, n1, p2, n2) */
    Buf* b = buf(c);
    pthread_mutex_lock(&g_mix);
    if (b->playing && b->looping && !b->stream)  /* a looping buffer written while it plays: a stream */
        b->stream = 1, vlog("sound: a %u-byte buffer is a stream (refilled while it plays)\n", b->bytes);
    b->fresh += (int64_t)ARG(2) + ARG(4);
    if (b->fresh > b->bytes) b->fresh = b->bytes;
    pthread_mutex_unlock(&g_mix);
    c->eax = 0;
}
static void b_Play(Ctx* c) {  /* (reserved, priority, flags) */
    Buf* b = buf(c);
    pthread_mutex_lock(&g_mix);
    b->looping = ARG(3) & 1, b->playing = 1;
    pthread_mutex_unlock(&g_mix);
    c->eax = 0;
}
static void b_Stop(Ctx* c) { buf(c)->playing = 0; c->eax = 0; }
static void b_GetStatus(Ctx* c) { Buf* b = buf(c); ST32(ARG(1), b->playing ? (b->looping ? 5 : 1) : 0); c->eax = 0; }
static void b_GetCurrentPosition(Ctx* c) {  /* (play, write) */
    Buf* b = buf(c);
    uint32_t p = (uint32_t)b->pos * frame_bytes(b);
    if (ARG(1)) ST32(ARG(1), p);
    if (ARG(2)) ST32(ARG(2), b->bytes ? (p + GRAIN * frame_bytes(b)) % b->bytes : 0);
    c->eax = 0;
}
static void b_SetCurrentPosition(Ctx* c) {  /* a restart: everything in the buffer counts as new again */
    Buf* b = buf(c);
    pthread_mutex_lock(&g_mix);
    b->pos = (double)(ARG(1) / frame_bytes(b)), b->fresh = b->bytes;
    pthread_mutex_unlock(&g_mix);
    c->eax = 0;
}
static void b_SetVolume(Ctx* c) { Buf* b = buf(c); b->vol = (int32_t)ARG(1); gains(b); c->eax = 0; }
static void b_GetVolume(Ctx* c) { ST32(ARG(1), (uint32_t)buf(c)->vol); c->eax = 0; }
static void b_SetPan(Ctx* c) { Buf* b = buf(c); b->pan = (int32_t)ARG(1); gains(b); c->eax = 0; }
static void b_GetPan(Ctx* c) { ST32(ARG(1), (uint32_t)buf(c)->pan); c->eax = 0; }
static void b_SetFrequency(Ctx* c) { Buf* b = buf(c); b->freq = ARG(1) ? ARG(1) : b->base_freq; c->eax = 0; }
static void b_GetFrequency(Ctx* c) { ST32(ARG(1), buf(c)->freq); c->eax = 0; }
static void b_GetFormat(Ctx* c) {  /* (wfx, size, written) */
    Buf* b = buf(c);
    uint16_t w[9] = {1, (uint16_t)b->channels, (uint16_t)b->base_freq, (uint16_t)(b->base_freq >> 16), 0, 0,
                     (uint16_t)frame_bytes(b), (uint16_t)b->bits, 0};
    uint32_t avg = b->base_freq * frame_bytes(b);
    w[4] = (uint16_t)avg, w[5] = (uint16_t)(avg >> 16);
    if (ARG(1)) memcpy(H(ARG(1)), w, ARG(2) < 18 ? ARG(2) : 18);
    if (ARG(3)) ST32(ARG(3), 18);
    c->eax = 0;
}
static void b_GetCaps(Ctx* c) { ST32(ARG(1) + 4, 0xe0), ST32(ARG(1) + 8, buf(c)->bytes); c->eax = 0; }

/* ---- IDirectSound3DListener, IDirectSound3DBuffer: the DS3DLISTENER / DS3DBUFFER (64 bytes) at object + 16 ---- */
#define P3D(c) (ARG(0) + 16)
static uint32_t new_3d(uint32_t vt, const uint32_t* init) {
    uint32_t o = com_new(vt, 16 + 64);
    memcpy(H(o + 16), init, 64);
    return o;
}
#define F32(name, off)                                                    \
    static void Set##name(Ctx* c) { ST32(P3D(c) + off, ARG(1)); c->eax = 0; } \
    static void Get##name(Ctx* c) { ST32(ARG(1), LD32(P3D(c) + off)); c->eax = 0; }
#define VEC(name, off)                                                                                       \
    static void Set##name(Ctx* c) { ST32(P3D(c) + off, ARG(1)), ST32(P3D(c) + off + 4, ARG(2)), ST32(P3D(c) + off + 8, ARG(3)); c->eax = 0; } \
    static void Get##name(Ctx* c) { memcpy(H(ARG(1)), H(P3D(c) + off), 12); c->eax = 0; }
static void SetAll(Ctx* c) { memcpy(H(P3D(c) + 4), H(ARG(1) + 4), 60); c->eax = 0; }
static void GetAll(Ctx* c) { memcpy(H(ARG(1)), H(P3D(c)), 64); c->eax = 0; }
static void Commit(Ctx* c) { c->eax = 0; }
/* DS3DLISTENER: size, position 4, velocity 16, front 28, top 40, distance 52, rolloff 56, doppler 60 */
VEC(LPosition, 4) VEC(LVelocity, 16) F32(DistanceFactor, 52) F32(RolloffFactor, 56) F32(DopplerFactor, 60)
static void SetOrientation(Ctx* c) { memcpy(H(P3D(c) + 28), H(c->esp + 4), 24); c->eax = 0; }  /* 6 floats */
static void GetOrientation(Ctx* c) { memcpy(H(ARG(1)), H(P3D(c) + 28), 12), memcpy(H(ARG(2)), H(P3D(c) + 40), 12); c->eax = 0; }
/* DS3DBUFFER: size, position 4, velocity 16, inside 28, outside 32, cone 36, outside volume 48, min 52, max 56, mode 60 */
VEC(BPosition, 4) VEC(BVelocity, 16) VEC(ConeOrientation, 36) F32(ConeOutsideVolume, 48) F32(MinDistance, 52)
F32(MaxDistance, 56) F32(Mode, 60)
static void SetConeAngles(Ctx* c) { ST32(P3D(c) + 28, ARG(1)), ST32(P3D(c) + 32, ARG(2)); c->eax = 0; }
static void GetConeAngles(Ctx* c) {
    if (ARG(1)) ST32(ARG(1), LD32(P3D(c) + 28));
    if (ARG(2)) ST32(ARG(2), LD32(P3D(c) + 32));
    c->eax = 0;
}
static const ShimDef listener_impl[] = {
    {"GetAllParameters", GetAll}, {"SetAllParameters", SetAll}, {"GetPosition", GetLPosition},
    {"SetPosition", SetLPosition}, {"GetVelocity", GetLVelocity}, {"SetVelocity", SetLVelocity},
    {"GetOrientation", GetOrientation}, {"SetOrientation", SetOrientation}, {"GetDistanceFactor", GetDistanceFactor},
    {"SetDistanceFactor", SetDistanceFactor}, {"GetRolloffFactor", GetRolloffFactor},
    {"SetRolloffFactor", SetRolloffFactor}, {"GetDopplerFactor", GetDopplerFactor},
    {"SetDopplerFactor", SetDopplerFactor}, {"CommitDeferredSettings", Commit}, {NULL, NULL},
};
static const ShimDef buf3d_impl[] = {
    {"GetAllParameters", GetAll}, {"SetAllParameters", SetAll}, {"GetPosition", GetBPosition},
    {"SetPosition", SetBPosition}, {"GetVelocity", GetBVelocity}, {"SetVelocity", SetBVelocity},
    {"GetConeAngles", GetConeAngles}, {"SetConeAngles", SetConeAngles}, {"GetConeOrientation", GetConeOrientation},
    {"SetConeOrientation", SetConeOrientation}, {"GetConeOutsideVolume", GetConeOutsideVolume},
    {"SetConeOutsideVolume", SetConeOutsideVolume}, {"GetMinDistance", GetMinDistance},
    {"SetMinDistance", SetMinDistance}, {"GetMaxDistance", GetMaxDistance}, {"SetMaxDistance", SetMaxDistance},
    {"GetMode", GetMode}, {"SetMode", SetMode}, {NULL, NULL},
};
static const ShimDef notify_impl[] = {{"SetNotificationPositions", Commit}, {NULL, NULL}};

static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* IDirectSoundBuffer(8) is this object; the listener (primary buffer), 3D buffer and notify are their own */
static void b_QueryInterface(Ctx* c) {
    uint32_t iid = LD32(ARG(1)), o = 0;
    Buf* b = buf(c);
    if (iid == 0x6825a449u || iid == 0x279afa85u || iid == 0) {
        o = ARG(0);
    } else if (iid == 0x279afa84u) {  /* IDirectSound3DListener */
        if (!g_listener) {
            uint32_t l[16] = {64, 0, 0, 0, 0, 0, 0, 0, 0, bits_of(1.f), 0, bits_of(1.f), 0,
                              bits_of(1.f), bits_of(1.f), bits_of(1.f)};
            g_listener = new_3d(vt_listener, l);
        }
        o = g_listener;
    } else if (iid == 0x279afa86u) {  /* IDirectSound3DBuffer */
        if (!b->obj3d) {
            uint32_t d[16] = {64, 0, 0, 0, 0, 0, 0, 360, 360, 0, 0, bits_of(1.f), 0, bits_of(1.f),
                              bits_of(1e9f), 0};
            b->obj3d = new_3d(vt_3dbuf, d);
        }
        o = b->obj3d;
    } else if (iid == 0xb0210783u) {  /* IDirectSoundNotify */
        o = com_new(vt_notify, 16);
    }
    if (!o) {
        vlog("IDirectSoundBuffer8::QueryInterface: no interface %08x\n", iid);
        ST32(ARG(2), 0);
        c->eax = 0x80004002u;
        return;
    }
    ST32(ARG(2), o);
    c->eax = 0;
}
static void b_SetFormat(Ctx* c) { c->eax = 0; }
static void b_GetObjectInPath(Ctx* c) { ST32(ARG(4), 0); c->eax = 0x80004001u; }  /* no effects (DMOs) */
static void b_Release(Ctx* c);
static const ShimDef buf_impl[] = {
    {"Release", b_Release}, {"Lock", b_Lock}, {"Unlock", b_Unlock}, {"Play", b_Play}, {"Stop", b_Stop}, {"GetStatus", b_GetStatus},
    {"GetCurrentPosition", b_GetCurrentPosition}, {"SetCurrentPosition", b_SetCurrentPosition},
    {"SetVolume", b_SetVolume}, {"GetVolume", b_GetVolume}, {"SetPan", b_SetPan}, {"GetPan", b_GetPan},
    {"SetFrequency", b_SetFrequency}, {"GetFrequency", b_GetFrequency}, {"GetFormat", b_GetFormat},
    {"GetCaps", b_GetCaps}, {"QueryInterface", b_QueryInterface}, {"SetFormat", b_SetFormat},
    {"GetObjectInPath", b_GetObjectInPath}, {NULL, NULL},
};

/* the game makes a buffer per voice line and stream and releases it after: at 0 the mixer forgets it and its data goes
   (the 16-byte object and the Buf stay, so a stale pointer finds an empty buffer) */
static void b_Release(Ctx* c) {
    uint32_t* rc = H(ARG(0) + 4);
    if (!*rc) { c->eax = 0; return; }
    c->eax = __atomic_sub_fetch(rc, 1, __ATOMIC_SEQ_CST);
    if (c->eax) return;
    Buf* b = buf(c);
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < MAX_BUFS; i++)
        if (g_bufs[i] == b) g_bufs[i] = NULL;
    b->playing = 0;
    int last = b->dref && !--*b->dref;  /* under the lock: buffers sharing data may be released from two threads */
    uint8_t* data = b->data;
    int* dref = b->dref;
    b->data = NULL, b->dref = NULL, b->bytes = 0;
    pthread_mutex_unlock(&g_mix);
    if (last) free(data), free(dref);
}

static uint32_t new_buf(uint8_t* data, int* dref, uint32_t bytes, uint32_t freq, uint32_t ch, uint32_t bits) {
    Buf* b = guest_alloc(sizeof(Buf));
    pthread_mutex_lock(&g_mix);
    b->dref = dref;
    if (dref) ++*dref;
    pthread_mutex_unlock(&g_mix);
    b->data = data, b->bytes = bytes, b->freq = b->base_freq = freq ? freq : 22050;
    b->channels = ch ? ch : 1, b->bits = bits == 8 ? 8 : 16;
    gains(b);
    b->obj = com_new(vt_buf, 16);
    ST32(b->obj + 8, G(b));
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < MAX_BUFS; i++)
        if (!g_bufs[i] || (!g_bufs[i]->playing && LD32(g_bufs[i]->obj + 4) == 0)) { g_bufs[i] = b; break; }
    pthread_mutex_unlock(&g_mix);
    return b->obj;
}

/* ---- IDirectSound8 ---- */
static void ds_CreateSoundBuffer(Ctx* c) {  /* (desc, ppBuf, outer); DSBUFFERDESC {size, flags, bytes, reserved, wfx} */
    uint32_t d = ARG(1), flags = LD32(d + 4), bytes = LD32(d + 8), wfx = LD32(d + 16);
    uint32_t freq = 22050, ch = 2, bits = 16;
    if (wfx) ch = LD16(wfx + 2), freq = LD32(wfx + 4), bits = LD16(wfx + 14);
    if (flags & 1) bytes = 4096;  /* DSBCAPS_PRIMARYBUFFER: never mixed, only configured */
    uint32_t o = new_buf(guest_alloc(bytes ? bytes : 16), calloc(1, sizeof(int)), bytes, freq, ch, bits);
    if (flags & 1) ((Buf*)H(LD32(o + 8)))->data = NULL;  /* the mixer skips it, Play or not */
    ST32(ARG(2), o);
    c->eax = 0;
}
static void ds_DuplicateSoundBuffer(Ctx* c) {  /* (original, ppDup): shares the sample data */
    Buf* s = H(LD32(ARG(1) + 8));
    uint32_t o = new_buf(s->data, s->dref, s->bytes, s->base_freq, s->channels, s->bits);
    ST32(ARG(2), o);
    c->eax = 0;
}
static void ds_GetCaps(Ctx* c) { ST32(ARG(1) + 4, 0xf5f); c->eax = 0; }
static void ds_GetSpeakerConfig(Ctx* c) { ST32(ARG(1), 4); c->eax = 0; }  /* DSSPEAKER_STEREO */
static void ds_Ok(Ctx* c) { c->eax = 0; }
static const ShimDef ds_impl[] = {{"CreateSoundBuffer", ds_CreateSoundBuffer},
                                  {"DuplicateSoundBuffer", ds_DuplicateSoundBuffer}, {"GetCaps", ds_GetCaps},
                                  {"GetSpeakerConfig", ds_GetSpeakerConfig}, {"SetCooperativeLevel", ds_Ok},
                                  {"SetSpeakerConfig", ds_Ok}, {"Compact", ds_Ok}, {"Initialize", ds_Ok},
                                  {NULL, NULL}};

static void DirectSoundCreate8_(Ctx* c) {  /* (guid, ppDS, outer) */
    if (!vt_ds) {
        vt_ds = com_vtable("IDirectSound8", ds_impl);
        vt_buf = com_vtable("IDirectSoundBuffer8", buf_impl);
        vt_listener = com_vtable("IDirectSound3DListener", listener_impl);
        vt_3dbuf = com_vtable("IDirectSound3DBuffer", buf3d_impl);
        vt_notify = com_vtable("IDirectSoundNotify", notify_impl);
        SceUID t = sceKernelCreateThread("usm_mixer", mixer, 0x40, 0x10000, 0, 0x80000, NULL);  /* core 3, else 1 (2 is the pipeline worker's) */
        if (t < 0) t = sceKernelCreateThread("usm_mixer", mixer, 0x40, 0x10000, 0, 0x20000, NULL);
        sceKernelStartThread(t, 0, NULL);
    }
    ST32(ARG(1), com_new(vt_ds, 16));
    vlog("DirectSoundCreate8\n");
    c->eax = 0;
}

const ShimDef shims_dsound[] = {{"DirectSoundCreate8", DirectSoundCreate8_}, {"DirectSoundCreate", DirectSoundCreate8_},
                                {NULL, NULL}};
