/* the sce* calls the Vita code makes, on Win32 (vpc.c) */
#pragma once
#include <stdint.h>

typedef int SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
typedef uint64_t SceUInt64;
typedef int (*SceKernelThreadEntry)(SceSize args, void* argp);

SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int prio, SceSize stack, SceUInt attr,
                             int affinity, const void* opt);
int sceKernelStartThread(SceUID t, SceSize args, void* argp);
int sceKernelGetThreadId(void);
int sceKernelChangeThreadCpuAffinityMask(SceUID t, int mask);
int sceKernelDelayThread(SceUInt us);
SceUInt64 sceKernelGetProcessTimeWide(void);
int sceKernelExitProcess(int code);
SceUID sceKernelCreateSema(const char* name, SceUInt attr, int init, int max, void* opt);
int sceKernelWaitSema(SceUID s, int n, SceUInt* timeout);
int sceKernelSignalSema(SceUID s, int n);
SceUID sceKernelFindMemBlockByAddr(const void* addr, SceSize size);

static inline int scePowerSetArmClockFrequency(int f) { (void)f; return 0; }
static inline int scePowerSetBusClockFrequency(int f) { (void)f; return 0; }
static inline int scePowerSetGpuClockFrequency(int f) { (void)f; return 0; }
static inline int scePowerSetGpuXbarClockFrequency(int f) { (void)f; return 0; }

enum {
    SCE_CTRL_SELECT = 0x1, SCE_CTRL_START = 0x8, SCE_CTRL_UP = 0x10, SCE_CTRL_RIGHT = 0x20, SCE_CTRL_DOWN = 0x40,
    SCE_CTRL_LEFT = 0x80, SCE_CTRL_LTRIGGER = 0x100, SCE_CTRL_RTRIGGER = 0x200, SCE_CTRL_TRIANGLE = 0x1000,
    SCE_CTRL_CIRCLE = 0x2000, SCE_CTRL_CROSS = 0x4000, SCE_CTRL_SQUARE = 0x8000,
    SCE_CTRL_MODE_ANALOG_WIDE = 2
};
typedef struct { uint64_t timeStamp; unsigned int buttons; uint8_t lx, ly, rx, ry; uint8_t reserved[16]; } SceCtrlData;
int sceCtrlSetSamplingMode(int mode);
int sceCtrlPeekBufferPositive(int port, SceCtrlData* d, int n);

enum { SCE_AUDIO_OUT_PORT_TYPE_BGM = 1, SCE_AUDIO_OUT_MODE_STEREO = 1 };
int sceAudioOutOpenPort(int type, int grain, int rate, int mode);
int sceAudioOutOutput(int port, const void* buf);
