# PicoDeck Native App Build
#
# Produces main.elf — a Position-Independent ELF32 ARM Thumb-2 binary.
# Copy main.elf + app.json to /apps/<name>/ on the SD card.
#
# Requirements:
#   arm-none-eabi-gcc (any modern version; Fedora: arm-none-eabi-gcc-cs, Debian/Ubuntu: gcc-arm-none-eabi)

CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -Os -Wall -Wextra -Wno-unused-parameter \
          -I.
LDFLAGS = -T linker.ld \
          -Wl,--entry=picodeck_main \
          -Wl,-pie \
          -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments \
          -nostartfiles -nodefaultlibs

# This sample uses no C library. To use one (malloc, snprintf, memcpy, float
# maths, 64-bit division), add stubs.c to SRCS and link with -lc -lm -lgcc after
# the sources, as docs/Native-Loading.md describes under "C library".
SRCS    = main.c
TARGET  = main.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) linker.ld app_abi.h os.h terminal.h
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	arm-none-eabi-size $@

clean:
	rm -f $(TARGET)
