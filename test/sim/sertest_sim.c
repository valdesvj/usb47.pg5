/*
 * sertest_sim.c - SERTEST's PC simulator: the real src/sertest/sertest.c against a fake DMCP5 3.58, in real time,
 * whose serial console is a pseudo-terminal, so tools/sertest.py runs against it exactly as against /dev/ttyACM0.
 * The console follows SwissMicros' data sheet; where the sheet says UNKNOWN the sim makes a choice, set by env:
 *   SIM_WAKE_ON_LINE=1   a line on channel 2/3 wakes sys_sleep() (default 0: only keys and timers do)
 *   a host that does not read: the pty fills, a write waits 100 ms for room, then the rest of it is dropped
 *   SIM_CABLE=0          no USB cable (SERIO_IS_ACTIVE 0)
 * The port's name goes to stdout and to build/sertest/tty (a symlink). The calculator disk is ./simdisk/.
 *   make sertest-sim && build/sertest/sim
 * GPL-3.0-or-later (Victor Valdes).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "dmcp.h"
#include "sercon_ifc.h"

sys_sdb_t sim_sdb;
static uint8_t fb[LCD_Y][LCD_X];
static int master = -1, wake_on_line, cable = 1;
static int en, s_lcd, s_keys, s_printf, s_ir;
static uint32_t t_start_ms;

static uint32_t real_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
uint32_t sys_current_ms(void) { return real_ms() - t_start_ms; }

/* ---- the console: input ---- */
typedef struct { uint8_t d[USIN_CHAN_SIZE + 1]; int n, ovf; } Line;
typedef struct { Line q[USIN_MAX_READLINES]; int nq; Line cur; } Chan;
static Chan in[4];
static int rx_ch;
static int keyq[64], nkq, held1 = -1, held2 = -1;

static void push_key(int k) { if (nkq < 64) keyq[nkq++] = k; }
static int used(Chan *c) { int u = c->cur.n; for (int i = 0; i < c->nq; i++) u += c->q[i].n; return u; }

static void out_raw(int ch, const uint8_t *s, int n);

static void key_report(void) {
  if (!en || !s_keys) return;
  char b[40];
  uint32_t cs = (sys_current_ms() / 10) % 360000;
  int n = sprintf(b, "K %02x %02x %06x\n", held1 < 0 ? 0xff : held1, held2 < 0 ? 0xff : held2, (unsigned)cs);
  out_raw(0, (uint8_t *)b, n);
}
static void sim_host_L(void);

static void chan0(const char *s) {            /* DMCP's own commands (data sheet section 8) */
  if (!strcmp(s, "V")) {
    char b[100];
    int n = sprintf(b, "V|HW SERTEST|VER 1.0|DMCP5 3.58-sim|IFC 3.18|SN SIM00001\n");
    out_raw(0, (uint8_t *)b, n);
  } else if (!strcmp(s, "L")) { sim_host_L(); push_key(SERCON_KEY_REDRAW); push_key(0); }
  else if (!strncmp(s, "KP", 2)) {
    unsigned v = (unsigned)strtoul(s + 2, NULL, 16);
    held1 = (int)(v & 0xff);
    held2 = strlen(s + 2) > 2 ? (int)(v >> 8) : -1;
    push_key(held2 > 0 ? held2 : held1);
    key_report();
  } else if (!strcmp(s, "KR")) { held1 = held2 = -1; push_key(0); key_report(); }
  else if (!strncmp(s, "K ", 2)) {
    int k = (int)strtoul(s + 2, NULL, 16);
    held1 = k; push_key(k); key_report();
    held1 = -1; push_key(0); key_report();
  } else fprintf(stderr, "sim: channel 0 command %s ignored\n", s);
}

static void rx_byte(uint8_t b) {
  if (b >= US_CHAN0_CHAR && b <= US_CHAN0_CHAR + 10) { rx_ch = b - US_CHAN0_CHAR; return; }
  if (rx_ch > 3) return;                      /* channels 4..10: no input FIFO, dropped */
  Chan *c = &in[rx_ch];
  if (b == '\n') {
    if (rx_ch <= 1) { c->cur.d[c->cur.n] = 0; if (rx_ch == 0) chan0((char *)c->cur.d); c->cur.n = 0; return; }
    if (c->nq < USIN_MAX_READLINES) {
      Line *l = &c->q[c->nq++];
      *l = c->cur;
      l->d[l->n++] = l->ovf ? US_OVERFLOW_CHAR : '\n';   /* a cut line ends in 0x0f */
    }                                          /* 16 lines waiting: the line is lost */
    memset(&c->cur, 0, sizeof c->cur);
    return;
  }
  if (used(c) >= USIN_CHAN_SIZE - 1 || c->cur.n >= USIN_CHAN_SIZE - 1) { c->cur.ovf = 1; return; }
  c->cur.d[c->cur.n++] = b;
}

/* the host: is the port open (the pty's other end), bytes from it */
static int host_open(void) {
  struct pollfd p = { master, POLLIN, 0 };
  poll(&p, 1, 0);
  return !(p.revents & POLLHUP);
}
static int pump(void) {                       /* returns what came: 1 a key, 2 a line on 2/3 */
  int got = 0, k0 = nkq, l0 = in[2].nq + in[3].nq;
  uint8_t b[512];
  for (;;) {
    ssize_t n = read(master, b, sizeof b);
    if (n <= 0) break;
    for (ssize_t i = 0; i < n; i++) if (en) rx_byte(b[i]);
  }
  if (nkq > k0) got |= 1;
  if (in[2].nq + in[3].nq > l0) got |= 2;
  return got;
}

/* ---- the console: output ---- */
static void out_raw(int ch, const uint8_t *s, int n) {
  if (!en || !cable || !host_open()) return;
  uint8_t b[300];
  if (n > 255) n = 255;                       /* the data sheet: at most 255 bytes of a write go out */
  b[0] = (uint8_t)(US_CHAN0_CHAR + ch);
  memcpy(b + 1, s, (size_t)n);
  int done = 0;
  uint32_t t0 = sys_current_ms();
  while (done < n + 1) {                       /* room for 100 ms at most; then the rest is dropped */
    ssize_t w = write(master, b + done, (size_t)(n + 1 - done));
    if (w > 0) { done += (int)w; continue; }
    if (sys_current_ms() - t0 >= 100) break;
    struct pollfd p = { master, POLLOUT, 0 };
    poll(&p, 1, 5);
  }
}

int usb_serial_ioctl(int op, int val) {
  pump();
  int rd = val == SER_IOCTL_READ;
  int *p = op == SERIO_IS_ENABLED ? &en : op == SERIO_SEND_PRINTF ? &s_printf : op == SERIO_SEND_LCD ? &s_lcd :
           op == SERIO_SEND_KEYS ? &s_keys : op == SERIO_SEND_IR ? &s_ir : NULL;
  if (p) { if (!rd) *p = val ? 1 : 0; return *p; }
  if (op == SERIO_IS_ACTIVE) return rd ? (en && cable) : SER_IOCTL_INVALID;
  if (op == SERIO_IS_CONNECTED) return rd ? (en && cable && host_open()) : SER_IOCTL_INVALID;
  if (op == SERIO_LINE_READY) return val >= 2 && val <= 3 ? in[val].nq : 0;
  if (op == SERIO_LINE_LENGTH) return val >= 2 && val <= 3 && in[val].nq ? in[val].q[0].n : 0;
  return SER_IOCTL_INVALID;
}
void usb_serial_write(uint8_t chan_nr, const char *buf, int len) { out_raw(chan_nr, (const uint8_t *)buf, len); }
int usb_serial_read_line(uint8_t chan_nr, uint8_t *buf, int buflen, uint8_t *is_overflow) {
  pump();
  if (chan_nr < 2 || chan_nr > 3 || !in[chan_nr].nq) return 0;
  Chan *c = &in[chan_nr];
  Line l = c->q[0];
  if (buf) {
    if (l.n > buflen) return -1;              /* stays in the FIFO */
    memcpy(buf, l.d, (size_t)l.n);
    if (l.n <= buflen) buf[l.n] = 0;
    if (is_overflow) *is_overflow = (uint8_t)l.ovf;
  }
  memmove(c->q, c->q + 1, sizeof(Line) * (size_t)(--c->nq));
  return l.n;
}

/* the LCD stream: a row goes out at a refresh when it differs from the one last sent for that line */
static int sent_ok[LCD_Y];
static uint8_t sent[LCD_Y][50];
static void sim_host_L(void) { memset(sent_ok, 0, sizeof sent_ok); }
static void lcd_out(void) {
  if (!en || !s_lcd) return;
  for (int y = 0; y < LCD_Y; y++) {
    uint8_t d[50];
    memset(d, 0xFF, 50);
    int blank = 1;
    for (int x = 0; x < LCD_X; x++)
      if (fb[y][x]) { d[49 - x / 8] &= (uint8_t)~(0x80 >> (x % 8)); blank = 0; }
    if (sent_ok[y] && !memcmp(sent[y], d, 50)) continue;
    int was = sent_ok[y];
    memcpy(sent[y], d, 50);
    sent_ok[y] = 1;
    if (!was && blank) continue;              /* after L: "every non-blank row" */
    uint8_t line[70];
    int n = sprintf((char *)line, "L %02X", LCD_Y - 1 - y);
    for (int j = 0; j < 58; j++) {
      int v = 0;
      for (int k = 0; k < 7; k++) { int i = j * 7 + k; if (i < 400 && (d[i / 8] >> (i % 8) & 1)) v |= 1 << k; }
      line[n++] = (uint8_t)(0x80 | v);
    }
    line[n++] = '\n';
    out_raw(0, line, n);
  }
}

/* printf: channel 1 while SERIO_SEND_PRINTF is on, else the sim's stderr */
static ssize_t pf_write(void *c, const char *b, size_t n) {
  (void)c;
  if (s_printf) out_raw(1, (const uint8_t *)b, (int)n); else fwrite(b, 1, n, stderr);
  return (ssize_t)n;
}

/* ---- the rest of DMCP ---- */
void lcd_clear_buf(void) { memset(fb, 0, sizeof fb); }
void lcd_refresh(void) { lcd_out(); }
void lcd_refresh_wait(void) {}
void lcd_forced_refresh(void) { lcd_out(); }
void lcd_set_buf_cleared(int v) { (void)v; }
void LCD_power_on(void) {}
void LCD_power_off(int c) { (void)c; }
void draw_power_off_image(int a) { (void)a; }
void rtc_wakeup_delay(void) {}
int  switch_usb_powered_freq(void) { return 0; }
void lcd_fill_rect(uint32_t x, uint32_t y, uint32_t dx, uint32_t dy, int val) {
  for (uint32_t j = y; j < y + dy && j < LCD_Y; j++)
    for (uint32_t i = x; i < x + dx && i < LCD_X; i++) fb[j][i] = (uint8_t)(val ? 1 : 0);
}
void lcd_draw_menu_keys(const char *keys[]) {
  (void)keys;
  int w = LCD_X / 6;
  for (int i = 0; i < 6; i++) lcd_fill_rect((uint32_t)(i * w + 2), LCD_Y - LCD_MENU_LINES + 2, (uint32_t)(w - 4), LCD_MENU_LINES - 4, 1);
}
void sys_reset(void) { exit(3); }

static uint32_t timer_at[4];
static int timer_on[4];
void sys_timer_start(int ix, uint32_t ms) { timer_at[ix] = sys_current_ms() + ms; timer_on[ix] = 1; }
void sys_timer_disable(int ix) { timer_on[ix] = 0; }
int  sys_timer_timeout(int ix) { return timer_on[ix] && (int32_t)(sys_current_ms() - timer_at[ix]) >= 0; }
void sys_delay(uint32_t ms) {
  uint32_t end = sys_current_ms() + ms;
  while ((int32_t)(end - sys_current_ms()) > 0) { pump(); usleep(1000); }
}

int key_empty(void) { pump(); return nkq == 0; }
int key_pop(void) {
  pump();
  if (!nkq) return -1;
  int k = keyq[0];
  memmove(keyq, keyq + 1, sizeof(int) * (size_t)(--nkq));
  return k;
}
int sys_last_scan(int *k1, int *k2) { *k1 = held1; *k2 = held2; return (held1 >= 0) + (held2 >= 0); }

void sys_sleep(void) {
  if (ST(STAT_PGM_END)) { fprintf(stderr, "sim: the program ended\n"); exit(0); }
  for (;;) {
    int w = pump();
    if (nkq || (w & 2 && wake_on_line)) return;
    int t = -1;
    for (int i = 0; i < 4; i++)
      if (timer_on[i]) {
        int32_t left = (int32_t)(timer_at[i] - sys_current_ms());
        if (left <= 0) return;
        if (t < 0 || left < t) t = left;
      }
    if (!host_open()) { usleep(20000); continue; }   /* no host: poll() would only say POLLHUP */
    struct pollfd p = { master, POLLIN, 0 };
    poll(&p, 1, t < 0 || t > 20 ? 20 : t);
  }
}

static void path(const char *p, char *o) { snprintf(o, 256, "simdisk%s", p); }
int sys_disk_ok(void) { return 1; }
int sys_disk_write_enable(int v) { (void)v; return 1; }
FRESULT f_open(FIL *fp, const TCHAR *p, BYTE mode) {
  char q[256];
  mkdir("simdisk", 0755);
  path(p, q);
  FILE *f = fopen(q, (mode & FA_WRITE) ? "wb" : "rb");
  fp->obj.fs = (FATFS *)f;
  return f ? FR_OK : FR_NO_FILE;
}
FRESULT f_close(FIL *fp) { fclose((FILE *)fp->obj.fs); return FR_OK; }
FRESULT f_write(FIL *fp, const void *b, UINT n, UINT *bw) { *bw = (UINT)fwrite(b, 1, n, (FILE *)fp->obj.fs); return FR_OK; }

void program_main(void);
int main(void) {
  t_start_ms = real_ms();
  wake_on_line = getenv("SIM_WAKE_ON_LINE") && atoi(getenv("SIM_WAKE_ON_LINE"));
  cable = !getenv("SIM_CABLE") || atoi(getenv("SIM_CABLE"));
  master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) || unlockpt(master)) { perror("pty"); return 1; }
  const char *name = ptsname(master);
  int s = open(name, O_RDWR | O_NOCTTY);      /* raw, then closed: the host opens it */
  struct termios t;
  tcgetattr(s, &t);
  cfmakeraw(&t);
  tcsetattr(s, TCSANOW, &t);
  close(s);
  fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
  mkdir("build", 0755); mkdir("build/sertest", 0755);
  unlink("build/sertest/tty");
  if (symlink(name, "build/sertest/tty")) perror("symlink");
  printf("%s\n", name);
  fflush(stdout);
  cookie_io_functions_t io = { NULL, pf_write, NULL, NULL };
  stdout = fopencookie(NULL, "w", io);
  setvbuf(stdout, NULL, _IOLBF, 0);
  program_main();
  return 0;
}
