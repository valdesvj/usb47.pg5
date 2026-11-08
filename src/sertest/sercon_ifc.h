/* sercon_ifc.h - the DMCP5 3.58 serial console calls (IFC 3.18), from SwissMicros' preliminary data sheet
 * "DMCP5 Serial Console API" (2026-11-06): its section 6 (the three library slots and the ioctl table) and the
 * constants it names. Replace with the official dmcp.h / lft_ifc.h of DMCP5 3.58 when you have them.
 *
 * Only for a .pg5 built with PLATFORM_IFC_VER 18 (make USB=sercon): on an older DMCP5 these slots hold other
 * functions and calling one crashes the calculator; DMCP5's loader refuses a program with a newer IFC than its own,
 * so a build with IFC 18 does not even load there.
 * GPL-3.0-or-later (NAV47); the names and values are SwissMicros'. */
#ifndef NAV47_SERCON_IFC_H
#define NAV47_SERCON_IFC_H
#include <stdint.h>

#define US_CHAN0_CHAR       0x10      /* a byte 0x10 + n switches the stream to channel n (up to 0x1a) */
#define US_LINEEND_CHAR     '\n'
#define US_OVERFLOW_CHAR    0x0f
#define USIN_CHAN_SIZE      256       /* bytes buffered per input channel */
#define USIN_MAX_READLINES  16

#define SER_IOCTL_READ      (1 << 31) /* val: read the switch */
#define SER_IOCTL_INVALID   (1 << 31) /* returned for an unknown op */

#define SERIO_IS_ENABLED    0x01      /* RW the serial console on / off (DMCP's menu item) */
#define SERIO_IS_ACTIVE     0x02      /* R  enabled and the USB cable in */
#define SERIO_IS_CONNECTED  0x03      /* R  a host has the port open (DTR) */
#define SERIO_LINE_READY    0x08      /* R  val = channel: > 0 when a whole line waits */
#define SERIO_LINE_LENGTH   0x09      /* R  val = channel: the length of that line */
#define SERIO_SEND_PRINTF   0x10      /* RW printf to output channel 1 */
#define SERIO_SEND_LCD      0x11      /* RW DMCP streams the changed LCD rows on channel 0 */
#define SERIO_SEND_KEYS     0x12      /* RW DMCP streams the key changes on channel 0 */
#define SERIO_SEND_IR       0x13      /* RW IR printer traffic over the console */

#define SERCON_KEY_REDRAW   77        /* DMCP's key code when the host sent L: draw the screen, lcd_forced_refresh() */

int  usb_serial_read_line(uint8_t chan_nr, uint8_t *buf, int buflen, uint8_t *is_overflow);
void usb_serial_write(uint8_t chan_nr, const char *buf, int len);
int  usb_serial_ioctl(int op, int val);

#ifndef NAV47_SIM                     /* the PC simulator links its own (test/sim/sercon_sim.c) */
#include <lft_ifc.h>
#define usb_serial_read_line (*(typeof(usb_serial_read_line)*)(LIBRARY_FN_BASE+804))
#define usb_serial_write     (*(typeof(usb_serial_write)*)(LIBRARY_FN_BASE+808))
#define usb_serial_ioctl     (*(typeof(usb_serial_ioctl)*)(LIBRARY_FN_BASE+812))
#endif

#endif
