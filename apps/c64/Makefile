# PicoDeck C64 Emulator Native App Build
#
# Produces main.elf — a Position-Independent ELF32 ARM Thumb-2 binary.
# Copy main.elf + app.json to /apps/c64/ on the SD card.
#
# Requirements:
#   arm-none-eabi-gcc (any modern version; Fedora: gcc-arm-none-eabi-cs)

CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -std=gnu99 \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -O3 -flto -Wall -Wextra -Wno-unused-parameter \
          -I. \
          -I../../sdk/native \
          -Ichips
# stubs.c is compiled without LTO to avoid ARM/Thumb interworking issues
CFLAGS_NOLTO = -mcpu=cortex-m33 -mthumb -std=gnu99 \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -O2 -Wall -Wextra -Wno-unused-parameter \
          -I. -I../../sdk/native
LDFLAGS = -T linker.ld \
          -Wl,--entry=picodeck_main \
          -Wl,-pie \
          -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments \
          -nostartfiles -nodefaultlibs -lc -lm -lgcc

TARGET  = main.elf

.PHONY: all clean

all: $(TARGET)

stubs.o: stubs.c
	$(CC) $(CFLAGS_NOLTO) -c $< -o $@

$(TARGET): main.c pad_input.h stubs.o linker.ld ../../sdk/native/os.h ../../sdk/native/app_abi.h
	$(CC) $(CFLAGS) main.c stubs.o $(LDFLAGS) -o $@
	arm-none-eabi-size $@

clean:
	rm -f $(TARGET) stubs.o
