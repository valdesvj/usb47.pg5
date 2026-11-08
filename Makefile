######################################
# USB47 - DMCP5 test program for SwissMicros DM42n
# Based on the SwissMicros DMCP5_SDK Makefile (BSD-3)
######################################
TARGET ?= USB47

ifdef DEBUG
DEBUG = 1
endif

BUILD_DIR = build
PYTHON ?= python3

######################################
# Sources
######################################
ASM_SOURCES = dmcp/startup_pgm.s
ifeq ($(TARGET),SERTEST)
# make sertest: the DMCP5 3.58 serial console test (IFC 18: needs DMCP5 3.58), its own build/sertest/
BUILD_DIR = build/sertest
QSPI_H = src/sertest/qspi_crc.h
C_INCLUDES += -Isrc/sertest -Idmcp -Isrc
C_SOURCES  += dmcp/sys/pgm_syscalls.c src/sertest/sertest.c src/ntext.c src/fonts21.c
C_DEFS += -DPLATFORM_IFC_VER=18
else
QSPI_H = src/qspi_crc.h
C_INCLUDES += -Idmcp -Isrc -Isrc/usb -Ithird_party/tinyusb/src
C_SOURCES  += dmcp/sys/pgm_syscalls.c
C_SOURCES += src/main.c src/ntext.c src/fonts21.c
C_SOURCES += src/usb/usbser.c src/usb/usb_desc.c
TUSB = third_party/tinyusb/src
C_SOURCES += $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
             $(TUSB)/class/cdc/cdc_device.c $(TUSB)/portable/synopsys/dwc2/dcd_dwc2.c $(TUSB)/portable/synopsys/dwc2/dwc2_common.c
endif

#######################################
# Toolchain
#######################################
CC      = arm-none-eabi-gcc
AS      = arm-none-eabi-gcc -x assembler-with-cpp
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

C_DEFS  += -D__weak="__attribute__((weak))" -D__packed="__attribute__((__packed__))"
CPUFLAGS += -mthumb -march=armv7e-m -mfloat-abi=hard -mfpu=fpv4-sp-d16

CFLAGS  = -std=gnu11 $(CPUFLAGS) $(C_DEFS) $(C_INCLUDES) -Wall -fdata-sections -ffunction-sections
CFLAGS += -Wno-misleading-indentation
ifeq ($(DEBUG), 1)
CFLAGS += -O0 -g -DDEBUG
else
CFLAGS += -O2 -g
endif
CFLAGS += -MD -MP -MF .dep/$(TARGET)_$(@F).d

LDSCRIPT = stm32_program.ld
LDFLAGS  = $(CPUFLAGS) -T$(LDSCRIPT) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections \
           -Wl,--wrap=_malloc_r -g

all: $(BUILD_DIR)/$(TARGET).pg5

OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

# QSPI CRC is compiled into the program header: if it changes, rebuild once.
$(BUILD_DIR)/$(TARGET).pg5: $(BUILD_DIR)/$(TARGET).elf tools/pg5tool.py
	$(OBJCOPY) --remove-section .qspi -O binary $< $(BUILD_DIR)/$(TARGET)_flash.bin
	$(OBJCOPY) --only-section   .qspi -O binary $< $(BUILD_DIR)/$(TARGET)_qspi.bin
	@if ! $(PYTHON) tools/pg5tool.py qspi $(BUILD_DIR)/$(TARGET)_qspi.bin $(QSPI_H); then \
	   rm -f $(BUILD_DIR)/pgm_syscalls.o $<; $(MAKE) --no-print-directory $@; exit $$?; fi
	$(PYTHON) tools/pg5tool.py pg5 $(BUILD_DIR)/$(TARGET)_flash.bin $@
	$(SIZE) $<

$(BUILD_DIR):
	mkdir -p $@

# the serial console test: build/sertest/SERTEST.pg5; build/sertest/sim, its PC simulator (a pty as the port)
sertest:
	$(MAKE) --no-print-directory TARGET=SERTEST

SIM_SRC = test/sim/sertest_sim.c src/sertest/sertest.c src/ntext.c src/fonts21.c
sertest-sim: $(SIM_SRC)
	@mkdir -p build/sertest
	gcc -std=gnu11 -O2 -Wall -Wno-unused-function '-D__packed=__attribute__((__packed__))' \
	    '-D__weak=__attribute__((weak))' -DNAV47_SIM -Itest/sim -Isrc/sertest -Isrc -o build/sertest/sim $(SIM_SRC)

sertest-test: sertest-sim
	$(PYTHON) test/test_sertest.py

clean:
	-rm -fR .dep $(BUILD_DIR)

-include $(shell mkdir .dep 2>/dev/null) $(wildcard .dep/*)

.PHONY: clean all test sertest sertest-sim sertest-test
