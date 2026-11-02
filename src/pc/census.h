/* census mode (census.c): which imports and COM methods the game really calls */
#pragma once

extern int g_census;
void census_init(const char* dir);
void* census_import(const char* dll, const char* name, void* real);  /* real, or a counting thunk in front of it */
void* census_hook(const char* dll, const char* name, void* real);    /* a wrapper for COM root creators, or NULL */
void census_dinput(void* di);
