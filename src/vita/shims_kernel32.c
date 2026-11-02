/* KERNEL32 (stdcall: shims.c pops the arguments). Handles are indices into one table of host objects. */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <setjmp.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "vita.h"
#include "vita_util.h"

#define S(i) ((char*)H(ARG(i)))
#define INVALID_HANDLE 0xffffffffu

/* ---- handles: files, mappings, and waitable objects (events, mutexes, semaphores, threads) ---- */
enum { H_FREE, H_FILE, H_MAP, H_WAIT, H_FIND };
typedef struct {
    int kind;
    int fd;                /* H_FILE; H_MAP: the file's handle */
    pthread_mutex_t m;     /* H_WAIT */
    pthread_cond_t cv;
    int signaled, manual;  /* events; a mutex is an auto-reset event that starts signaled; threads signal on exit */
    int count, max;        /* semaphores (max > 0) */
    int suspended;         /* threads made with CREATE_SUSPENDED */
    uint32_t start, param, stack;
    int overlapped;        /* H_FILE opened with FILE_FLAG_OVERLAPPED */
} Obj;
#define MAX_HANDLES 1024
static Obj g_h[MAX_HANDLES];
static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t h_new(int kind) {
    pthread_mutex_lock(&g_hlock);
    for (unsigned i = 1; i < MAX_HANDLES; i++)
        if (g_h[i].kind == H_FREE) {
            memset(&g_h[i], 0, sizeof g_h[i]);
            g_h[i].kind = kind;
            if (kind == H_WAIT) pthread_mutex_init(&g_h[i].m, NULL), pthread_cond_init(&g_h[i].cv, NULL);
            pthread_mutex_unlock(&g_hlock);
            return 0x100 + 4 * i;
        }
    pthread_mutex_unlock(&g_hlock);
    vlog("out of handles\n");
    return 0;
}
static Obj* h_get(uint32_t h) {
    uint32_t i = (h - 0x100) / 4;
    return h >= 0x100 && i < MAX_HANDLES && g_h[i].kind != H_FREE ? &g_h[i] : NULL;
}

static void set_error(Ctx* c, uint32_t e) { ST32(c->fsbase + 0x34, e); }  /* TEB LastErrorValue */
static void GetLastError_(Ctx* c) { c->eax = LD32(c->fsbase + 0x34); }
static void SetLastError_(Ctx* c) { set_error(c, ARG(0)); }

/* ---- files ---- */
static void create_file(Ctx* c, const char* name) {
    char p[512];
    map_path(name, p, sizeof p);
    uint32_t access = ARG(1), disp = ARG(4);
    int fl = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disp == 1) fl |= O_CREAT | O_EXCL;
    else if (disp == 2) fl |= O_CREAT | O_TRUNC;
    else if (disp == 4) fl |= O_CREAT;
    else if (disp == 5) fl |= O_TRUNC;
    int fd = open(p, fl, 0666);
    if (fd < 0) { vlog("CreateFile %s: not found\n", p); set_error(c, 2); c->eax = INVALID_HANDLE; return; }  /* ERROR_FILE_NOT_FOUND */
    uint32_t h = h_new(H_FILE);
    h_get(h)->fd = fd;
    h_get(h)->overlapped = (ARG(5) & 0x40000000u) != 0;  /* FILE_FLAG_OVERLAPPED */
    set_error(c, 0);
    c->eax = h;
}
static void CreateFileA_(Ctx* c) { create_file(c, S(0)); }
static void narrow(uint32_t w, char* out, unsigned n) {  /* UTF-16 guest string to ASCII */
    unsigned k = 0;
    for (; LD16(w) && k < n - 1; w += 2) out[k++] = (char)(LD16(w) < 0x80 ? LD16(w) : '?');
    out[k] = 0;
}
static void CreateFileW_(Ctx* c) { char p[512]; narrow(ARG(0), p, sizeof p); create_file(c, p); }
static void WideCharToMultiByte_(Ctx* c) {  /* (cp, flags, wstr, wlen, out, outlen, default, used) */
    uint32_t w = ARG(2), out = ARG(4), cap = ARG(5);
    int32_t len = (int32_t)ARG(3);
    uint32_t n = 0;
    if (len < 0) { while (LD16(w + 2 * n)) n++; n++; }  /* -1: through the terminator */
    else n = (uint32_t)len;
    if (cap) for (uint32_t i = 0; i < n && i < cap; i++) ST8(out + i, LD16(w + 2 * i) < 0x80 ? LD16(w + 2 * i) : '?');
    c->eax = cap && n > cap ? 0 : n;
}
static void SetEndOfFile_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    c->eax = o && o->kind == H_FILE && ftruncate(o->fd, lseek(o->fd, 0, SEEK_CUR)) == 0;
}
static void SetFilePointerEx_(Ctx* c) {  /* (h, distance lo, distance hi, newpos*, method) */
    Obj* o = h_get(ARG(0));
    off_t r = o ? lseek(o->fd, (off_t)(int32_t)ARG(1), (int)ARG(4)) : -1;
    if (ARG(3)) ST32(ARG(3), (uint32_t)r), ST32(ARG(3) + 4, 0);
    c->eax = r >= 0;
}
static void RemoveDirectoryA_(Ctx* c) {
    char p[512];
    map_path(S(0), p, sizeof p);
    c->eax = rmdir(p) == 0;
}
static void GetCurrentDirectoryA_(Ctx* c) {
    if (ARG(1)) strncpy(S(1), GAME_CWD, ARG(0));
    c->eax = (uint32_t)strlen(GAME_CWD);
}
static void GetFullPathNameA_(Ctx* c) {  /* (name, size, out, filepart*) */
    char b[512];
    const char* n = S(0);
    if (n[0] && n[1] == ':') snprintf(b, sizeof b, "%s", n);
    else snprintf(b, sizeof b, GAME_CWD "\\%s", n);
    strncpy(S(2), b, ARG(1));
    if (ARG(3)) { char* s = strrchr(S(2), '\\'); ST32(ARG(3), s ? G(s + 1) : ARG(2)); }
    c->eax = (uint32_t)strlen(b);
}
static void lstrcmpi_(Ctx* c) { c->eax = (uint32_t)strcasecmp(S(0), S(1)); }

/* ---- directory listing: FindFirstFile/FindNextFile with * and ? (case-insensitive, like Windows) ---- */
typedef struct { DIR* d; char pat[260], dir[512]; } Find;
static int wild(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return wild(p + 1, s) || (*s && wild(p, s + 1));
    return *s && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) && wild(p + 1, s + 1);
}
/* WIN32_FIND_DATAA: attributes, 3 FILETIMEs, size hi, size lo, 2 reserved, name[260] at 44, alt name[14] */
static int find_next(Find* f, uint32_t out) {
    struct dirent* e;
    while ((e = readdir(f->d)))
        if (wild(f->pat, e->d_name)) {
            char full[800];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
            memset(H(out), 0, 320);
            int isdir = !stat(full, &st) && S_ISDIR(st.st_mode);
            ST32(out, isdir ? 0x10 : 0x80);
            uint64_t ft = (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull;  /* save slots show these */
            for (int i = 0; i < 3; i++) ST32(out + 4 + 8 * i, (uint32_t)ft), ST32(out + 8 + 8 * i, (uint32_t)(ft >> 32));
            ST32(out + 32, isdir ? 0 : (uint32_t)st.st_size);
            strncpy((char*)H(out + 44), e->d_name, 259);
            return 1;
        }
    return 0;
}
static void find_first(Ctx* c, const char* pattern, uint32_t out) {
    char p[512];
    map_path(pattern, p, sizeof p);
    char* slash = strrchr(p, '/');
    Find* f = guest_alloc(sizeof(Find));
    snprintf(f->pat, sizeof f->pat, "%s", slash ? slash + 1 : p);
    if (slash) *slash = 0;
    snprintf(f->dir, sizeof f->dir, "%s", p);
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, out)) {
        if (f->d) closedir(f->d);
        set_error(c, 2);
        c->eax = INVALID_HANDLE;
        return;
    }
    uint32_t h = h_new(H_FIND);
    h_get(h)->fd = (int)G(f);
    c->eax = h;
}
static void FindFirstFileA_(Ctx* c) { find_first(c, S(0), ARG(1)); }
static void FindFirstFileExA_(Ctx* c) { find_first(c, S(0), ARG(2)); }  /* (name, level, data, op, filter, flags) */
static void FindNextFileA_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    c->eax = o && o->kind == H_FIND && find_next(H((uint32_t)o->fd), ARG(1));
    if (!c->eax) set_error(c, 18);  /* ERROR_NO_MORE_FILES */
}
static void FindClose_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    if (o && o->kind == H_FIND) closedir(((Find*)H((uint32_t)o->fd))->d), o->kind = H_FREE;
    c->eax = 1;
}

/* ---- asynchronous I/O: ReadFileEx/WriteFileEx go to an I/O thread (core 1), which does the transfer and queues the
   completion routine for the thread that asked; it runs when that thread next waits alertably (SleepEx), as on
   Windows. The city streamer reads this way on the game thread: done at once, its reads stalled the game when
   swinging fast, and a read it cancelled (CancelIo) still landed and completed as a success, feeding it a stale
   buffer (the crash at 4c1fa0). Now a cancelled or closed request not yet started completes as aborted. ---- */
typedef struct { uint32_t fn, err, bytes, ov; } Apc;
typedef struct {  /* a thread's completions, filled by the I/O thread */
    Apc* q;
    unsigned n, cap;
    pthread_mutex_t m;
    pthread_cond_t cv;
} ApcQueue;
typedef struct IoReq { struct IoReq* next; ApcQueue* to; uint32_t h, buf, n, ov, fn; int fd, write_; } IoReq;
static IoReq *g_io_head, *g_io_tail;
static pthread_mutex_t g_io_m = PTHREAD_MUTEX_INITIALIZER;    /* the request list */
static pthread_mutex_t g_io_busy = PTHREAD_MUTEX_INITIALIZER; /* held while a transfer runs: CloseHandle waits on it */
static pthread_cond_t g_io_cv = PTHREAD_COND_INITIALIZER;
static uint32_t g_io_n, g_io_aborted, g_io_depth, g_io_maxdepth;  /* the log */
static pthread_key_t g_apc_key;
static pthread_once_t g_apc_once = PTHREAD_ONCE_INIT;
static void apc_key(void) { pthread_key_create(&g_apc_key, NULL); }
static ApcQueue* apc_queue(void) {
    pthread_once(&g_apc_once, apc_key);
    ApcQueue* q = pthread_getspecific(g_apc_key);
    if (!q) {
        q = calloc(1, sizeof *q);
        pthread_mutex_init(&q->m, NULL), pthread_cond_init(&q->cv, NULL);
        pthread_setspecific(g_apc_key, q);
    }
    return q;
}
static void apc_push(ApcQueue* q, Apc a) {
    pthread_mutex_lock(&q->m);
    if (q->n == q->cap) {  /* a dropped completion leaves the read pending forever */
        Apc* na = realloc(q->q, (q->cap = q->cap ? q->cap * 2 : 64) * sizeof(Apc));
        if (!na) { vlog("ReadFileEx: out of memory for completions\n"); abort(); }
        q->q = na;
    }
    q->q[q->n++] = a;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->m);
}
void apc_run(Ctx* c) {
    (void)c;
    ApcQueue* q = apc_queue();
    for (;;) {
        pthread_mutex_lock(&q->m);
        if (!q->n) { pthread_mutex_unlock(&q->m); return; }
        Apc a = q->q[0];
        memmove(q->q, q->q + 1, --q->n * sizeof(Apc));
        pthread_mutex_unlock(&q->m);
        uint32_t args[3] = {a.err, a.bytes, a.ov};
        guest_call(a.fn, 3, args);
    }
}
static void* io_main(void* arg) {
    (void)arg;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x20000);  /* core 1: the renderer's, half idle */
    for (;;) {
        pthread_mutex_lock(&g_io_m);
        while (!g_io_head) pthread_cond_wait(&g_io_cv, &g_io_m);
        IoReq* r = g_io_head;
        pthread_mutex_lock(&g_io_busy);  /* before the request leaves the list: CloseHandle sees it one way or the other */
        if (!(g_io_head = r->next)) g_io_tail = NULL;
        g_io_depth--;
        pthread_mutex_unlock(&g_io_m);
        off_t at = (off_t)LD32(r->ov + 8);
        int n = r->write_ ? (int)pwrite(r->fd, H(r->buf), r->n, at) : (int)pread(r->fd, H(r->buf), r->n, at);
        pthread_mutex_unlock(&g_io_busy);
        if (n < 0) n = 0;
        ST32(r->ov + 4, (uint32_t)n), ST32(r->ov, 0);  /* Internal: STATUS_SUCCESS */
        apc_push(r->to, (Apc){r->fn, 0, (uint32_t)n, r->ov});
        free(r);
    }
    return NULL;
}
/* requests of handle h not yet started (from thread q, or any when q is NULL) complete as ERROR_OPERATION_ABORTED */
static void io_cancel(uint32_t h, ApcQueue* q) {
    pthread_mutex_lock(&g_io_m);
    IoReq **p = &g_io_head, *last = NULL;
    while (*p) {
        IoReq* r = *p;
        if (r->h == h && (!q || r->to == q)) {
            *p = r->next;
            g_io_depth--, g_io_aborted++;
            ST32(r->ov + 4, 0), ST32(r->ov, 0xc0000120u);  /* STATUS_CANCELLED */
            apc_push(r->to, (Apc){r->fn, 995, 0, r->ov});
            free(r);
        } else
            last = r, p = &r->next;
    }
    g_io_tail = last;
    pthread_mutex_unlock(&g_io_m);
}
static void file_ex(Ctx* c, int write_) {  /* (h, buf, n, ov, completion) */
    Obj* o = h_get(ARG(0));
    if (!o || o->kind != H_FILE) { set_error(c, 6); c->eax = 0; return; }  /* ERROR_INVALID_HANDLE */
    static int started;
    if (!started) {
        pthread_t io;
        pthread_attr_t at;
        pthread_attr_init(&at), pthread_attr_setstacksize(&at, 0x10000);
        started = pthread_create(&io, &at, io_main, NULL) == 0;
        if (!started) vlog("ReadFileEx: no I/O thread\n");
    }
    IoReq* r = malloc(sizeof *r);
    *r = (IoReq){NULL, apc_queue(), ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), o->fd, write_};
    ST32(r->ov, 0x103), ST32(r->ov + 4, 0);  /* STATUS_PENDING */
    pthread_mutex_lock(&g_io_m);
    if (g_io_tail) g_io_tail->next = r;
    else g_io_head = r;
    g_io_tail = r;
    if (++g_io_depth > g_io_maxdepth) g_io_maxdepth = g_io_depth;
    if (!(++g_io_n & 63) || g_io_n == 1)
        vlog("async I/O: %u requests, %u aborted, up to %u waiting\n", g_io_n, g_io_aborted, g_io_maxdepth);
    pthread_cond_signal(&g_io_cv);
    pthread_mutex_unlock(&g_io_m);
    c->eax = 1;
}
static void ReadFileEx_(Ctx* c) { file_ex(c, 0); }
static void WriteFileEx_(Ctx* c) { file_ex(c, 1); }
static void CancelIo_(Ctx* c) { io_cancel(ARG(0), apc_queue()); c->eax = 1; }
static void SleepEx_(Ctx* c) {  /* (ms, alertable): WAIT_IO_COMPLETION (0xc0) if completions ran */
    uint32_t ms = ARG(0);
    if (ARG(1)) {
        ApcQueue* q = apc_queue();
        pthread_mutex_lock(&q->m);
        if (!q->n && ms) {  /* wait for a completion or the time */
            struct timeval tv;
            gettimeofday(&tv, NULL);
            struct timespec ts = {tv.tv_sec, tv.tv_usec * 1000};
            uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)(ms == 0xffffffffu ? 1000 : ms) * 1000000u;
            ts.tv_sec += (time_t)(ns / 1000000000u), ts.tv_nsec = (long)(ns % 1000000000u);
            pthread_cond_timedwait(&q->cv, &q->m, &ts);
        }
        int any = q->n != 0;
        pthread_mutex_unlock(&q->m);
        if (any) { apc_run(c); c->eax = 0xc0; return; }
        if (!ms) sceKernelDelayThread(100);
        c->eax = 0;
        return;
    }
    sceKernelDelayThread(ms ? (ms == 0xffffffffu ? 1000 : ms * 1000) : 100);
    c->eax = 0;
}
static void SwitchToThread_(Ctx* c) { sceKernelDelayThread(0); c->eax = 1; }

/* ---- profile (.ini) files: read for real, writes ignored ---- */
static int ini_get(const char* file, const char* app, const char* key, char* out, unsigned n) {
    char p[512], line[512], sect[128] = "";
    map_path(file, p, sizeof p);
    FILE* f = fopen(p, "r");
    if (!f) return 0;
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char* s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '[') { char* e = strchr(s, ']'); if (e) *e = 0, snprintf(sect, sizeof sect, "%s", s + 1); continue; }
        char* eq = strchr(s, '=');
        if (!eq || strcasecmp(sect, app)) continue;
        char* ke = eq;
        while (ke > s && isspace((unsigned char)ke[-1])) ke--;
        *ke = 0;
        if (strcasecmp(s, key)) continue;
        char* v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(out, n, "%s", v);
        found = 1;
    }
    fclose(f);
    return found;
}
static void GetPrivateProfileStringA_(Ctx* c) {  /* (app, key, default, out, size, file) */
    char v[512];
    const char* r = ARG(0) && ARG(1) && ini_get(S(5), S(0), S(1), v, sizeof v) ? v : ARG(2) ? S(2) : "";
    strncpy(S(3), r, ARG(4));
    if (ARG(4)) ST8(ARG(3) + ARG(4) - 1, 0);
    c->eax = (uint32_t)strlen(S(3));
}
static void GetPrivateProfileIntA_(Ctx* c) {  /* (app, key, default, file) */
    char v[64];
    c->eax = ini_get(S(3), S(0), S(1), v, sizeof v) ? (uint32_t)atoi(v) : ARG(2);
}
static void GetProfileStringA_(Ctx* c) {  /* (app, key, default, out, size): win.ini is empty */
    strncpy(S(3), ARG(2) ? S(2) : "", ARG(4));
    c->eax = (uint32_t)strlen(S(3));
}
static void GetProfileIntA_(Ctx* c) { c->eax = ARG(2); }
static void True1_(Ctx* c) { c->eax = 1; }
static void Zero0_(Ctx* c) { c->eax = 0; }

static void GetSystemInfo_(Ctx* c) {  /* SYSTEM_INFO: one Pentium-class processor, 4 KB pages */
    uint32_t v[9] = {0, 4096, 0x10000, 0x7ffeffff, 1, 1, 586, 0x10000, 6 | 0x0603u << 16};
    memcpy(H(ARG(0)), v, 36);
}
static void IsProcessorFeaturePresent_(Ctx* c) { c->eax = 0; }  /* no MMX/SSE/3DNow!: the translated paths */
static void FileTimeToLocalFileTime_(Ctx* c) { ST32(ARG(1), LD32(ARG(0))), ST32(ARG(1) + 4, LD32(ARG(0) + 4)); c->eax = 1; }
static void FileTimeToSystemTime_(Ctx* c) {
    uint64_t ft = LD32(ARG(0)) | (uint64_t)LD32(ARG(0) + 4) << 32;
    time_t t = (time_t)((ft - 116444736000000000ull) / 10000000ull);
    struct tm* tm = ft >= 116444736000000000ull ? gmtime(&t) : NULL;
    if (!tm) { c->eax = 0; return; }  /* before 1970 or out of range */
    uint16_t v[8] = {(uint16_t)(tm->tm_year + 1900), (uint16_t)(tm->tm_mon + 1), (uint16_t)tm->tm_wday, (uint16_t)tm->tm_mday,
                     (uint16_t)tm->tm_hour, (uint16_t)tm->tm_min, (uint16_t)tm->tm_sec, (uint16_t)(ft / 10000 % 1000)};
    memcpy(H(ARG(1)), v, 16);
    c->eax = 1;
}
static void FormatMessageA_(Ctx* c) {  /* (flags, source, id, lang, out, size, args) */
    char m[64];
    snprintf(m, sizeof m, "error %u", ARG(2));
    if (ARG(0) & 0x100) ST32(ARG(4), G(gstrdup(m)));  /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
    else strncpy(S(4), m, ARG(5));
    c->eax = (uint32_t)strlen(m);
}
static void ExitThread_(Ctx* c);
static void ReadFile_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    uint32_t ov = ARG(4);
    if (!o || o->kind != H_FILE) { c->eax = 0; return; }
    int n = ov ? (int)pread(o->fd, H(ARG(1)), ARG(2), (off_t)LD32(ov + 8))  /* OVERLAPPED.Offset: done at once */
               : (int)read(o->fd, H(ARG(1)), ARG(2));
    if (n < 0) n = 0;
    if (ARG(3)) ST32(ARG(3), (uint32_t)n);
    if (ov) {
        ST32(ov, 0), ST32(ov + 4, (uint32_t)n);  /* Internal = STATUS_SUCCESS, InternalHigh = bytes */
        Obj* e = h_get(LD32(ov + 16));
        if (e && e->kind == H_WAIT) pthread_mutex_lock(&e->m), e->signaled = 1, pthread_cond_broadcast(&e->cv), pthread_mutex_unlock(&e->m);
        /* an overlapped handle reports ERROR_IO_PENDING as Windows does: the stream reader (81f18e) only moves its
           file position on that, a plain success made it read the same music/voice chunk forever */
        if (o->overlapped) { set_error(c, 997); c->eax = 0; return; }
    }
    c->eax = 1;
}
static void GetOverlappedResult_(Ctx* c) { ST32(ARG(2), LD32(ARG(1) + 4)); c->eax = 1; }
static void WriteFile_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    int n = o && o->kind == H_FILE ? (int)write(o->fd, H(ARG(1)), ARG(2)) : -1;
    if (ARG(3)) ST32(ARG(3), n < 0 ? 0 : (uint32_t)n);
    c->eax = n >= 0;
}
static void GetFileSize_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    struct stat st;
    if (!o || fstat(o->fd, &st)) { c->eax = INVALID_HANDLE; return; }
    if (ARG(1)) ST32(ARG(1), 0);
    c->eax = (uint32_t)st.st_size;
}
static void SetFilePointer_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    off_t r = o ? lseek(o->fd, (off_t)(int32_t)ARG(1), (int)ARG(3)) : -1;
    if (ARG(2)) ST32(ARG(2), 0);
    c->eax = r < 0 ? INVALID_HANDLE : (uint32_t)r;
}
static void CloseHandle_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    if (o && o->kind == H_FILE) {  /* its queued async requests are aborted, a running one finishes first */
        io_cancel(ARG(0), NULL);
        pthread_mutex_lock(&g_io_busy), close(o->fd), pthread_mutex_unlock(&g_io_busy);
    }
    if (o && o->kind != H_WAIT) o->kind = H_FREE;  /* waitable objects stay: a thread may still signal one */
    c->eax = o != NULL;
}
static void GetFileAttributesA_(Ctx* c) {
    char p[512];
    struct stat st;
    map_path(S(0), p, sizeof p);
    c->eax = stat(p, &st) ? INVALID_HANDLE : S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
static void CreateDirectoryA_(Ctx* c) {
    char p[512];
    map_path(S(0), p, sizeof p);
    c->eax = mkdir(p, 0777) == 0;
}
static void DeleteFileA_(Ctx* c) {
    char p[512];
    map_path(S(0), p, sizeof p);
    c->eax = remove(p) == 0;
}
static void GetDiskFreeSpaceExA_(Ctx* c) {
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) ST32(ARG(i), 0), ST32(ARG(i) + 4, 1);  /* 4 GB */
    c->eax = 1;
}

/* file mappings: the view is a copy of the file read into memory */
static void CreateFileMappingA_(Ctx* c) {
    Obj* f = h_get(ARG(0));
    if (!f || f->kind != H_FILE) { c->eax = 0; return; }
    uint32_t h = h_new(H_MAP);
    h_get(h)->fd = f->fd;
    c->eax = h;
}
static void MapViewOfFile_(Ctx* c) {
    Obj* m = h_get(ARG(0));
    struct stat st;
    if (!m || m->kind != H_MAP || fstat(m->fd, &st)) { c->eax = 0; return; }
    uint32_t off = ARG(3), n = ARG(4) ? ARG(4) : (uint32_t)st.st_size - off;
    void* p = memalign(4096, n);
    pread(m->fd, p, n, off);
    c->eax = GA(p);
}
static void UnmapViewOfFile_(Ctx* c) { free(H(ARG(0))); c->eax = 1; }

/* ---- time ---- */
static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }
static void GetTickCount_(Ctx* c) { c->eax = (uint32_t)(now_us() / 1000); }
static void QueryPerformanceCounter_(Ctx* c) { uint64_t t = now_us(); ST32(ARG(0), (uint32_t)t), ST32(ARG(0) + 4, (uint32_t)(t >> 32)); c->eax = 1; }
static void QueryPerformanceFrequency_(Ctx* c) { ST32(ARG(0), 1000000), ST32(ARG(0) + 4, 0); c->eax = 1; }
static void GetSystemTimeAsFileTime_(Ctx* c) {
    uint64_t t = (uint64_t)time(NULL) * 10000000ull + 116444736000000000ull;
    ST32(ARG(0), (uint32_t)t), ST32(ARG(0) + 4, (uint32_t)(t >> 32));
}
static void GetLocalTime_(Ctx* c) {
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    uint16_t v[8] = {(uint16_t)(tm->tm_year + 1900), (uint16_t)(tm->tm_mon + 1), (uint16_t)tm->tm_wday, (uint16_t)tm->tm_mday,
                     (uint16_t)tm->tm_hour, (uint16_t)tm->tm_min, (uint16_t)tm->tm_sec, 0};
    memcpy(H(ARG(0)), v, 16);
}
static void Sleep_(Ctx* c) { sceKernelDelayThread(ARG(0) ? ARG(0) * 1000 : 100); }

/* ---- process and modules ---- */
static void GetCurrentProcessId_(Ctx* c) { c->eax = 0x1234; }
static void GetCurrentThreadId_(Ctx* c) { c->eax = (uint32_t)sceKernelGetThreadId(); }
static void GetCurrentThread_(Ctx* c) { c->eax = 0xfffffffeu; }
static void GetCurrentProcess_(Ctx* c) { c->eax = 0xffffffffu; }
static void GetModuleHandleA_(Ctx* c) { c->eax = ARG(0) ? 0x10000000u : 0x400000u; }
static void GetModuleFileNameA_(Ctx* c) {
    strncpy(S(1), GAME_CWD "\\USM.exe", ARG(2));
    c->eax = (uint32_t)strlen(S(1));
}
static void GetCommandLineA_(Ctx* c) {
    static uint32_t s;
    if (!s) s = G(gstrdup("\"" GAME_CWD "\\USM.exe\""));
    c->eax = s;
}
static void GetStartupInfoA_(Ctx* c) { memset(H(ARG(0)), 0, 68); ST32(ARG(0), 68); }
static void GetSystemDirectoryA_(Ctx* c) {
    strncpy(S(0), "C:\\WINDOWS\\system32", ARG(1));
    c->eax = (uint32_t)strlen(S(0));
}
static void GetVersionExA_(Ctx* c) {  /* Windows XP SP2 */
    uint32_t p = ARG(0);
    ST32(p + 4, 5), ST32(p + 8, 1), ST32(p + 12, 2600), ST32(p + 16, 2);
    strcpy((char*)H(p + 20), "Service Pack 2");
    c->eax = 1;
}
static void GlobalMemoryStatus_(Ctx* c) {
    uint32_t p = ARG(0);
    uint32_t v[8] = {32, 50, 512u << 20, 256u << 20, 1024u << 20, 768u << 20, 2047u << 20, 1800u << 20};
    memcpy(H(p), v, 32);
}
/* LoadLibrary hands out a fake module; GetProcAddress binds a shim like the import table does */
static void LoadLibraryA_(Ctx* c) {  /* the handle is the DLL's file name (the game passes full system paths) */
    const char* n = S(0);
    for (const char* p = n; *p; p++)
        if (*p == '\\' || *p == '/') n = p + 1;
    vlog("LoadLibrary %s\n", S(0));
    c->eax = G(gstrdup(n));
}
static void FreeLibrary_(Ctx* c) { c->eax = 1; }
/* every name the game looks up (its GetProcAddress calls), with the stdcall bytes each pops: a wrong pop shifts the
   guest stack, which is what tripped the game's /GS check after SHGetFolderPathA */
static void GetProcAddress_(Ctx* c) {
    static const struct { const char* name; int pops; } k[] = {{"DirectInput8Create", 20}, {"Direct3DCreate9", 4},
                                                               {"SHGetFolderPathA", 20}, {"DebugSetMute", 0},
                                                               {"DirectSoundCreate8", 12}};
    const char* dll = ARG(0) > 0x10000 && ARG(0) != 0x400000 ? S(0) : "?";
    const char* name = ARG(1) > 0xffff ? S(1) : "#ordinal";
    int pops = -1;
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++)
        if (!strcmp(name, k[i].name)) pops = k[i].pops;
    for (unsigned i = 0; i < g_nimports && pops < 0; i++)
        if (!strcmp(g_imports[i].name, name)) pops = g_imports[i].pops;
    vlog("GetProcAddress %s!%s%s\n", dll, name, pops < 0 ? " (argument size unknown: assuming cdecl)" : "");
    c->eax = shim_addr(dll, name, pops < 0 ? 0 : pops);
}
static void OutputDebugStringA_(Ctx* c) { vlog("[debug] %s", S(0)); }
static void ExitProcess_(Ctx* c) {
    vlog("ExitProcess(%d)\n", (int)ARG(0));
    sceKernelExitProcess((int)ARG(0));
}

/* ---- memory ---- */
static void VirtualAlloc_(Ctx* c) {
    if (ARG(0)) { c->eax = ARG(0); return; }  /* committing what an earlier call reserved: already backed */
    void* p = game_mem(memalign(4096, ARG(1)));
    if (p) memset(p, 0, ARG(1));
    c->eax = GA(p);
}
static void VirtualFree_(Ctx* c) { if (ARG(2) & 0x8000) game_free(H(ARG(0))); c->eax = 1; }  /* MEM_RELEASE */
static void VirtualQuery_(Ctx* c) {
    uint32_t a = ARG(0), m = ARG(1);
    /* committed read-write private if mapped, else free: the game's allocator (592d50) reads the page only if committed */
    int mapped = sceKernelFindMemBlockByAddr(H(a & ~0xfffu), 0) >= 0;
    uint32_t v[7] = {a & ~0xfffu, a & ~0xfffu, 4, 0x1000, mapped ? 0x1000 : 0x10000, mapped ? 4 : 1, 0x20000};
    memcpy(H(m), v, 28);
    c->eax = 28;
}
static void VirtualProtect_(Ctx* c) { if (ARG(3)) ST32(ARG(3), 4); c->eax = 1; }
static void HeapAlloc_(Ctx* c) { void* p = game_mem(malloc(ARG(2) ? ARG(2) : 1)); if (p && ARG(1) & 8) memset(p, 0, ARG(2)); c->eax = GA(p); }
static void HeapReAlloc_(Ctx* c) { c->eax = GA(game_realloc(H(ARG(2)), ARG(3))); }
static void HeapFree_(Ctx* c) { game_free(H(ARG(2))); c->eax = 1; }
static void HeapSize_(Ctx* c) { c->eax = (uint32_t)malloc_usable_size(H(ARG(2))); }
static void GetProcessHeap_(Ctx* c) { c->eax = 0x500; }
static void HeapCreate_(Ctx* c) { c->eax = 0x504; }
static void GlobalAlloc_(Ctx* c) { void* p = game_mem(malloc(ARG(1) ? ARG(1) : 1)); if (p && ARG(0) & 0x40) memset(p, 0, ARG(1)); c->eax = GA(p); }
static void GlobalLock_(Ctx* c) { c->eax = ARG(0); }
static void GlobalUnlock_(Ctx* c) { c->eax = 1; }
static void GlobalFree_(Ctx* c) { game_free(H(ARG(0))); c->eax = 0; }

/* ---- synchronisation ---- */
static pthread_mutex_t* cs_mutex(Ctx* c, uint32_t cs) {
    uint32_t m = LD32(cs);  /* DebugInfo holds our mutex: the game never reads it */
    if (m) return H(m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t* p = guest_alloc(sizeof *p);
    pthread_mutex_init(p, &a);
    ST32(cs, GA(p));
    return p;
}
static void InitializeCriticalSection_(Ctx* c) { memset(H(ARG(0)), 0, 24); cs_mutex(c, ARG(0)); c->eax = 1; }
static void EnterCriticalSection_(Ctx* c) { pthread_mutex_lock(cs_mutex(c, ARG(0))); }
static void LeaveCriticalSection_(Ctx* c) { pthread_mutex_unlock(cs_mutex(c, ARG(0))); }
static void TryEnterCriticalSection_(Ctx* c) { c->eax = pthread_mutex_trylock(cs_mutex(c, ARG(0))) == 0; }
static void DeleteCriticalSection_(Ctx* c) { (void)c; }

static void InterlockedIncrement_(Ctx* c) { c->eax = __atomic_add_fetch((uint32_t*)H(ARG(0)), 1, __ATOMIC_SEQ_CST); }
static void InterlockedDecrement_(Ctx* c) { c->eax = __atomic_sub_fetch((uint32_t*)H(ARG(0)), 1, __ATOMIC_SEQ_CST); }
static void InterlockedExchange_(Ctx* c) { c->eax = __atomic_exchange_n((uint32_t*)H(ARG(0)), ARG(1), __ATOMIC_SEQ_CST); }
static void InterlockedExchangeAdd_(Ctx* c) { c->eax = __atomic_fetch_add((uint32_t*)H(ARG(0)), ARG(1), __ATOMIC_SEQ_CST); }
static void InterlockedCompareExchange_(Ctx* c) {
    uint32_t expected = ARG(2);
    __atomic_compare_exchange_n((uint32_t*)H(ARG(0)), &expected, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    c->eax = expected;
}

static void CreateEventA_(Ctx* c) {
    uint32_t h = h_new(H_WAIT);
    Obj* o = h_get(h);
    o->manual = (int)ARG(1), o->signaled = (int)ARG(2);
    set_error(c, 0);
    c->eax = h;
}
static void CreateMutexA_(Ctx* c) {
    uint32_t h = h_new(H_WAIT);
    h_get(h)->signaled = !ARG(1);  /* not owned yet: free to take */
    set_error(c, 0);               /* never ERROR_ALREADY_EXISTS: no second copy runs on the Vita */
    c->eax = h;
}
static void CreateSemaphoreA_(Ctx* c) {
    uint32_t h = h_new(H_WAIT);
    Obj* o = h_get(h);
    o->count = (int)ARG(1), o->max = (int)ARG(2) ? (int)ARG(2) : 1;
    c->eax = h;
}
static void obj_signal(Obj* o, int on) {
    pthread_mutex_lock(&o->m);
    o->signaled = on;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
}
static void SetEvent_(Ctx* c) { Obj* o = h_get(ARG(0)); if (o && o->kind == H_WAIT) obj_signal(o, 1); c->eax = o != NULL; }
static void ResetEvent_(Ctx* c) { Obj* o = h_get(ARG(0)); if (o && o->kind == H_WAIT) obj_signal(o, 0); c->eax = o != NULL; }
static void ReleaseMutex_(Ctx* c) { SetEvent_(c); }
static void ReleaseSemaphore_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    if (!o) { c->eax = 0; return; }
    pthread_mutex_lock(&o->m);
    if (ARG(2)) ST32(ARG(2), (uint32_t)o->count);
    o->count += (int)ARG(1);
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    c->eax = 1;
}

/* 0 = WAIT_OBJECT_0, 0x102 = WAIT_TIMEOUT */
static uint32_t wait_one(Obj* o, uint32_t ms) {
    if (!o || o->kind != H_WAIT) return 0;  /* files and others count as signaled */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000, until.tv_nsec += (long)(ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
    pthread_mutex_lock(&o->m);
    int sem = o->max > 0;
    while (sem ? o->count <= 0 : !o->signaled) {
        if (ms == 0 || (ms != 0xffffffffu && pthread_cond_timedwait(&o->cv, &o->m, &until))) {
            pthread_mutex_unlock(&o->m);
            return 0x102;
        }
        if (ms == 0xffffffffu) pthread_cond_wait(&o->cv, &o->m);
    }
    if (sem) o->count--;
    else if (!o->manual) o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return 0;
}
static void WaitForSingleObject_(Ctx* c) { c->eax = wait_one(h_get(ARG(0)), ARG(1)); }
static void WaitForMultipleObjects_(Ctx* c) {
    uint32_t n = ARG(0), hs = ARG(1), all = ARG(2), ms = ARG(3);
    uint64_t end = now_us() + (uint64_t)ms * 1000;
    for (;;) {  /* polls: waits on several objects are rare (streaming threads) */
        for (uint32_t i = 0; i < n; i++)
            if (!all && wait_one(h_get(LD32(hs + 4 * i)), 0) == 0) { c->eax = i; return; }
        if (all) {
            for (uint32_t i = 0; i < n; i++) wait_one(h_get(LD32(hs + 4 * i)), 0xffffffffu);
            c->eax = 0;
            return;
        }
        if (ms != 0xffffffffu && now_us() >= end) { c->eax = 0x102; return; }
        sceKernelDelayThread(500);
    }
}

/* ---- threads: each runs the guest start routine on its own context; the handle signals when it ends ---- */
static pthread_key_t g_exit_key;  /* the jmp_buf ExitThread returns through, per guest thread */
static pthread_once_t g_exit_once = PTHREAD_ONCE_INIT;
static void exit_key(void) { pthread_key_create(&g_exit_key, NULL); }
static void* thread_main(void* arg) {
    Obj* o = arg;
    pthread_mutex_lock(&o->m);
    while (o->suspended) pthread_cond_wait(&o->cv, &o->m);
    pthread_mutex_unlock(&o->m);
    /* core 3 (streaming, sound decoding, device watch): 0 is the game loop's, 1 the renderer's, 2 the pipeline
       worker's. Core 1 if the system keeps core 3: on core 2 they kept pausing the worker mid-draw, and the renderer
       on core 1 is idle about half the frame */
    int r3 = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x80000);
    static int said;
    if (!said++) vlog("game threads on core %d (core 3: %08x)\n", r3 < 0 ? 1 : 3, (unsigned)r3);
    if (r3 < 0) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x20000);
    ctx_new(o->stack);
    jmp_buf out;
    pthread_once(&g_exit_once, exit_key);
    pthread_setspecific(g_exit_key, &out);
    if (!setjmp(out)) guest_call(o->start, 1, &o->param);
    obj_signal(o, 1);
    return NULL;
}
static void ExitThread_(Ctx* c) {
    pthread_once(&g_exit_once, exit_key);
    jmp_buf* out = pthread_getspecific(g_exit_key);
    if (!out) { vlog("ExitThread on the main thread\n"); exit((int)ARG(0)); }
    longjmp(*out, 1);
}
static void CreateThread_(Ctx* c) {
    uint32_t h = h_new(H_WAIT);
    Obj* o = h_get(h);
    o->manual = 1;
    o->start = ARG(2), o->param = ARG(3), o->suspended = (ARG(4) & 4) != 0;
    o->stack = ARG(1) > 0x100000 ? ARG(1) : 0x100000;
    pthread_attr_t a;
    pthread_attr_init(&a);
    /* translated code nests deep on the host stack, and vitaGL's CG compiler runs on top of it */
    pthread_attr_setstacksize(&a, 8 << 20);
    pthread_t t;
    if (pthread_create(&t, &a, thread_main, o)) { c->eax = 0; return; }
    pthread_detach(t);
    if (ARG(5)) ST32(ARG(5), h);
    vlog("thread %x started at %06x\n", h, o->start);
    c->eax = h;
}
static void ResumeThread_(Ctx* c) {
    Obj* o = h_get(ARG(0));
    if (o) { pthread_mutex_lock(&o->m); o->suspended = 0; pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->m); }
    c->eax = 1;
}
static void SetThreadPriority_(Ctx* c) { c->eax = 1; }
static void GetThreadPriority_(Ctx* c) { c->eax = 0; }

/* TLS: the slots live in each thread's TEB, like on Windows */
static uint32_t g_tls_next;
static void TlsAlloc_(Ctx* c) { c->eax = g_tls_next < 64 ? __atomic_fetch_add(&g_tls_next, 1, __ATOMIC_SEQ_CST) : 0xffffffffu; }
static void TlsGetValue_(Ctx* c) { set_error(c, 0); c->eax = LD32(c->fsbase + 0xe10 + 4 * ARG(0)); }
static void TlsSetValue_(Ctx* c) { ST32(c->fsbase + 0xe10 + 4 * ARG(0), ARG(1)); c->eax = 1; }
static void TlsFree_(Ctx* c) { c->eax = 1; }

const ShimDef shims_kernel32[] = {
    {"GetLastError", GetLastError_}, {"SetLastError", SetLastError_},
    {"CreateFileW", CreateFileW_}, {"WideCharToMultiByte", WideCharToMultiByte_}, {"SetEndOfFile", SetEndOfFile_},
    {"SetFilePointerEx", SetFilePointerEx_}, {"RemoveDirectoryA", RemoveDirectoryA_},
    {"GetCurrentDirectoryA", GetCurrentDirectoryA_}, {"GetFullPathNameA", GetFullPathNameA_}, {"lstrcmpi", lstrcmpi_},
    {"lstrcmpiA", lstrcmpi_}, {"FindFirstFileA", FindFirstFileA_}, {"FindFirstFileExA", FindFirstFileExA_},
    {"FindNextFileA", FindNextFileA_}, {"FindClose", FindClose_}, {"ReadFileEx", ReadFileEx_},
    {"WriteFileEx", WriteFileEx_}, {"CancelIo", CancelIo_}, {"SleepEx", SleepEx_}, {"SwitchToThread", SwitchToThread_},
    {"GetPrivateProfileStringA", GetPrivateProfileStringA_}, {"GetPrivateProfileIntA", GetPrivateProfileIntA_},
    {"GetProfileStringA", GetProfileStringA_}, {"GetProfileIntA", GetProfileIntA_},
    {"WritePrivateProfileStringA", True1_}, {"WriteProfileStringA", True1_}, {"SetErrorMode", Zero0_},
    {"SetUnhandledExceptionFilter", Zero0_}, {"OpenProcess", Zero0_}, {"GetSystemInfo", GetSystemInfo_},
    {"IsProcessorFeaturePresent", IsProcessorFeaturePresent_}, {"FileTimeToLocalFileTime", FileTimeToLocalFileTime_},
    {"FileTimeToSystemTime", FileTimeToSystemTime_}, {"FormatMessageA", FormatMessageA_}, {"ExitThread", ExitThread_},
    {"CreateFileA", CreateFileA_}, {"ReadFile", ReadFile_}, {"WriteFile", WriteFile_}, {"GetFileSize", GetFileSize_},
    {"SetFilePointer", SetFilePointer_}, {"CloseHandle", CloseHandle_}, {"GetOverlappedResult", GetOverlappedResult_},
    {"GetFileAttributesA", GetFileAttributesA_}, {"CreateDirectoryA", CreateDirectoryA_}, {"DeleteFileA", DeleteFileA_},
    {"GetDiskFreeSpaceExA", GetDiskFreeSpaceExA_}, {"CreateFileMappingA", CreateFileMappingA_},
    {"MapViewOfFile", MapViewOfFile_}, {"UnmapViewOfFile", UnmapViewOfFile_},
    {"GetTickCount", GetTickCount_}, {"QueryPerformanceCounter", QueryPerformanceCounter_},
    {"QueryPerformanceFrequency", QueryPerformanceFrequency_}, {"GetSystemTimeAsFileTime", GetSystemTimeAsFileTime_},
    {"GetLocalTime", GetLocalTime_}, {"Sleep", Sleep_},
    {"GetCurrentProcessId", GetCurrentProcessId_}, {"GetCurrentThreadId", GetCurrentThreadId_},
    {"GetCurrentThread", GetCurrentThread_}, {"GetCurrentProcess", GetCurrentProcess_},
    {"GetModuleHandleA", GetModuleHandleA_}, {"GetModuleFileNameA", GetModuleFileNameA_},
    {"GetCommandLineA", GetCommandLineA_}, {"GetStartupInfoA", GetStartupInfoA_},
    {"GetSystemDirectoryA", GetSystemDirectoryA_}, {"GetVersionExA", GetVersionExA_},
    {"GlobalMemoryStatus", GlobalMemoryStatus_}, {"LoadLibraryA", LoadLibraryA_}, {"FreeLibrary", FreeLibrary_},
    {"GetProcAddress", GetProcAddress_}, {"OutputDebugStringA", OutputDebugStringA_}, {"ExitProcess", ExitProcess_},
    {"VirtualAlloc", VirtualAlloc_}, {"VirtualFree", VirtualFree_}, {"VirtualQuery", VirtualQuery_},
    {"VirtualProtect", VirtualProtect_}, {"HeapAlloc", HeapAlloc_}, {"HeapReAlloc", HeapReAlloc_},
    {"HeapFree", HeapFree_}, {"HeapSize", HeapSize_}, {"GetProcessHeap", GetProcessHeap_}, {"HeapCreate", HeapCreate_},
    {"GlobalAlloc", GlobalAlloc_}, {"GlobalLock", GlobalLock_}, {"GlobalUnlock", GlobalUnlock_},
    {"GlobalFree", GlobalFree_}, {"LocalFree", GlobalFree_},
    {"InitializeCriticalSection", InitializeCriticalSection_},
    {"InitializeCriticalSectionAndSpinCount", InitializeCriticalSection_},
    {"EnterCriticalSection", EnterCriticalSection_}, {"LeaveCriticalSection", LeaveCriticalSection_},
    {"TryEnterCriticalSection", TryEnterCriticalSection_}, {"DeleteCriticalSection", DeleteCriticalSection_},
    {"InterlockedIncrement", InterlockedIncrement_}, {"InterlockedDecrement", InterlockedDecrement_},
    {"InterlockedExchange", InterlockedExchange_}, {"InterlockedExchangeAdd", InterlockedExchangeAdd_},
    {"InterlockedCompareExchange", InterlockedCompareExchange_},
    {"CreateEventA", CreateEventA_}, {"CreateMutexA", CreateMutexA_}, {"CreateSemaphoreA", CreateSemaphoreA_},
    {"SetEvent", SetEvent_}, {"ResetEvent", ResetEvent_}, {"ReleaseMutex", ReleaseMutex_},
    {"ReleaseSemaphore", ReleaseSemaphore_}, {"WaitForSingleObject", WaitForSingleObject_},
    {"WaitForMultipleObjects", WaitForMultipleObjects_},
    {"CreateThread", CreateThread_}, {"ResumeThread", ResumeThread_}, {"SetThreadPriority", SetThreadPriority_},
    {"GetThreadPriority", GetThreadPriority_},
    {"TlsAlloc", TlsAlloc_}, {"TlsGetValue", TlsGetValue_}, {"TlsSetValue", TlsSetValue_}, {"TlsFree", TlsFree_},
    {NULL, NULL},
};
