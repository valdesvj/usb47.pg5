# USB47: working rules

- Work on a new branch; Victor creates and merges the PRs. Never force-push or rewrite history.
- Commit author: "Victor Valdes" <valdesvj@users.noreply.github.com>.
- A test program for the DM42n's USB (STM32U575): it may read and write the USB hardware; it must give the USB
  back to the firmware (usbser_stop) before it ends. Never call the firmware's usb_acm_on() twice (it asserts,
  dm/hw/usb_hw.c:87).
- src/usb/ (usbser, descriptors, the CMSIS shim) is shared with nav47.pg5 and chess47.pg5: a fix here goes there too.
- third_party/tinyusb: TinyUSB 0.21.0 (MIT), only the device stack, CDC and DWC2; never edited.
- Nothing from build/ is committed. Ask before anything that can't be undone.
