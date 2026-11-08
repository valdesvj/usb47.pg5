# USB47 — the DM42n's USB hardware, step by step

A small DMCP5 test program (`USB47.pg5`) for the SwissMicros DM42n (STM32U575). The goal: USB **in and out** from a
.pg5 (a CDC serial port with a receive function, which the DMCP SDK lacks: it has only `usb_acm_on` and `acm_puts`).

**Step 2 (SERIAL, F5): our own USB serial port with TinyUSB**: `/dev/ttyACM0` appears; it sends `USB47 hello N` every second and echoes what Linux sends (shown on screen). **Step 1: the register probe.** It only reads: the core (CPUID, VTOR, SAU), the OTG_FS interrupt
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

## SERTEST: DMCP5 3.58's serial console, item by item

`SERTEST.pg5` tests every item of SwissMicros' preliminary data sheet "DMCP5 Serial Console API" (DMCP5 3.58,
IFC 3.18), with no TinyUSB: the program is a command server on channels 2 and 3, and `tools/sertest.py` on the PC
drives it, checks each answer and writes the log to send to SwissMicros.

    make sertest                                      # build/sertest/SERTEST.pg5 (IFC 18: only on DMCP5 3.58)
    python3 tools/sertest.py /dev/ttyACM0             # about 3 minutes, nothing to press
    python3 tools/sertest.py /dev/ttyACM0 --manual    # + a real key, unplugging the cable, DMCP's menu

It writes `sertest_<date>.log` (each item PASS / FAIL / MEASURED / INFO, with the detail, and a summary table with
the columns NAV47 and CHESS47: which of those programs use the item) and `sertest_<date>_wire.txt` (every line on
the wire, with times). The two UNKNOWNs of the sheet are measured: T1 (does a line wake `sys_sleep()`: NOWAKE sleeps
with no poll timer) and WR4 / WR4b (the port open but the host not reading: FLOOD and LCDFLOOD, every write timed).
The bootloader command B is not tested. Leaving (QUIT or EXIT) puts your DMCP settings back and saves the
calculator's own log to `/SERTEST.TXT`.

The PC simulator: `make sertest-sim` (build/sertest/sim: a pseudo-terminal as the port, the console as the sheet
says it, its choices for the unknowns set by environment variables); `make sertest-test` runs sertest.py against it.
