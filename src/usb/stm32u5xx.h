/* stm32u5xx.h - the few STM32U575 / CMSIS names TinyUSB's DWC2 driver needs, instead of ST's 2 MB headers.
 * Values from ST's cmsis-device-u5 stm32u575xx.h (RM0456). GPL-3.0-or-later (NAV47 / Chess 47 / USB47). */
#ifndef USB47_STM32U5XX_SHIM_H
#define USB47_STM32U5XX_SHIM_H
#include <stdint.h>

typedef enum { OTG_FS_IRQn = 73 } IRQn_Type;
#define USB_OTG_FS_BASE  0x42040000UL                 /* non-secure alias (TrustZone is off on the DM42n) */
#define USB_OTG_FS       ((void *)USB_OTG_FS_BASE)
extern uint32_t SystemCoreClock;

#define __NOP() __asm volatile ("nop")
#define __DSB() __asm volatile ("dsb 0xF" ::: "memory")
#define __ISB() __asm volatile ("isb 0xF" ::: "memory")

static inline void NVIC_EnableIRQ(IRQn_Type n) {
  ((volatile uint32_t *)0xE000E100UL)[(uint32_t)n >> 5] = 1u << ((uint32_t)n & 31u);
}
static inline void NVIC_DisableIRQ(IRQn_Type n) {
  ((volatile uint32_t *)0xE000E180UL)[(uint32_t)n >> 5] = 1u << ((uint32_t)n & 31u);
  __DSB();
  __ISB();
}
#endif
