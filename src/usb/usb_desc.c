/* usb_desc.c - the DM42n's USB identity while a 47 program drives the port: one CDC serial port (/dev/ttyACM0).
 * VID 0xCAFE is TinyUSB's test vendor ID (for personal use). GPL-3.0-or-later. */
#include "tusb.h"
#include "usbser.h"

#define PID 0x4047
static const tusb_desc_device_t DEV = {
  .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE, .bcdUSB = 0x0200,
  .bDeviceClass = TUSB_CLASS_MISC, .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE, .idVendor = 0xCAFE, .idProduct = PID, .bcdDevice = 0x0100,
  .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
};
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&DEV; }

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_TOTAL };
#define EP_NOTIF 0x81
#define EP_OUT   0x02
#define EP_IN    0x82
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
static const uint8_t CONF[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0x00, 100),
  TUD_CDC_DESCRIPTOR(ITF_CDC, 4, EP_NOTIF, 8, EP_OUT, EP_IN, 64),
};
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return CONF; }

static uint16_t str[32];
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  const char *s;
  switch (index) {
  case 0: str[1] = 0x0409; str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4); return str;
  case 1: s = "SwissMicros DM42n"; break;
  case 2: s = usbser_product(); break;
  case 3: s = "47"; break;
  case 4: s = "47 serial"; break;
  default: return NULL;
  }
  int n = 0;
  for (; s[n] && n < 31; n++) str[1 + n] = (uint16_t)s[n];
  str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return str;
}
