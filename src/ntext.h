/* ntext.h - NAV47: the page text in the C47 standard font (GRFNT 21) on 16-pixel rows, and the degree ring.
 * GPL-3.0-or-later. */
#ifndef NAV47_NTEXT_H
#define NAV47_NTEXT_H

#define NT_ROW 16              /* the row pitch: 13 rows above the menu bar */
#define NT_DEG "\x01"          /* the degree ring */

void nt_init(void);
int  nt(int x, int y, const char *s);            /* y = top of the row; returns the end x */
int  nt_inv(int x, int y, const char *s);        /* white on a black box */
int  nt_r(int xr, int y, const char *s);         /* right aligned at xr */
int  nt_c(int xc, int y, const char *s);         /* centred at xc */
int  nt_w(const char *s);
int  nt_cw(void);                                /* a digit's width */

#endif
