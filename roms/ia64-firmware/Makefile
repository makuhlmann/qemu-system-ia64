# SPDX-License-Identifier: GPL-2.0-or-later
# Thin wrapper around the canonical firmware build script.

CROSS   ?= ia64-linux-gnu-
AS      = $(CROSS)as
CC      = $(CROSS)gcc
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

BUILD_SCRIPT := build_firmware.sh
MANIFEST := firmware.sources
SOURCES := $(shell sed -e '/^[[:space:]]*$$/d' -e '/^[[:space:]]*#/d' $(MANIFEST))
OUTPUTS := ia64-firmware.bin ia64-firmware.raw ia64-firmware.elf \
	ia64-firmware.map ia64-firmware.sections
DEPFILE := ia64-firmware.d

all: $(OUTPUTS)

$(OUTPUTS) &: $(BUILD_SCRIPT) $(MANIFEST) $(SOURCES)
	AS="$(AS)" CC="$(CC)" LD="$(LD)" OBJCOPY="$(OBJCOPY)" SIZE="$(SIZE)" \
		./$(BUILD_SCRIPT) ia64-firmware.bin ia64-firmware.raw ia64-firmware.elf \
		ia64-firmware.map ia64-firmware.sections . $(DEPFILE)

-include $(DEPFILE)

clean:
	rm -f ia64-fw-*.o ia64-fw-*.o.d $(OUTPUTS) $(DEPFILE)

.PHONY: all clean
