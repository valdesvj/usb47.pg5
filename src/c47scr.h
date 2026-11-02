/* c47scr.h - NAV47: a C47 screen (400 x 240, origin bottom-left, as PIXEL / AGRAPH) and its text and number
 * printers, so Almanac 47's views (python/native/c47screen.py, c47screen21.py) port line for line.
 * The bitmap lives on the heap; c47_show() copies it to the DMCP LCD buffer. GPL-3.0-or-later. */
#ifndef NAV47_C47SCR_H
#define NAV47_C47SCR_H

typedef struct { unsigned char x; unsigned int m; } C47Col;
typedef struct { signed char adv, yoff; unsigned char n; unsigned short ofs; } C47Glyph;
typedef struct { const C47Glyph *g; const C47Col *c; } C47Font;

extern const C47Font F21, TINY21, SYMB, SYMS;

#define SW 400
#define SH 240

typedef struct { unsigned char b[SH][SW / 8]; } C47Screen;   /* row 0 = the bottom (y = 0) */

int  c47_init(void);                 /* allocates the screen; 0 = no memory */
C47Screen *c47(void);
void c47_clear(void);
void c47_pixel(int y, int x);        /* PIXEL: y, x >= 0 a dot; x < 0 a vertical line at -x; y < 0 a horizontal one */
void c47_dot4(int y, int x);
int  c47_hline(int y, int x, int n);
void c47_xor_box(int y, int x, int w, int h);
int  c47_glyph(const C47Font *f, int ch, int y, int x);
int  c47_text(int y, int x, const char *s, const C47Font *f);
int  c47_width(const char *s, const C47Font *f);
/* number printers in F21 (PINB PF1 PHM PDM PZN PDAT) and the tiny font */
int  c47_pinb(int y, int x, double v);
int  c47_pf1(int y, int x, double v);
int  c47_phm(int y, int x, double h);
int  c47_pdm(int y, int x, double v);
int  c47_pzn(int y, int x, double v);
int  c47_pdat(int y, int x, double jd);
int  c47_tiny_num(int y, int x, double v);
void c47_show(void);                 /* the bitmap to the LCD (y up -> LCD rows down) */
int  c47_get(int x, int y);          /* the pixel (x, y from the bottom) */
void c47_xor_text_from(const C47Screen *other);

#endif
