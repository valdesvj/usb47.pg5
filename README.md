# USB47 — the DM42n's USB hardware, step by step

A small DMCP5 test program (`USB47.pg5`) for the SwissMicros DM42n (STM32U575). The goal: USB **in and out** from a
.pg5 (a CDC serial port with a receive function, which the DMCP SDK lacks: it has only `usb_acm_on` and `acm_puts`).

**Step 1 (this version): the register probe.** It only reads: the core (CPUID, VTOR, SAU), the OTG_FS interrupt
vector, FLASH OPTR (TrustZone), RCC and PWR (the USB clock and supply), and the USB OTG_FS core registers. Each read
is shown before it is made, so if one faults (TrustZone keeping USB for the firmware) the screen still shows which.

| Key | |
|---|---|
| F1 READ | read again |
| F2 ACM ON | `usb_acm_on()`, then read: what the firmware changed is shown inverted |
| F3 ACM OF | `usb_turn_off()` |
| F4 SAVE | `/USB47.TXT`: both readings, to send back |
| EXIT | the end |

Next steps: 2) TinyUSB CDC in polling mode, echoing what Linux sends; 3) the code into NAV47 and Chess 47.

Build: `make` (arm-none-eabi-gcc). Copyright (C) 2026 Victor Valdes, GPL-3.0-or-later. `dmcp/`, `stm32_program.ld`:
SwissMicros DMCP5 SDK (BSD-3).
