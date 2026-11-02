/*
 * USB47 step 1: the USB register probe; step 2 (SERIAL): our own USB serial port with TinyUSB, an echo test for the SwissMicros DM42n (DMCP5, STM32U575).
 * Reads the chip's USB registers to find out whether a .pg5 can reach the USB hardware (or whether TrustZone keeps it
 * for the firmware), and what DMCP's usb_acm_on() changes. It only reads registers; the firmware calls are the SDK's
 * own (usb_acm_on, usb_turn_off). The screen keeps its picture if a read faults: the last "reading ..." line shows
 * which register it was.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
 */
#include <main.h>
#include <dmcp.h>
#include <stdio.h>
#include <string.h>
#include "ntext.h"
#include "usbser.h"

#define OTG 0x42040000u                       /* USB OTG_FS, non-secure alias (RM0456) */
typedef struct { const char *name; uint32_t addr; } Reg;
enum { V_POWER = 0, V_ISON, V_VECT, NSPECIAL };
static const Reg REGS[] = {
  { "usb_powered", 0 }, { "usb_is_on", 0 }, { "VEC OTG_FS", 0 },
  { "CPUID", 0xE000ED00u }, { "VTOR", 0xE000ED08u }, { "SAU_CTRL", 0xE000EDD0u }, { "DBG IDCODE", 0xE0044000u },
  { "FLASH OPTR", 0x40022040u }, { "RCC AHB2ENR1", 0x46020C8Cu }, { "RCC CCIPR1", 0x46020CE0u },
  { "PWR SVMCR", 0x46020810u },
  { "OTG GOTGCTL", OTG + 0x000 }, { "OTG GAHBCFG", OTG + 0x008 }, { "OTG GUSBCFG", OTG + 0x00C },
  { "OTG GINTSTS", OTG + 0x014 }, { "OTG GINTMSK", OTG + 0x018 }, { "OTG GCCFG", OTG + 0x038 },
  { "OTG CID", OTG + 0x03C }, { "OTG GSNPSID", OTG + 0x040 }, { "OTG DCFG", OTG + 0x800 },
  { "OTG DCTL", OTG + 0x804 }, { "OTG DSTS", OTG + 0x808 },
};
#define NREG ((int)(sizeof REGS / sizeof *REGS))
#define OTG_IRQ 73                            /* OTG_FS_IRQn on the STM32U5 */

#ifdef USB47_SIM                              /* PC layout check only: made-up values */
#define RD(a) ((uint32_t)(a) * 2654435761u)
#else
#define RD(a) (*(volatile uint32_t *)(a))
#endif

static uint32_t A[NREG], B[NREG];             /* before and after usb_acm_on */
static int haveA, haveB, nread, acm, watch;
static int ser;                               /* step 2: our own USB serial port (TinyUSB) */
static char rx[8][52];                        /* the last lines the PC sent */
static int nrx, sent_n;
static char rxline[52];
static int rxlen;
static char msg[64];

static void row(int i, int y, const uint32_t *v, int have, const uint32_t *ref) {
  char s[40];
  int x = (i & 1) ? 202 : 2;
  if (have > i) sprintf(s, "%-12s %08lX", REGS[i].name, (unsigned long)v[i]);
  else sprintf(s, "%-12s ........", REGS[i].name);
  if (ref && have > i && v[i] != ref[i]) nt_inv(x, y, s); else nt(x, y, s);
}

static void draw_ser(void) {
  char s[64];
  nt_inv(2, 0, "USB47 step 2: own USB serial (TinyUSB)");
  lcd_fill_rect(0, 15, LCD_X, 1, 1);
  sprintf(s, "port %s   PC %s   PC has it open %s", usbser_active() ? "ON" : "off", usbser_mounted() ? "sees it" : "-",
          usbser_connected() ? "YES" : "no");
  nt(2, 20, s);
  sprintf(s, "DCTL %08lX  GINTSTS %08lX", (unsigned long)RD(0x42040804u), (unsigned long)RD(0x42040014u));
  nt(2, 38, s);
  sprintf(s, "sent %d lines. Linux: cat /dev/ttyACM0", sent_n);
  nt(2, 56, s);
  nt(2, 76, "received (echoed back):");
  for (int i = 0; i < 7; i++) nt(10, 92 + NT_ROW * i, rx[i]);
}

static void draw(void) {
  lcd_clear_buf();
  if (ser) {
    draw_ser();
    nt(2, 192, msg);
    static const char *k2[6] = { "", "", "", "", "SERIAL", "EXIT" };
    lcd_draw_menu_keys(k2);
    lcd_refresh();
    lcd_refresh_wait();
    return;
  }
  nt_inv(2, 0, "USB47 step 1: USB registers");
  nt(230, 0, acm ? "after usb_acm_on" : "now");
  lcd_fill_rect(0, 15, LCD_X, 1, 1);
  const uint32_t *v = acm ? B : A;
  int have = acm ? (haveB ? NREG : nread) : (haveA ? NREG : nread);
  for (int i = 0; i < NREG; i++) row(i, 17 + NT_ROW * (i / 2), v, have, acm && haveA ? A : NULL);
  nt(2, 192, msg);
  static const char *keys[6] = { "READ", "ACM ON", "ACM OF", "SAVE", "SERIAL", "EXIT" };
  lcd_draw_menu_keys(keys);
  lcd_refresh();
  lcd_refresh_wait();
}

/* one register at a time, the screen updated before each read: if a read faults, the picture stays */
static void read_all(uint32_t *v, int *have) {
  nread = 0;
  for (int i = 0; i < NREG; i++) {
    snprintf(msg, sizeof msg, "reading %s ...", REGS[i].name);
    draw();
    if (i == V_POWER) v[i] = (uint32_t)usb_powered();
    else if (i == V_ISON) v[i] = (uint32_t)usb_is_on();
    else if (i == V_VECT) {
      uint32_t vtor = RD(0xE000ED08u);
      v[i] = RD(vtor + 4u * (16 + OTG_IRQ));
    } else v[i] = RD(REGS[i].addr);
    nread = i + 1;
  }
  *have = 1;
  snprintf(msg, sizeof msg, "all %d read. inverted = changed", NREG);
}

static void save(void) {
  if (!sys_disk_ok()) { strcpy(msg, "no disk"); return; }
  char b[2048], *o = b;
  o += sprintf(o, "USB47 step 1 (DM42n USB registers)\nname before_acm after_acm\n");
  for (int i = 0; i < NREG; i++)
    o += sprintf(o, "%-12s %08lX %08lX\n", REGS[i].name, haveA ? (unsigned long)A[i] : 0ul, haveB ? (unsigned long)B[i] : 0ul);
  sys_disk_write_enable(1);
  FIL f;
  UINT bw;
  if (f_open(&f, "/USB47.TXT", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) { f_write(&f, b, (UINT)strlen(b), &bw); f_close(&f); strcpy(msg, "saved /USB47.TXT"); }
  else strcpy(msg, "cannot write /USB47.TXT");
  sys_disk_write_enable(0);
}

static void ser_toggle(void) {
  if (ser) { usbser_stop(); ser = 0; strcpy(msg, "own serial port off"); return; }
  int r = usbser_start("USB47 test");
  if (r == -1) { strcpy(msg, "no USB power: connect the cable"); return; }
  if (r == -2) { strcpy(msg, "no memory"); return; }
  ser = 1;
  nrx = 0; sent_n = 0; rxlen = 0;
  memset(rx, 0, sizeof rx);
  strcpy(msg, "on: dmesg should show cdc_acm");
}

static void ser_poll(void) {                     /* echo what the PC sends; a line a second to the PC */
  static uint32_t last;
  uint8_t b[64];
  int n = usbser_read(b, sizeof b);
  for (int i = 0; i < n; i++) {
    char c = (char)b[i];
    if (c == '\r' || c == '\n' || rxlen >= 50) {
      if (rxlen) {
        rxline[rxlen] = 0;
        memmove(rx[0], rx[1], sizeof rx[0] * 6);
        strcpy(rx[6], rxline);
        nrx++;
        usbser_puts("echo: "); usbser_puts(rxline); usbser_puts("\r\n");
        rxlen = 0;
      }
    } else if (c >= 32 && c < 127) rxline[rxlen++] = c;
  }
  if (sys_current_ms() - last >= 1000) {
    last = sys_current_ms();
    if (usbser_connected()) {
      char t[48];
      sprintf(t, "USB47 hello %d\r\n", sent_n + 1);
      if (usbser_puts(t) > 0) sent_n++;
    }
    draw();
  } else if (n) draw();
}

void _exit(int code) { (void)code; sys_reset(); for (;;) {} }

void program_main(void) {
  run_menu_item_app = NULL;
  menu_line_str_app = NULL;
  switch_usb_powered_freq();
  read_all(A, &haveA);
  draw();
  for (;;) {
    if (ser && !ST(STAT_PGM_END)) {              /* the USB runs: no sleep (it would stop the USB) */
      usbser_idle(50);
      ser_poll();
      if (key_empty()) continue;
    } else if ((ST(STAT_PGM_END) && ST(STAT_SUSPENDED)) || (!ST(STAT_PGM_END) && key_empty())) {
      if (acm) sys_timer_start(0, 1000);        /* after ACM ON: read again every second, the firmware running between */
      CLR_ST(STAT_RUNNING);
      sys_sleep();
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
    if (ST(STAT_OFF)) { LCD_power_on(); rtc_wakeup_delay(); CLR_ST(STAT_OFF); draw(); }
    if (ST(STAT_POWER_CHANGE)) { switch_usb_powered_freq(); CLR_ST(STAT_POWER_CHANGE); }
    int k = key_pop();
    if (k <= 0) {
      if (acm && !ser && key_empty()) {
        read_all(B, &haveB);
        snprintf(msg, sizeof msg, "watching %d s: DCTL bit 1 = 0 is connected", ++watch);
        draw();
      }
      continue;
    }
    switch (k) {
    case KEY_F1: if (ser) break; if (acm) read_all(B, &haveB); else read_all(A, &haveA); break;
    case KEY_F5: ser_toggle(); break;
    case KEY_F2:
      if (ser || usb_is_on()) { strcpy(msg, "already on (twice crashes the firmware)"); break; }
      strcpy(msg, "usb_acm_on() ...");
      draw();
      usb_acm_on();
      sys_delay(500);
      acm = 1;
      watch = 0;
      read_all(B, &haveB);
      acm_puts("USB47 hello\r\n");
      snprintf(msg, sizeof msg, "acm on: inverted = changed. dmesg?");
      break;
    case KEY_F3: if (ser) break; usb_turn_off(); sys_delay(200); acm = 0; read_all(A, &haveA); strcpy(msg, "usb_turn_off()"); break;
    case KEY_F4: if (!ser) save(); break;
    case KEY_EXIT: case KEY_F6: if (ser) { usbser_stop(); ser = 0; } SET_ST(STAT_PGM_END); break;
    }
    draw();
  }
}
