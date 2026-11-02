/* usbser.h - the DM42n's USB as a serial port (/dev/ttyACM0), send AND receive, driven by the program itself
 * with TinyUSB: the DMCP firmware's usb_acm_on() leaves the port disconnected (USB47 step 1: DCTL.SDIS stays 1).
 * GPL-3.0-or-later. */
#ifndef USB47_USBSER_H
#define USB47_USBSER_H
#include <stdint.h>

int  usbser_start(const char *product);   /* 0 ok, -1 no USB power, -2 no memory */
void usbser_stop(void);
int  usbser_active(void);
int  usbser_connected(void);              /* the PC has the port open (DTR) */
int  usbser_mounted(void);                /* the PC has enumerated the device */
void usbser_task(void);                   /* call often: TinyUSB's device task */
int  usbser_write(const void *p, int n);  /* waits while the PC reads; n sent, or -1 (timeout / not open) */
int  usbser_puts(const char *s);
int  usbser_read(void *p, int max);       /* what the PC sent: up to max bytes, 0 if none */
/* instead of sys_sleep() while the port is on: runs the USB until a key, or ms milliseconds */
void usbser_idle(uint32_t ms);
const char *usbser_product(void);
#endif
