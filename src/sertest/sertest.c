/*
 * SERTEST - every item of SwissMicros' "DMCP5 Serial Console API" data sheet (DMCP5 3.58, IFC 3.18), tested on the
 * DM42n. The calculator side: a command server on channels 2 and 3; tools/sertest.py on the PC drives it, checks
 * each answer and writes the log to send to SwissMicros.
 *
 * At start it reads every switch (the user's DMCP settings, kept to put back), switches the console on and waits.
 * Commands (one line each, on channel 2 or 3; the answer goes back on the same channel):
 *   PING x              PONG x t=<ms>
 *   IO op val|R         usb_serial_ioctl(op, val or SER_IOCTL_READ) -> its result
 *   ECHO text           ECHO text
 *   BUF n               usb_serial_read_line's buflen from now on (1..256)
 *   VERBOSE 0|1         1: report every read: LINE_READY, LINE_LENGTH, n, is_overflow, the last byte, buf[n]
 *   HOLD ms             read nothing for ms, then HOLDEND (the input FIFO fills: 256 bytes / 16 lines)
 *   BURST n size gap    n writes of size bytes (one line each, numbered), gap ms between (0: back to back)
 *   BIG n               one write of n bytes (one line), then BIGEND in a second write (the 255-byte cap)
 *   MULTI               one write of three lines (the id byte only at the start of a write)
 *   CHAN n text         text on output channel n (0..10)
 *   PRINTF              SERIO_SEND_PRINTF 1, a printf, the switch put back
 *   LCD 0|1, KEYS 0|1   SERIO_SEND_LCD, SERIO_SEND_KEYS
 *   PATTERN, SCREEN     a known full-screen pattern (checked pixel for pixel on the PC), or the status screen
 *   NO77 0|1            1: key 77 is only logged, no redraw ("without that redraw, L alone sends nothing")
 *   NOWAKE ms           sleep with no poll timer (only a fallback at ms): does a line wake sys_sleep()? (UNKNOWN 1)
 *   FLOOD ms size       write lines non-stop for ms, timing every call: a host that does not read (UNKNOWN 2)
 *   LCDFLOOD ms         redraw the screen non-stop for ms with SERIO_SEND_LCD on, timing every refresh
 *   RESULT              the last result again (it may have been sent while the PC was not there)
 *   REPORT, CLEARLOG    the calculator's own log (boot readings, keys, connects, wakes)
 *   QUIT                put the user's settings back, read them again, save /SERTEST.TXT, end
 * Keys on the calculator: F4 saves /SERTEST.TXT, EXIT or F6 puts the settings back and ends; every key is logged.
 * From the PC: tools/sertest.py --quit (the command QUIT) ends it the same way. No reset needed: the calculator
 * goes off; ON brings back DMCP's menu.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
 */
#include <main.h>
#include <dmcp.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "ntext.h"
#include "sercon_ifc.h"

#define RD      SER_IOCTL_READ
#define T_POLL  0                      /* the poll timer (and NOWAKE's fallback) */
#define CAP     128                    /* bytes in one reply */
#define LOGSZ   (24 * 1024)
#define BIGSZ   320

static const int OPS[] = { SERIO_IS_ENABLED, SERIO_IS_ACTIVE, SERIO_IS_CONNECTED, SERIO_SEND_PRINTF, SERIO_SEND_LCD,
                           SERIO_SEND_KEYS, SERIO_SEND_IR };
static const char *const OPN[] = { "IS_ENABLED", "IS_ACTIVE", "IS_CONNECTED", "SEND_PRINTF", "SEND_LCD", "SEND_KEYS",
                                   "SEND_IR" };
#define NOPS 7
static const int RW[] = { 1, 0, 0, 1, 1, 1, 1 };
static int was[NOPS];                  /* the user's settings at start */

static char *logb;
static int logn, loglines;
static uint8_t inb[USIN_CHAN_SIZE + 2];
static char big[BIGSZ];
static int bufsz = USIN_CHAN_SIZE, verbose, no77, pattern, dirty = 1;
static uint32_t hold_until, nowake_until, sleep_t0;
static int conn = -1, act = -1, hold_ch = 2;
static unsigned n_in, n_out, n77, nkeys;
static char last_result[CAP], cur[48] = "waiting for tools/sertest.py";

static uint32_t now(void) { return sys_current_ms(); }
static int io(int op) { return usb_serial_ioctl(op, RD); }

static void logline(const char *fmt, ...) {
  if (!logb || logn > LOGSZ - 160) return;
  logn += sprintf(logb + logn, "%7lu ", (unsigned long)now());
  va_list a;
  va_start(a, fmt);
  logn += vsnprintf(logb + logn, 140, fmt, a);
  va_end(a);
  logb[logn++] = '\n';
  logb[logn] = 0;
  loglines++;
}

static void out(int ch, const char *s, int n) { usb_serial_write((uint8_t)ch, s, n); n_out++; }

/* one whole line, at most CAP bytes, only while a host has the port open (as the data sheet's example) */
static void say(int ch, const char *fmt, ...) {
  char b[CAP + 8];
  va_list a;
  va_start(a, fmt);
  int n = vsnprintf(b, CAP - 1, fmt, a);
  va_end(a);
  if (n > CAP - 2) n = CAP - 2;
  b[n++] = '\n';
  if (io(SERIO_IS_CONNECTED) > 0) out(ch, b, n);
}

static void result(int ch, const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(last_result, sizeof last_result - 2, fmt, a);
  va_end(a);
  logline("%s", last_result);
  say(ch, "%s", last_result);
  dirty = 1;
}

/* ---- the screen ---- */
/* the test pattern: a slope (x + 3y) mod 50 < 9 left of x 384; at 384..399 the row number y in 8 bits, 2 px each.
   Every row has dark pixels, it is not symmetric left / right or top / bottom, and each row says which it is. */
int sertest_pixel(int x, int y) {
  if (x < 384) return (x + 3 * y) % 50 < 9;
  return (y >> ((x - 384) / 2)) & 1;
}

static void draw_pattern(void) {
  lcd_clear_buf();
  for (int y = 0; y < LCD_Y; y++)
    for (int x = 0; x < LCD_X;) {
      if (!sertest_pixel(x, y)) { x++; continue; }
      int e = x;
      while (e < LCD_X && sertest_pixel(e, y)) e++;
      lcd_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)(e - x), 1, 1);
      x = e;
    }
}

static void draw(void) {
  char s[80];
  dirty = 0;
  if (pattern) { draw_pattern(); lcd_refresh(); return; }
  lcd_clear_buf();
  nt_inv(2, 0, "SERTEST  DMCP5 serial console test");
  lcd_fill_rect(0, 15, LCD_X, 1, 1);
  sprintf(s, "console %s   cable %s   PC port %s", io(SERIO_IS_ENABLED) > 0 ? "ON" : "off",
          io(SERIO_IS_ACTIVE) > 0 ? "in" : "OUT", io(SERIO_IS_CONNECTED) > 0 ? "open" : "closed");
  nt(2, 20, s);
  sprintf(s, "LCD %d  KEYS %d  PRINTF %d  IR %d", io(SERIO_SEND_LCD), io(SERIO_SEND_KEYS), io(SERIO_SEND_PRINTF),
          io(SERIO_SEND_IR));
  nt(2, 38, s);
  sprintf(s, "lines in %u  writes %u  key 77 %u  keys %u", n_in, n_out, n77, nkeys);
  nt(2, 56, s);
  nt(2, 80, "now:");
  nt(40, 80, cur);
  nt(2, 98, "last:");
  snprintf(s, 60, "%.58s", last_result);
  nt(40, 98, s);
  sprintf(s, "log %d lines, %d bytes", loglines, logn);
  nt(2, 122, s);
  nt(2, 146, "PC: python3 tools/sertest.py /dev/ttyACM0");
  nt(2, 162, "F4 saves /SERTEST.TXT   EXIT or F6 ends");
  static const char *keys[6] = { "", "", "", "SAVE", "", "EXIT" };
  lcd_draw_menu_keys(keys);
  lcd_refresh();
}

static void save(void) {
  if (!sys_disk_ok() || !logb) { strcpy(cur, "no disk"); return; }
  sys_disk_write_enable(1);
  FIL f;
  UINT bw;
  const char *h = "SERTEST " PROGRAM_VERSION " - DMCP5 serial console test, the calculator's log (ms, event)\n";
  if (f_open(&f, "/SERTEST.TXT", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    f_write(&f, h, (UINT)strlen(h), &bw);
    f_write(&f, logb, (UINT)logn, &bw);
    f_close(&f);
    strcpy(cur, "saved /SERTEST.TXT");
  } else strcpy(cur, "cannot write /SERTEST.TXT");
  sys_disk_write_enable(0);
  dirty = 1;
}

static void read_all(const char *tag) {
  char s[120];
  int n = 0;
  for (int i = 0; i < NOPS; i++) n += sprintf(s + n, " %s=%d", OPN[i], io(OPS[i]));
  logline("%s%s", tag, s);
}

static void restore(void) {
  for (int i = 0; i < NOPS; i++)
    if (RW[i] && (was[i] == 0 || was[i] == 1) && OPS[i] != SERIO_IS_ENABLED) usb_serial_ioctl(OPS[i], was[i]);
  if (was[0] == 0 || was[0] == 1) usb_serial_ioctl(SERIO_IS_ENABLED, was[0]);     /* the console last */
}

static void quit(int ch) {
  char s[100];
  int n = 0;
  for (int i = 0; i < NOPS; i++) if (RW[i]) n += sprintf(s + n, " %s=%d", OPN[i], was[i]);
  say(ch, "QUIT putting back:%s", s);
  logline("QUIT putting back:%s", s);
  sys_delay(300);
  restore();
  read_all("AFTER QUIT");
  say(ch, "QUIT done (if the console was on before, this line arrives)");
  save();
  SET_ST(STAT_PGM_END);
}

/* ---- the tests that write ---- */
static void line_of(char *b, int size, char tag, int seq) {
  int n = sprintf(b, "%c%06d ", tag, seq);
  while (n < size - 1) { b[n] = (char)('a' + n % 26); n++; }
  b[size - 1] = '\n';
}

static void burst(int ch, int n, int size, int gap) {
  if (size < 10) size = 10;
  if (size > BIGSZ) size = BIGSZ;
  uint32_t t0 = now(), maxc = 0;
  for (int i = 0; i < n; i++) {
    line_of(big, size, 'S', i);
    uint32_t t = now();
    out(ch, big, size);
    uint32_t d = now() - t;
    if (d > maxc) maxc = d;
    if (gap) sys_delay((uint32_t)gap);
  }
  sys_delay(300);
  result(ch, "BURSTEND n=%d size=%d gap=%d maxcall=%lums total=%lums", n, size, gap, (unsigned long)maxc,
         (unsigned long)(now() - t0));
}

static void flood(int ch, int ms, int size) {
  if (size < 10) size = 10;
  if (size > CAP) size = CAP;
  uint32_t t0 = now(), maxc = 0, last = t0;
  int calls = 0, slow = 0, samples = 0, conn_on = 0, keys = 0;
  strcpy(cur, "FLOOD: writing non-stop");
  while (now() - t0 < (uint32_t)ms) {
    line_of(big, size, 'F', calls);
    uint32_t t = now();
    out(ch, big, size);
    uint32_t d = now() - t;
    if (d > maxc) maxc = d;
    if (d > 20) slow++;
    calls++;
    if (now() - last >= 250) {                      /* the screen goes on (and out, with SERIO_SEND_LCD) */
      last = now();
      samples++;
      conn_on += io(SERIO_IS_CONNECTED) > 0;
      while (!key_empty()) { int k = key_pop(); if (k > 0) { keys++; logline("KEY %d during FLOOD", k); } }
      snprintf(cur, sizeof cur, "FLOOD %d writes, %lu s", calls, (unsigned long)((now() - t0) / 1000));
      draw();
    }
  }
  strcpy(cur, "FLOOD done");
  result(ch, "FLOODEND calls=%d size=%d ms=%d maxcall=%lums slow20=%d connected=%d/%d keys=%d", calls, size, ms,
         (unsigned long)maxc, slow, conn_on, samples, keys);
}

static void lcdflood(int ch, int ms) {
  uint32_t t0 = now(), maxr = 0;
  int n = 0;
  int lcd = io(SERIO_SEND_LCD);
  usb_serial_ioctl(SERIO_SEND_LCD, 1);
  while (now() - t0 < (uint32_t)ms) {
    lcd_clear_buf();
    lcd_fill_rect((uint32_t)(n * 7 % LCD_X), 0, 20, LCD_Y, 1);   /* a moving bar: every row changes */
    char s[40];
    sprintf(s, "LCDFLOOD %d", n);
    nt(2, 100, s);
    uint32_t t = now();
    lcd_refresh();
    lcd_refresh_wait();
    uint32_t d = now() - t;
    if (d > maxr) maxr = d;
    n++;
  }
  usb_serial_ioctl(SERIO_SEND_LCD, lcd);
  dirty = 1;
  result(ch, "LCDFLOODEND refreshes=%d ms=%d maxrefresh=%lums connected=%d", n, ms, (unsigned long)maxr,
         io(SERIO_IS_CONNECTED));
}

static void report(int ch) {
  int lines = 0;
  for (char *p = logb; p && *p;) {
    char *e = strchr(p, '\n');
    int n = e ? (int)(e - p) + 1 : (int)strlen(p);
    if (n > CAP) { char t[CAP]; memcpy(t, p, CAP - 1); t[CAP - 1] = '\n'; out(ch, t, CAP); }
    else out(ch, p, n);
    p += n;
    lines++;
    sys_delay(25);                                   /* whole lines, one write each, gently */
  }
  say(ch, "REPORTEND lines=%d", lines);
}

/* ---- the commands ---- */
static void command(int ch, char *s) {
  char *arg = strchr(s, ' ');
  if (arg) *arg++ = 0; else arg = s + strlen(s);
  long a = strtol(arg, NULL, 0);
  char *p2 = strchr(arg, ' ');
  long b = p2 ? strtol(p2 + 1, NULL, 0) : 0;
  char *p3 = p2 ? strchr(p2 + 1, ' ') : NULL;
  long c = p3 ? strtol(p3 + 1, NULL, 0) : 0;
  snprintf(cur, sizeof cur, "%s %.30s", s, arg);
  dirty = 1;
  if (!strcmp(s, "PING")) say(ch, "PONG %s t=%lu", arg, (unsigned long)now());
  else if (!strcmp(s, "IO")) {
    int val = (p2 && (p2[1] == 'R' || p2[1] == 'r')) ? RD : (int)b;
    int r = usb_serial_ioctl((int)a, val);
    say(ch, "IO 0x%02lX %s -> %d (0x%08lX)", a, val == RD ? "R" : p2 ? p2 + 1 : "?", r, (unsigned long)r);
    logline("IO 0x%02lX %s -> %d", a, val == RD ? "R" : p2 ? p2 + 1 : "?", r);
  } else if (!strcmp(s, "ECHO")) say(ch, "ECHO %s", arg);
  else if (!strcmp(s, "BUF")) { bufsz = a < 1 ? 1 : a > USIN_CHAN_SIZE ? USIN_CHAN_SIZE : (int)a; say(ch, "BUF %d", bufsz); }
  else if (!strcmp(s, "VERBOSE")) { verbose = (int)a; say(ch, "VERBOSE %d", verbose); }
  else if (!strcmp(s, "HOLD")) { hold_until = now() + (uint32_t)a; hold_ch = ch; say(ch, "HOLD %ld", a); logline("HOLD %ld", a); }
  else if (!strcmp(s, "BURST")) burst(ch, (int)a, (int)b, (int)c);
  else if (!strcmp(s, "BIG")) {
    int n = a < 2 ? 2 : a > BIGSZ ? BIGSZ : (int)a;
    big[0] = 'G';
    for (int i = 1; i < n - 1; i++) big[i] = (char)('0' + i % 10);
    big[n - 1] = '\n';
    out(ch, big, n);
    sys_delay(150);
    out(ch, "\nBIGEND\n", 8);
    logline("BIG %d", n);
  } else if (!strcmp(s, "MULTI")) { out(ch, "M1\nM2\nM3\n", 9); logline("MULTI"); }
  else if (!strcmp(s, "CHAN")) {
    char t[CAP];
    int n = snprintf(t, CAP - 1, "%s", p2 ? p2 + 1 : "x");
    t[n++] = '\n';
    out((int)a, t, n);
    sys_delay(50);
    say(ch, "CHAN %ld sent", a);
    logline("CHAN %ld", a);
  } else if (!strcmp(s, "PRINTF")) {
    int w = io(SERIO_SEND_PRINTF);
    int r = usb_serial_ioctl(SERIO_SEND_PRINTF, 1);
    printf("PRINTF hello %d\n", 42);
    fflush(stdout);
    sys_delay(100);
    usb_serial_ioctl(SERIO_SEND_PRINTF, w);
    say(ch, "PRINTF was=%d set1->%d now=%d", w, r, io(SERIO_SEND_PRINTF));
    logline("PRINTF was=%d", w);
  } else if (!strcmp(s, "LCD")) say(ch, "LCD %d -> %d", (int)a, usb_serial_ioctl(SERIO_SEND_LCD, (int)a));
  else if (!strcmp(s, "KEYS")) say(ch, "KEYS %d -> %d", (int)a, usb_serial_ioctl(SERIO_SEND_KEYS, (int)a));
  else if (!strcmp(s, "PATTERN")) { pattern = 1; draw(); say(ch, "PATTERN on"); }
  else if (!strcmp(s, "SCREEN")) { pattern = 0; draw(); say(ch, "SCREEN on"); }
  else if (!strcmp(s, "NO77")) { no77 = (int)a; say(ch, "NO77 %d", no77); }
  else if (!strcmp(s, "NOWAKE")) {
    nowake_until = now() + (uint32_t)a;
    say(ch, "NOWAKE %ld armed", a);
    logline("NOWAKE %ld armed", a);
  } else if (!strcmp(s, "FLOOD")) flood(ch, (int)a, (int)b);
  else if (!strcmp(s, "LCDFLOOD")) lcdflood(ch, (int)a);
  else if (!strcmp(s, "RESULT")) say(ch, "%s", last_result[0] ? last_result : "none");
  else if (!strcmp(s, "REPORT")) report(ch);
  else if (!strcmp(s, "CLEARLOG")) { logn = 0; loglines = 0; if (logb) logb[0] = 0; say(ch, "CLEARLOG"); }
  else if (!strcmp(s, "QUIT")) quit(ch);
  else say(ch, "?? %.20s", s);
}

/* the PC's lines, channels 2 and 3 (the only input channels a program can read) */
static void poll_lines(void) {
  if (hold_until) {
    if ((int32_t)(now() - hold_until) < 0) return;
    hold_until = 0;
    say(hold_ch, "HOLDEND ready2=%d len2=%d ready3=%d len3=%d", usb_serial_ioctl(SERIO_LINE_READY, 2),
        usb_serial_ioctl(SERIO_LINE_LENGTH, 2), usb_serial_ioctl(SERIO_LINE_READY, 3),
        usb_serial_ioctl(SERIO_LINE_LENGTH, 3));
  }
  for (int ch = 2; ch <= 3; ch++)
    for (int i = 0; i < USIN_MAX_READLINES + 4; i++) {
      int ready = usb_serial_ioctl(SERIO_LINE_READY, ch);
      if (ready <= 0) break;
      int len = usb_serial_ioctl(SERIO_LINE_LENGTH, ch);
      memset(inb, 0xAA, sizeof inb);
      uint8_t ovf = 0;
      int n = usb_serial_read_line((uint8_t)ch, inb, bufsz, &ovf);
      if (verbose)
        say(ch, "RX ch=%d ready=%d len=%d n=%d ovf=%d last=%02X after=%02X head=%.12s", ch, ready, len, n, ovf,
            n > 0 ? inb[n - 1] : 0, n > 0 && n < (int)sizeof inb ? inb[n] : 0, n > 0 ? (char *)inb : "");
      if (n == 0) break;
      if (n < 0) {
        int r = usb_serial_read_line((uint8_t)ch, NULL, bufsz, NULL);
        say(ch, "RXLONG ch=%d n=-1 discard->%d ready_after=%d", ch, r, usb_serial_ioctl(SERIO_LINE_READY, ch));
        logline("RXLONG ch=%d discard->%d", ch, r);
        continue;
      }
      n_in++;
      if (ovf || inb[n - 1] != '\n') { logline("RX ch=%d n=%d ovf=%d last=%02X dropped", ch, n, ovf, inb[n - 1]); continue; }
      inb[n - 1] = 0;
      if (n > 1 && inb[n - 2] == '\r') inb[n - 2] = 0;
      command(ch, (char *)inb);
      if (ST(STAT_PGM_END)) return;
    }
}

static void check_conn(void) {
  int a = io(SERIO_IS_ACTIVE), c = io(SERIO_IS_CONNECTED);
  if (a == act && c == conn) return;
  logline("STATE active %d->%d connected %d->%d ready2=%d", act, a, conn, c, usb_serial_ioctl(SERIO_LINE_READY, 2));
  if (c > 0 && conn != c) say(2, "HELLO SERTEST " PROGRAM_VERSION " ready2=%d (lines waiting at connect)",
                               usb_serial_ioctl(SERIO_LINE_READY, 2));
  act = a;
  conn = c;
  dirty = 1;
}

/* after a NOWAKE sleep: why did sys_sleep() return? */
static void after_nowake(void) {
  uint32_t t = now(), slept = t - sleep_t0;
  int tm = sys_timer_timeout(T_POLL), ln = usb_serial_ioctl(SERIO_LINE_READY, 2) > 0, ky = !key_empty();
  const char *why = tm ? (ln ? "timer(line_waiting)" : "timer") : ln ? "line" : ky ? "key" : "other";
  logline("WAKE %s slept=%lums timer=%d line=%d key=%d", why, (unsigned long)slept, tm, ln, ky);
  if (ln || (int32_t)(t - nowake_until) >= 0) {
    nowake_until = 0;
    result(2, "NOWAKEEND woke=%s slept=%lums timer_expired=%d line_waiting=%d", why, (unsigned long)slept, tm, ln);
  }
}

void _exit(int code) { (void)code; sys_reset(); for (;;) {} }

void program_main(void) {
  run_menu_item_app = NULL;
  menu_line_str_app = NULL;
  logb = malloc(LOGSZ + 1);
  if (logb) logb[0] = 0;
  for (int i = 0; i < NOPS; i++) was[i] = io(OPS[i]);
  read_all("BOOT read");
  logline("BOOT invalid op 0x7F read -> %d, write IS_ACTIVE 1 -> %d", usb_serial_ioctl(0x7F, RD),
          usb_serial_ioctl(SERIO_IS_ACTIVE, 1));
  char s[120];
  int n = 0;
  for (int ch = 0; ch <= 10; ch++) n += sprintf(s + n, " %d", usb_serial_ioctl(SERIO_LINE_READY, ch));
  logline("BOOT LINE_READY ch0..10:%s", s);
  logline("BOOT IS_ENABLED 1 -> %d", usb_serial_ioctl(SERIO_IS_ENABLED, 1));
  read_all("BOOT after enable");
  draw();
  for (;;) {
    if (!ST(STAT_PGM_END)) {
      check_conn();
      poll_lines();
      if (dirty && !ST(STAT_PGM_END)) draw();
    }
    if ((ST(STAT_PGM_END) && ST(STAT_SUSPENDED)) || (!ST(STAT_PGM_END) && key_empty())) {
      uint32_t t = now();
      if (nowake_until) {                              /* no poll timer: only the fallback at the end */
        int32_t left = (int32_t)(nowake_until - t);
        sys_timer_start(T_POLL, left > 1 ? (uint32_t)left : 1);
        sleep_t0 = t;
      } else sys_timer_start(T_POLL, act > 0 ? 50 : 1000);
      CLR_ST(STAT_RUNNING);
      sys_sleep();
      if (nowake_until && !ST(STAT_PGM_END)) after_nowake();
    }
    if (ST(STAT_PGM_END) || ST(STAT_SUSPENDED)) {
      if (!ST(STAT_SUSPENDED)) {
        lcd_set_buf_cleared(0);
        draw_power_off_image(1);
        LCD_power_off(0);
        SET_ST(STAT_SUSPENDED);
        SET_ST(STAT_OFF);
      }
      continue;
    }
    SET_ST(STAT_RUNNING);
    CLR_ST(STAT_SUSPENDED);
    if (ST(STAT_OFF)) { LCD_power_on(); rtc_wakeup_delay(); CLR_ST(STAT_OFF); dirty = 1; }
    if (ST(STAT_POWER_CHANGE)) { switch_usb_powered_freq(); CLR_ST(STAT_POWER_CHANGE); }
    int k = key_pop();
    if (k < 0) continue;
    int k1 = -1, k2 = -1;
    sys_last_scan(&k1, &k2);              /* the keys held now */
    logline("KEY %d held=%d,%d", k, k1, k2);
    say(2, "KEY %d held=%d,%d t=%lu", k, k1, k2, (unsigned long)now());
    if (k == 0) continue;
    nkeys++;
    dirty = 1;
    if (k == SERCON_KEY_REDRAW) {                       /* the PC sent L */
      n77++;
      if (!no77) { draw(); lcd_forced_refresh(); }
      dirty = 0;
      continue;
    }
    if (k == KEY_F4) save();
    if (k == KEY_EXIT || k == KEY_F6) {               /* EXIT or F6: the settings back, the log saved, the end */
      restore();
      read_all("AFTER EXIT");
      save();
      SET_ST(STAT_PGM_END);
    }
  }
}
