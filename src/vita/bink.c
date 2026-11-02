/* Bink movies (intro logos, IGC cutscenes), played straight from the game's own files (Data/movies/pc/*.dat, Bink 1
   "BIKi") with FFmpeg's Bink video and audio decoders (vendor/ffmpeg, LGPL 2.1) on a thread of their own: nothing to
   convert, the game data is used as it is.
   The container is read here (the header, the audio track table, the frame index; each frame is every audio track's
   packet then the video packet), as FFmpeg's libavformat/bink.c reads it.
   The game's player (BinkOpen call at 0x629a0b) reads BINK {Width, Height, Frames, FrameNum}, makes an X8R8G8B8 texture
   that size, and while BinkWait says 0 does DoFrame, CopyToBuffer(bink, bits, pitch, height, 0, 0, 3 = 32-bit BGRx) and
   NextFrame; it stops when FrameNum reaches Frames or on a skip press. No file: BinkOpen fails and the game skips the
   movie. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>

#include "vita.h"

#define S(i) ((char*)H(ARG(i)))

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

static int g_open;
static uint32_t g_frames;
static int g_shown;

/* ---- the file ---- */
static FILE* g_f;
static uint32_t* g_index;  /* frames + 1 offsets (key-frame bit cleared) */
static uint32_t g_tag, g_w, g_h, g_fps_num, g_fps_den, g_ntracks, g_maxframe;
static uint16_t g_arate, g_aflags;  /* the first audio track's (the one played) */
static uint8_t* g_pkt;

/* ---- decoding: the decode thread follows the movie clock, decodes each frame that is due (Bink frames depend on the
   one before, so none is skipped), copies the picture's planes into one of 3 slots (never the one published last or
   the one being copied), and feeds the sound to a FIFO the audio thread plays from. The game thread, idle while it
   waits for pictures, converts to BGRx. ---- */
static AVCodecContext *g_vc, *g_ac;
static AVFrame* g_fr;
static pthread_t g_mthread, g_athread;
static int g_athread_on;
static volatile int g_mrun, g_mdone;
static pthread_mutex_t g_mlock;
static uint8_t* g_ring[3];
static int g_pub = -1, g_busy = -1;
static volatile uint32_t g_seq;  /* pictures published */
static uint32_t g_seen;          /* the last one handed to the game */
static int16_t g_af[1 << 19];    /* audio FIFO: 5 s of 48 kHz stereo (Bink sends sound well ahead of the pictures) */
static volatile uint32_t g_aw, g_ar;
static uint32_t g_under, g_decoded;
static uint64_t g_mt0, g_dec_us;
#define AF_N (sizeof g_af / sizeof *g_af)

static int16_t s16(float v) { v *= 32767.f; return (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : v); }
/* one decoded audio frame into the FIFO as interleaved stereo s16 */
static void push_audio(const AVFrame* f) {
    int ch = f->ch_layout.nb_channels, n = f->nb_samples;
    if (ch < 1 || g_aw - g_ar + 2u * n > AF_N) return;  /* full (the audio thread stalled): drop */
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 2; k++) {
            int c = k < ch ? k : 0;
            int16_t v = 0;
            switch (f->format) {
            case AV_SAMPLE_FMT_FLTP: v = s16(((const float*)f->extended_data[c])[i]); break;
            case AV_SAMPLE_FMT_FLT: v = s16(((const float*)f->extended_data[0])[i * ch + c]); break;
            case AV_SAMPLE_FMT_S16P: v = ((const int16_t*)f->extended_data[c])[i]; break;
            case AV_SAMPLE_FMT_S16: v = ((const int16_t*)f->extended_data[0])[i * ch + c]; break;
            }
            g_af[(g_aw + 2 * i + k) & (AF_N - 1)] = v;
        }
    __sync_synchronize();
    g_aw += 2u * n;
}
static void decode(AVCodecContext* cx, const uint8_t* p, uint32_t n, int video) {
    AVPacket* pk = av_packet_alloc();
    if (!pk) return;
    pk->data = (uint8_t*)p, pk->size = (int)n;
    if (avcodec_send_packet(cx, pk) >= 0)
        while (avcodec_receive_frame(cx, g_fr) >= 0) {
            if (!video) { push_audio(g_fr); continue; }
            pthread_mutex_lock(&g_mlock);
            int s = 0;
            while (s == g_pub || s == g_busy) s++;
            pthread_mutex_unlock(&g_mlock);
            uint8_t* o = g_ring[s];  /* Y, U, V planes, tightly packed */
            for (int pl = 0; pl < 3; pl++) {
                int w = pl ? (int)(g_w + 1) / 2 : (int)g_w, h = pl ? (int)(g_h + 1) / 2 : (int)g_h;
                for (int y = 0; y < h; y++) memcpy(o, g_fr->data[pl] + y * g_fr->linesize[pl], w), o += w;
            }
            pthread_mutex_lock(&g_mlock);
            g_pub = s, g_seq++;
            pthread_mutex_unlock(&g_mlock);
        }
    av_packet_free(&pk);
}
/* frame k's packets: per audio track a size and that many bytes (only track 0 is decoded), then the video */
static int read_frame(uint32_t k) {
    uint32_t n = g_index[k + 1] - g_index[k];
    if (n > g_maxframe || fseek(g_f, (long)g_index[k], SEEK_SET) || fread(g_pkt, 1, n, g_f) != n) return 0;
    memset(g_pkt + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    uint32_t at = 0;
    for (uint32_t t = 0; t < g_ntracks; t++) {
        if (at + 4 > n) return 0;
        uint32_t a;
        memcpy(&a, g_pkt + at, 4);
        at += 4;
        if (a > n - at) return 0;
        if (t == 0 && g_ac && a >= 4) decode(g_ac, g_pkt + at, a, 0);
        at += a;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    decode(g_vc, g_pkt + at, n - at, 1);
    g_dec_us += sceKernelGetProcessTimeWide() - t0, g_decoded++;
    return 1;
}
static void* movie_main(void* a) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint32_t next = 0;
    while (g_mrun && next < g_frames) {
        double t = (sceKernelGetProcessTimeWide() - t0) / 1e6;
        uint32_t due = (uint32_t)(t * g_fps_num / g_fps_den) + 1;  /* frames that should have been shown by now */
        if (next >= due) { sceKernelDelayThread(2000); continue; }
        if (!read_frame(next)) break;
        next++;
    }
    g_mdone = 1;
    return NULL;
}
static void* audio_main(void* a) {
    enum { GRAIN = 1024, OUT_RATE = 48000 };  /* a multiple of 64, as SceAudio wants */
    static int16_t buf[GRAIN * 2];
    /* the MAIN port only plays 48 kHz (80260008 at the movies' 22050; the one BGM port is the game's mixer), so the
       movie's audio is resampled (nearest sample) on the way out */
#ifndef USM_VPC
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { vlog("movie: audio port failed %08x\n", port); return NULL; }
#endif
    double step = (double)g_arate / OUT_RATE, pos = 0;  /* source frames per output frame; the fraction carried */
    while (g_mrun) {
        uint32_t need = (uint32_t)(pos + GRAIN * step) + 1;  /* source frames this grain reads */
        if ((g_aw - g_ar) / 2 >= need) {
            for (int i = 0; i < GRAIN; i++) {
                uint32_t s = g_ar + 2 * (uint32_t)(pos + i * step);
                buf[2 * i] = g_af[s & (AF_N - 1)], buf[2 * i + 1] = g_af[(s + 1) & (AF_N - 1)];
            }
            __sync_synchronize();
            double end = pos + GRAIN * step;
            uint32_t whole = (uint32_t)end;
            pos = end - whole, g_ar += 2 * whole;
        } else if (g_mdone) break;
        else memset(buf, 0, sizeof buf), g_under++;  /* not decoded yet: silence keeps the pace */
#ifndef USM_VPC
        sceAudioOutOutput(port, buf);  /* blocks for one grain */
#else
        sceKernelDelayThread(GRAIN * 1000000u / OUT_RATE);  /* vpc: silent */
#endif
    }
#ifndef USM_VPC
    sceAudioOutReleasePort(port);
#endif
    return NULL;
}

static AVCodecContext* codec(enum AVCodecID id, uint32_t tag, uint32_t extra) {
    const AVCodec* d = avcodec_find_decoder(id);
    AVCodecContext* cx = d ? avcodec_alloc_context3(d) : NULL;
    if (!cx) return NULL;
    cx->codec_tag = tag;
    cx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (cx->extradata) memcpy(cx->extradata, &extra, 4), cx->extradata_size = 4;
    if (id == AV_CODEC_ID_BINKVIDEO) cx->width = (int)g_w, cx->height = (int)g_h;
    else cx->sample_rate = g_arate, av_channel_layout_default(&cx->ch_layout, g_aflags & 0x2000 ? 2 : 1);
    if (avcodec_open2(cx, d, NULL) < 0) avcodec_free_context(&cx);
    return cx;
}
static void close_movie(void) {
    if (!g_open) return;
    g_mrun = 0;
    pthread_join(g_mthread, NULL);
    if (g_athread_on) pthread_join(g_athread, NULL), g_athread_on = 0;
    vlog("movie: %.1f s played, %u frames decoded in %.1f ms each, %u audio grains of silence\n",
         (sceKernelGetProcessTimeWide() - g_mt0) / 1e6, g_decoded, g_decoded ? g_dec_us / 1000.0 / g_decoded : 0.0, g_under);
    avcodec_free_context(&g_vc), avcodec_free_context(&g_ac), av_frame_free(&g_fr);
    for (int i = 0; i < 3; i++) free(g_ring[i]), g_ring[i] = NULL;
    free(g_index), g_index = NULL, free(g_pkt), g_pkt = NULL;
    fclose(g_f), g_f = NULL;
    pthread_mutex_destroy(&g_mlock);
    g_open = 0;
}
/* the header as libavformat/bink.c reads it; 0 if it is not a Bink 1 movie we can play */
static int open_movie(const char* path) {
    uint32_t h[11];
    if (!(g_f = fopen(path, "rb"))) return 0;
    if (fread(h, 4, 11, g_f) != 11 || (h[0] & 0xffffff) != ('B' | 'I' << 8 | 'K' << 16) || !h[2] || h[2] > 1000000 ||
        !h[5] || h[5] > 7680 || !h[6] || h[6] > 4800 || !h[7] || !h[8] || h[10] > 256) {
        fclose(g_f), g_f = NULL;
        return 0;
    }
    g_tag = h[0], g_frames = h[2], g_maxframe = h[3], g_w = h[5], g_h = h[6], g_fps_num = h[7], g_fps_den = h[8];
    g_ntracks = h[10];
    uint32_t file_size = h[1] + 8, vflags = h[9];
    if ((g_tag >> 24) == 'k') fseek(g_f, 4, SEEK_CUR);  /* an extra field from revision k on */
    g_arate = 0, g_aflags = 0;
    if (g_ntracks) {
        fseek(g_f, 4 * g_ntracks, SEEK_CUR);  /* max decoded sizes */
        for (uint32_t t = 0; t < g_ntracks; t++) {
            uint16_t r[2] = {0, 0};
            fread(r, 2, 2, g_f);
            if (t == 0) g_arate = r[0], g_aflags = r[1];
        }
        fseek(g_f, 4 * g_ntracks, SEEK_CUR);  /* track ids */
    }
    g_index = malloc(4 * (g_frames + 1));
    if (!g_index || fread(g_index, 4, g_frames, g_f) != g_frames) { free(g_index), g_index = NULL, fclose(g_f), g_f = NULL; return 0; }
    g_index[g_frames] = file_size;
    for (uint32_t i = 0; i <= g_frames; i++) g_index[i] &= ~1u;  /* bit 0 marks key frames */
    if (g_maxframe < 16 || g_maxframe > file_size) g_maxframe = file_size;
    g_pkt = malloc(g_maxframe + AV_INPUT_BUFFER_PADDING_SIZE);
    g_vc = codec(AV_CODEC_ID_BINKVIDEO, g_tag, vflags);
    g_ac = g_ntracks && g_arate ? codec(g_aflags & 0x1000 ? AV_CODEC_ID_BINKAUDIO_DCT : AV_CODEC_ID_BINKAUDIO_RDFT, 0, g_tag) : NULL;
    g_fr = av_frame_alloc();
    size_t planes = (size_t)g_w * g_h + 2 * (size_t)((g_w + 1) / 2) * ((g_h + 1) / 2);
    for (int i = 0; i < 3; i++) g_ring[i] = malloc(planes);
    if (!g_pkt || !g_vc || !g_fr || !g_ring[0] || !g_ring[1] || !g_ring[2]) {
        vlog("movie: decoder setup failed (video %p)\n", (void*)g_vc);
        avcodec_free_context(&g_vc), avcodec_free_context(&g_ac), av_frame_free(&g_fr);
        for (int i = 0; i < 3; i++) free(g_ring[i]), g_ring[i] = NULL;
        free(g_index), g_index = NULL, free(g_pkt), g_pkt = NULL, fclose(g_f), g_f = NULL;
        return 0;
    }
    pthread_mutex_init(&g_mlock, NULL);
    g_mrun = 1, g_mdone = 0, g_seq = g_seen = 0, g_pub = g_busy = -1, g_aw = g_ar = g_under = g_decoded = 0, g_dec_us = 0;
    g_mt0 = sceKernelGetProcessTimeWide();
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024);
    pthread_create(&g_mthread, &at, movie_main, NULL);
    g_athread_on = g_ac && pthread_create(&g_athread, &at, audio_main, NULL) == 0;
    pthread_attr_destroy(&at);
    g_open = 1;
    return 1;
}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
/* the published picture to 32-bit BGRx (BT.601, video range) */
static void copy_picture(uint32_t dest, uint32_t pitch, uint32_t height) {
    pthread_mutex_lock(&g_mlock);
    int s = g_pub;
    g_busy = s, g_seen = g_seq;
    pthread_mutex_unlock(&g_mlock);
    /* the game sizes the buffer from the size BinkOpen reported, so the picture fits */
    if (s >= 0 && pitch >= g_w * 4 && height >= g_h) {
        uint32_t cw = (g_w + 1) / 2;
        const uint8_t *Y = g_ring[s], *U = Y + g_w * g_h, *V = U + cw * ((g_h + 1) / 2);
        for (uint32_t y = 0; y < g_h; y++) {
            uint32_t* out = (uint32_t*)H(dest + y * pitch);
            const uint8_t *yr = Y + y * g_w, *ur = U + (y / 2) * cw, *vr = V + (y / 2) * cw;
            for (uint32_t x = 0; x < g_w; x++) {
                int C = 298 * (yr[x] - 16), u = ur[x / 2] - 128, v = vr[x / 2] - 128;
                out[x] = clamp8((C + 516 * u + 128) >> 8) | clamp8((C - 100 * u - 208 * v + 128) >> 8) << 8 |
                         (uint32_t)clamp8((C + 409 * v + 128) >> 8) << 16 | 0xff000000u;
            }
        }
    }
    pthread_mutex_lock(&g_mlock);
    g_busy = -1;
    pthread_mutex_unlock(&g_mlock);
}

/* ---- the game's Bink calls ---- */
static void BinkOpen_(Ctx* c) {  /* (path, flags) */
    const char *path = S(0), *b = path;
    for (const char* p = path; *p; p++)
        if (*p == '\\' || *p == '/') b = p + 1;
    char base[96], dat[160];
    snprintf(base, sizeof base, "%s", b);
    char* dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(dat, sizeof dat, USM_DIR "Data/movies/pc/%s.dat", base);
    c->eax = 0;
    close_movie();
    g_shown = 0;
    if (!open_movie(dat)) { vlog("movie %s: %s missing or not a Bink 1 movie, skipped\n", base, dat); return; }
    static uint32_t bink;
    if (!bink) bink = G(guest_alloc(64));
    memset(H(bink), 0, 64);
    ST32(bink, g_w), ST32(bink + 4, g_h), ST32(bink + 8, g_frames), ST32(bink + 12, 1);
    vlog("movie %s: %ux%u, %u frames at %.2f fps, %u audio tracks (%u Hz%s)\n", base, g_w, g_h, g_frames,
         (double)g_fps_num / g_fps_den, g_ntracks, g_arate, g_aflags & 0x2000 ? " stereo" : "");
    c->eax = bink;
}

/* 0 = a picture is ready to show */
static void BinkWait_(Ctx* c) {
    uint32_t bink = ARG(0);
    int r = 1;
    if (!g_open) ST32(bink + 12, LD32(bink + 8));
    else if (g_seq != g_seen) r = 0;
    else if (g_mdone) ST32(bink + 12, LD32(bink + 8));  /* every picture shown: the end */
    if (r) sceKernelDelayThread(1000);
    else g_shown++;
    c->eax = (uint32_t)r;
}

static void BinkCopyToBuffer_(Ctx* c) {  /* (bink, dest, pitch, height, x, y, flags) */
    if (g_open) copy_picture(ARG(1), ARG(2), ARG(3));
    c->eax = 0;
}

static void BinkNextFrame_(Ctx* c) {
    uint32_t bink = ARG(0), n = LD32(bink + 12) + 1;
    if (n < g_frames) ST32(bink + 12, n);  /* the last frame number only when the movie ends (BinkWait) */
    c->eax = 0;
}
static void BinkClose_(Ctx* c) {
    vlog("movie closed: %d pictures shown\n", g_shown);
    close_movie();
    c->eax = 0;
}

static void Zero(Ctx* c) { c->eax = 0; }
static void BinkSetSoundSystem_(Ctx* c) { c->eax = 1; }

const ShimDef shims_bink[] = {
    {"_BinkOpen@8", BinkOpen_}, {"_BinkWait@4", BinkWait_}, {"_BinkDoFrame@4", Zero},
    {"_BinkCopyToBuffer@28", BinkCopyToBuffer_}, {"_BinkNextFrame@4", BinkNextFrame_}, {"_BinkClose@4", BinkClose_},
    {"_BinkSetSoundSystem@8", BinkSetSoundSystem_}, {NULL, NULL},
};
