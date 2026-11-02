/* helpers shared by the shims (util.c) */
#pragma once
#include <stdint.h>

#define GAME_CWD "C:\\USM"  /* what the game sees as its folder; map_path turns it into USM_DIR */

char* gstrdup(const char* s);  /* a copy in guest-visible memory */
/* printf for the game: the format's arguments are read from guest memory starting at `ap` (32-bit slots, doubles take
   two, %s is a guest pointer). Returns the length it would have written. */
int gformat(char* out, uint32_t n, const char* fmt, uint32_t ap);
/* a game path ("data\\x.pcpack", "C:\\USM\\data\\x") as a Vita path under USM_DIR */
void map_path(const char* game, char* out, unsigned n);
