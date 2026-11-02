/* ntext.c - NAV47: the page text in the C47 standard font (GRFNT 21, Almanac 47's font), drawn straight on the
 * DMCP LCD on 16-pixel rows: the digits 12 pixels high from row + 2 to the baseline at row + 13, descenders below.
 * The DMCP system font is left to the menu bar. The degree sign is a small ring drawn here.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later. */
#include <main.h>
#include <dmcp.h>
#include <string.h>
#include "ntext.h"
#include "c47scr.h"

#define BASE 13                /* the baseline (the digits' bottom row) from the top of a row */

void nt_init(void) {}
int nt_cw(void) { return F21.g['0'].adv; }

static int adv(int c) { return c == 1 ? 5 : F21.g[c & 0x7f].adv; }

int nt_w(const char *s) {
  int w = 0;
  for (; *s; s++) w += adv((unsigned char)*s);
  return w;
}

static void px(int x, int y, int v) {
  if (x >= 0 && y >= 0 && x < LCD_X && y < LCD_Y) lcd_fill_rect((uint32_t)x, (uint32_t)y, 1, 1, v);
}

static int draw(int x, int y, const char *s, int v) {
  int bl = y + BASE;
  for (; *s; s++) {
    int c = (unsigned char)*s;
    if (c == 1) {                                   /* the degree ring, 4 x 4 at the top of the digits */
      int t = y + 2;
      px(x + 1, t, v); px(x + 2, t, v); px(x + 1, t + 3, v); px(x + 2, t + 3, v);
      px(x, t + 1, v); px(x, t + 2, v); px(x + 3, t + 1, v); px(x + 3, t + 2, v);
    } else {
      const C47Glyph *g = &F21.g[c & 0x7f];
      for (int k = 0; k < g->n; k++) {
        const C47Col *col = &F21.c[g->ofs + k];
        unsigned int m = col->m;
        for (int i = 0; m; i++, m >>= 1)
          if (m & 1) px(x + col->x, bl - (g->yoff + i), v);
      }
    }
    x += adv(c);
  }
  return x;
}

int nt(int x, int y, const char *s) { return draw(x, y, s, 1); }
int nt_inv(int x, int y, const char *s) {
  lcd_fill_rect((uint32_t)(x - 2), (uint32_t)y, (uint32_t)(nt_w(s) + 3), NT_ROW, 1);
  return draw(x, y, s, 0);
}
int nt_r(int xr, int y, const char *s) { return draw(xr - nt_w(s), y, s, 1); }
int nt_c(int xc, int y, const char *s) { return draw(xc - nt_w(s) / 2, y, s, 1); }
