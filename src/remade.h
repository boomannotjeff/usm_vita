#pragma once
#include <windows.h>

// Count calls into a remade function, so the log shows which hooks the play session actually exercised.
// A hook that was never hit has not been verified.
#define REMADE_HIT(name)                          \
    do {                                          \
        extern volatile long remade_hits_##name;  \
        InterlockedIncrement(&remade_hits_##name); \
    } while (0)

// Install every function in remade.cpp's table over the original, unless USM_NO_REMADE=1.
void install_remade();

// Mark the session as ended normally; otherwise the next start quarantines the last hooks to first run.
void shutdown_remade();
