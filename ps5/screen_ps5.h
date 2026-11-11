/* reColossus (from reKing): a text screen for what happens before the game (from reUndr-ps5's screen_ps5.h, MIT, bystritskiy). */
#ifndef RECOLOSSUS_SCREEN_PS5_H
#define RECOLOSSUS_SCREEN_PS5_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned y, scale; /* top pixel row on 1080p; 5x7 glyphs drawn scale times larger */
    uint32_t color;    /* 0xAARRGGBB, 0 for white */
    const char *text;
} RcScreenLine;

/* Shows the lines, and a progress bar unless percent is negative. Negative on failure. */
int rc_screen_show(const RcScreenLine *lines, unsigned count, int percent);
/* Gives VideoOut back; the game's renderer cannot open it before this. */
void rc_screen_close(void);

#ifdef __cplusplus
}
#endif
#endif
