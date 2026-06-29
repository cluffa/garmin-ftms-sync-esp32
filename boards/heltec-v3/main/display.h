#pragma once
#include "display_format.h"

void display_init(void);
/* Draw the text lines. bar_pct < 0 = no bar; 0..100 = long-press loading bar. */
void display_draw(const char lines[DISP_LINES][DISP_COLS], int bar_pct);
/* Blank the panel and cut its power (for shutdown). */
void display_off(void);
