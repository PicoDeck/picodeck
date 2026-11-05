# perf_rig: estimate the time of firmware-compiled C on the device's XIP cache,
# without the device. See README.md.
#
#   make run                                 this tree's gfx3d.c, the firmware's layout
#   make run GFX3D_SRC=/path/to/worktree     another tree's src/os/gfx3d.c and display_clip.h
#   make compare GFX3D_SRC=/path/to/worktree this tree's, then that one's
#   make run MAP=text-sram                   the same code moved (see "Maps" below)
#   make run RIG_ARGS="--policy random"      any option of the host tool (make help)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
HERE := $(ROOT)/tools/perf_rig
GFX3D_SRC ?= $(ROOT)
NOVA_RAIL ?= $(HOME)/Projects/PicoDeck/nova-rail
FRAMES ?= 60
LAP_S ?= 71
MAP ?= flash
RIG_ARGS ?=
# Build directory: one per source tree and map, so `compare` keeps both.
TAG := $(shell printf '%s' '$(abspath $(GFX3D_SRC))' | cksum | cut -d' ' -f1)
O ?= $(HERE)/build/$(TAG)-$(MAP)

# ── Unicorn: the simulator's static build, here or in the main checkout ──────
MAIN_CHECKOUT := $(abspath $(dir $(shell git -C $(ROOT) rev-parse --path-format=absolute --git-common-dir)))
UNICORN_BUILD ?= $(firstword $(wildcard $(ROOT)/build_sim/_deps/unicorn-build/libarm-softmmu.a $(MAIN_CHECKOUT)/build_sim/_deps/unicorn-build/libarm-softmmu.a))
UB := $(patsubst %/_deps/unicorn-build/libarm-softmmu.a,%,$(UNICORN_BUILD))
HOSTCC ?= cc
HOST_CFLAGS := -O2 -g -Wall -Wextra -I$(UB)/_deps/unicorn-src/include
HOST_LIBS := -Wl,--start-group $(UB)/_deps/unicorn-build/libunicorn-static.a \
	$(UB)/_deps/unicorn-build/libunicorn-common.a $(UB)/_deps/unicorn-build/libarm-softmmu.a \
	-Wl,--end-group -lpthread -lm

# ── Guest: the firmware's compiler and flags (build/.../flags.make, gfx3d.c's
# per-file options from CMakeLists.txt) ──────────────────────────────────────
CC := arm-none-eabi-gcc
ARCH := -mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse
GFX3D_FLAGS := -g -DNDEBUG -std=gnu11 -Os -ffunction-sections -fdata-sections -O2 -ffp-contract=off
GUEST_INC := -I$(HERE)/guest -I$(HERE)/guest/stubs -I$(GFX3D_SRC)/src/os -I$(GFX3D_SRC)/src/drivers

# Maps: where each thing lives. Defaults are the firmware's: gfx3d.c's text at
# its address in build/picodeck.elf, memset/memcpy near theirs, sqrtf in SRAM.
TEXT_BASE ?= 0x10015a58
RAMTEXT_BASE ?= 0x20003090
LIB_BASE ?= 0x1009de00
RODATA_BASE ?= 0x10122000
DATA_BASE ?= 0x20002000
STACK_TOP ?= 0x11210000
HEAP_BASE ?= 0x11210000
FB_BASE ?= 0x20010000
ifeq ($(MAP),text-sram)        # gfx3d's code in SRAM (not room on the device: an upper bound)
TEXT_BASE := 0x20004000
endif
ifeq ($(MAP),stack-sram)       # the stack in SRAM (the 4 KB-ish free heap could not hold 64 KB)
STACK_TOP := 0x20080000
endif
ifeq ($(MAP),scratch-sram)     # stack and every gfx3d allocation in SRAM: no PSRAM data at all
STACK_TOP := 0x20080000
HEAP_BASE := 0x20080000
endif
LDFLAGS := -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-T,$(HERE)/guest/guest.ld \
	-Wl,--defsym=TEXT_BASE=$(TEXT_BASE) -Wl,--defsym=RAMTEXT_BASE=$(RAMTEXT_BASE) \
	-Wl,--defsym=LIB_BASE=$(LIB_BASE) -Wl,--defsym=RODATA_BASE=$(RODATA_BASE) \
	-Wl,--defsym=DATA_BASE=$(DATA_BASE) -Wl,--defsym=STACK_TOP=$(STACK_TOP) \
	-Wl,--defsym=HEAP_BASE=$(HEAP_BASE) -Wl,--defsym=FB_BASE=$(FB_BASE) \
	-Wl,-Map=$(O)/guest.map -Wl,--no-warn-rwx-segments

RIG := $(HERE)/build/perf_rig
SCENE := $(HERE)/build/scene-$(FRAMES)-$(LAP_S).bin
GUEST := $(O)/gfx3d.elf

.PHONY: all run sweep compare check help clean
all: $(RIG) $(GUEST)

# Checks the emulator: the self-test of VFP, DSP, hook counts and the cache model,
# and every frame rendered natively on the host against the same frames emulated.
check: all $(HERE)/build/selftest.elf $(O)/native
	@$(RIG) $(HERE)/build/selftest.elf --selftest
	@a=$$($(O)/native); b=$$($(RIG) $(GUEST) | sed -n 's/.*buffer //p'); echo "pixels: native $$a, emulated $$b"; test "$$a" = "$$b"

$(HERE)/build/selftest.elf: $(HERE)/guest/selftest.c $(HERE)/guest/rig.h $(HERE)/guest/selftest.ld
	$(CC) $(ARCH) -O1 -g -I$(HERE)/guest -nostdlib -Wl,-T,$(HERE)/guest/selftest.ld -Wl,--no-warn-rwx-segments -o $@ $< -lgcc

$(O)/native: $(HERE)/guest/native.c $(HERE)/guest/gfx3d_main.c $(GFX3D_SRC)/src/os/gfx3d.c $(GFX3D_SRC)/src/drivers/display_clip.h $(SCENE)
	@mkdir -p $(O)
	$(HOSTCC) -O2 -std=gnu11 -ffp-contract=off -DRIG_NATIVE -DSCENE_BIN=\"$(SCENE)\" $(GUEST_INC) -include rig_alloc.h -DGFX3D_MALLOC=rig_malloc -DGFX3D_FREE=rig_free -o $@ $(HERE)/guest/native.c $(HERE)/guest/gfx3d_main.c $(GFX3D_SRC)/src/os/gfx3d.c -lm

run: all
	@$(RIG) $(GUEST) --label "gfx3d.c from $(GFX3D_SRC), map $(MAP)" $(RIG_ARGS)

sweep: all
	@bash $(HERE)/sweep.sh $(RIG) $(GUEST)

compare:
	@$(MAKE) --no-print-directory run GFX3D_SRC=$(ROOT)
	@$(MAKE) --no-print-directory run GFX3D_SRC=$(GFX3D_SRC)

help: $(RIG)
	@$(RIG) 2>&1 || true

$(RIG): $(HERE)/rig.c
	@test -n "$(UNICORN_BUILD)" || { echo "perf_rig: no Unicorn build: run 'make simulator' or set UNICORN_BUILD" >&2; exit 2; }
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $< $(HOST_LIBS)

$(SCENE): $(HERE)/scene/dump_scene.lua
	@mkdir -p $(dir $@)
	cd $(NOVA_RAIL) && lua $(HERE)/scene/dump_scene.lua . $@ $(FRAMES) $(LAP_S)

# gfx3d.c is its own translation unit, as in the firmware: nothing inlines across it.
$(O)/gfx3d.o: $(GFX3D_SRC)/src/os/gfx3d.c $(GFX3D_SRC)/src/os/gfx3d.h $(GFX3D_SRC)/src/drivers/display_clip.h $(HERE)/guest/rig_alloc.h
	@mkdir -p $(O)
	$(CC) $(ARCH) $(GFX3D_FLAGS) $(GUEST_INC) -include rig_alloc.h -DGFX3D_MALLOC=rig_malloc -DGFX3D_FREE=rig_free -c -o $@ $<

$(O)/gfx3d_main.o: $(HERE)/guest/gfx3d_main.c $(HERE)/guest/rig.h $(HERE)/guest/rig_alloc.h $(GFX3D_SRC)/src/os/gfx3d.h
	@mkdir -p $(O)
	$(CC) $(ARCH) $(GFX3D_FLAGS) $(GUEST_INC) -c -o $@ $<

$(O)/start.o: $(HERE)/guest/start.S $(SCENE)
	@mkdir -p $(O)
	$(CC) $(ARCH) -DSCENE_BIN=\"$(SCENE)\" -c -o $@ $<

$(GUEST): $(O)/gfx3d.o $(O)/gfx3d_main.o $(O)/start.o $(HERE)/guest/guest.ld
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(O)/gfx3d.o $(O)/gfx3d_main.o $(O)/start.o -lm -lc -lgcc

clean:
	rm -rf $(HERE)/build
