# DOS/86 — fake86 8086 PC emulator for PicoDeck

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
CFLAGS  = -mcpu=cortex-m33 -mthumb -std=gnu11 \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -O2 -Wall -Wextra -Wno-unused-parameter \
          -I. -I../../sdk/native
LDFLAGS = -T linker.ld \
          -Wl,--entry=picodeck_main \
          -Wl,-pie \
          -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments \
          -nostartfiles -nodefaultlibs -lc -lgcc

# CPU core compiled separately for SRAM placement via .time_critical section
CPU_OBJ = fake86/cpu_ram.o

# All other sources (everything EXCEPT fake86/cpu.c)
SRCS    = main.c backend.c scancode.c stubs.c opl.c audio_mix.c speaker.c \
          opl2/opl2_core.c \
          fake86/i8259.c fake86/i8253.c fake86/ports.c fake86/disk.c \
          fake86/video.c fake86/render.c fake86/bios.c

TARGET  = main.elf

.PHONY: all clean

all: $(TARGET)

$(CPU_OBJ): fake86/cpu.c fake86/cpu.h fake86/config.h
	$(CC) $(CFLAGS) -fno-function-sections -c fake86/cpu.c -o fake86/cpu_tmp.o
	$(OBJCOPY) --rename-section .text=.time_critical.cpu fake86/cpu_tmp.o $(CPU_OBJ)
	rm -f fake86/cpu_tmp.o

$(TARGET): $(SRCS) $(CPU_OBJ) linker.ld ../../sdk/native/os.h ../../sdk/native/app_abi.h
	$(CC) $(CFLAGS) $(SRCS) $(CPU_OBJ) $(LDFLAGS) -o $@
	arm-none-eabi-strip $@
	arm-none-eabi-size $@

clean:
	rm -f $(TARGET) $(CPU_OBJ) fake86/cpu_tmp.o
