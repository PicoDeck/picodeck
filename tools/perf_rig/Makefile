# perf_rig: estimate the time of firmware-compiled C on the device's XIP cache,
# without the device. See README.md.
#
#   make run                                 this tree's gfx3d.c, the firmware's layout
#   make run GFX3D_SRC=/path/to/worktree     another tree's src/os/gfx3d.c and display_clip.h
#   make compare GFX3D_SRC=/path/to/worktree this tree's, then that one's
#   make run MAP=text-sram                   the same code moved (see "Maps" below)
#   make run RIG_ARGS="--policy random"      any option of the host tool (make help)
#   make run-lua                             the real Lua core, an empty for-loop
#   make run-tex                             gfx3d's textured rasteriser, a frame of ground
#   make run-tmesh                           gfx3d's whole textured pipeline, a frame of a valley
#   make check-tex                           its M33 pixels against this host's
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
HERE := $(ROOT)/tools/perf_rig
GFX3D_SRC ?= $(ROOT)
NOVA_RAIL ?= $(HOME)/Projects/PicoDeck/nova-rail
FRAMES ?= 60
LAP_S ?= 71
MAP ?= flash
# The lent SRAM's sizes, as the firmware's: LENT="-DLENT_TIER=0" keeps that one in PSRAM.
LENT ?=
GUEST_API = $(shell grep -q gfx3d_xform $(GFX3D_SRC)/src/os/gfx3d.h && echo -DRIG_HAS_XFORM)
RIG_ARGS ?=

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
# Build directory: one per source tree, scene and placement, so `compare` keeps
# both trees and changing FRAMES or a base can never run a stale guest.
TAG := $(shell printf '%s' '$(LENT) $(abspath $(GFX3D_SRC)) $(TEXT_BASE) $(RAMTEXT_BASE) $(LIB_BASE) $(RODATA_BASE) $(DATA_BASE) $(STACK_TOP) $(HEAP_BASE) $(FB_BASE)' | cksum | cut -d' ' -f1)
O ?= $(HERE)/build/$(TAG)-$(MAP)-$(FRAMES)-$(LAP_S)
LDFLAGS := -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-T,$(HERE)/guest/guest.ld \
	-Wl,--defsym=TEXT_BASE=$(TEXT_BASE) -Wl,--defsym=RAMTEXT_BASE=$(RAMTEXT_BASE) \
	-Wl,--defsym=LIB_BASE=$(LIB_BASE) -Wl,--defsym=RODATA_BASE=$(RODATA_BASE) \
	-Wl,--defsym=DATA_BASE=$(DATA_BASE) -Wl,--defsym=STACK_TOP=$(STACK_TOP) \
	-Wl,--defsym=HEAP_BASE=$(HEAP_BASE) -Wl,--defsym=FB_BASE=$(FB_BASE) \
	-Wl,-Map=$(O)/guest.map -Wl,--no-warn-rwx-segments

RIG := $(HERE)/build/perf_rig
SCENE := $(HERE)/build/scene-$(FRAMES)-$(LAP_S).bin
GUEST := $(O)/gfx3d.elf

.PHONY: all run run-lua run-tex run-tmesh check-tex sweep compare check help clean
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
	$(HOSTCC) -O2 -std=gnu11 -ffp-contract=off -DRIG_NATIVE $(LENT) $(GUEST_API) -DSCENE_BIN=\"$(SCENE)\" $(GUEST_INC) -include rig_alloc.h -DGFX3D_MALLOC=rig_malloc -DGFX3D_FREE=rig_free -o $@ $(HERE)/guest/native.c $(HERE)/guest/gfx3d_main.c $(GFX3D_SRC)/src/os/gfx3d.c -lm

run: all
	@$(RIG) $(GUEST) --label "gfx3d.c from $(GFX3D_SRC), map $(MAP)" $(RIG_ARGS)

sweep: all
	@bash $(HERE)/sweep.sh $(RIG) $(GUEST)

# The real Lua core, the device's defines and -O2 (cmake/picodeck_lua.cmake,
# CMakeLists.txt), and one empty for-loop.
LUA_SRC ?= $(ROOT)/third_party/lua-5.4/src
LUA_DEFS := -DLUA_32BITS=1 -DLUA_USE_LONGJMP=1 -DLUAI_MAXSTACK=1000 -DLUAI_MAXCCALLS=60 -DLUA_IDSIZE=60 -DPICODECK_LUA_SPRINTF=1
LUA_ITERS ?= 1000000
LUAV_BASE ?= 0x100a8ef8
LUA_CORE := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib
LUA_OBJS := $(LUA_CORE:%=$(HERE)/build/lua/%.o)

run-lua: $(RIG) $(HERE)/build/lua/lua.elf
	@$(RIG) $(HERE)/build/lua/lua.elf --label "Lua core, empty for-loop" --per $(LUA_ITERS) $(RIG_ARGS)

$(HERE)/build/lua/%.o: $(LUA_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -g -DNDEBUG -std=gnu11 -Os -ffunction-sections -fdata-sections -O2 $(LUA_DEFS) -I$(LUA_SRC) -c -o $@ $<

$(HERE)/build/lua/lua_main.o: $(HERE)/guest/lua_main.c $(HERE)/guest/rig.h
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -g -DNDEBUG -std=gnu11 -O2 $(LUA_DEFS) -DRIG_LUA_ITERS=\"$(LUA_ITERS)\" -I$(LUA_SRC) -I$(HERE)/guest -c -o $@ $<

$(HERE)/build/lua/start.o: $(HERE)/guest/start.S $(SCENE)
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -DSCENE_BIN=\"$(SCENE)\" -c -o $@ $<

$(HERE)/build/lua/lua.elf: $(LUA_OBJS) $(HERE)/build/lua/lua_main.o $(HERE)/build/lua/start.o $(HERE)/guest/lua.ld
	$(CC) $(ARCH) -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-T,$(HERE)/guest/lua.ld \
		-Wl,--defsym=LUAV_BASE=$(LUAV_BASE) -Wl,--defsym=DATA_BASE=$(DATA_BASE) \
		-Wl,--defsym=STACK_TOP=$(STACK_TOP) -Wl,--defsym=HEAP_BASE=$(HEAP_BASE) \
		-Wl,--no-warn-rwx-segments -Wl,--no-warnings -o $@ $(LUA_OBJS) $(HERE)/build/lua/lua_main.o $(HERE)/build/lua/start.o -lm -lc -lgcc

compare:
	@$(MAKE) --no-print-directory run GFX3D_SRC=$(ROOT)
	@$(MAKE) --no-print-directory run GFX3D_SRC=$(GFX3D_SRC)

# The textured rasteriser through gfx3d.c's GFX3D_TEST entry: no scene from a game.
TEX_DIR = $(O)/tex
TEX_FLAGS = -DGFX3D_TEST -DPICO_ON_DEVICE=1 -I$(GFX3D_SRC)/tests/unit
TEX_DEPS = $(GFX3D_SRC)/src/os/gfx3d.c $(GFX3D_SRC)/src/os/gfx3d.h $(GFX3D_SRC)/src/os/gfx3d_tex.h \
	$(GFX3D_SRC)/src/drivers/display_clip.h $(GFX3D_SRC)/tests/unit/gfx3d_tex_scene.h \
	$(HERE)/guest/gfx3d_tex_main.c $(HERE)/guest/rig.h $(HERE)/guest/rig_alloc.h

$(TEX_DIR)/empty.bin:
	@mkdir -p $(TEX_DIR)/terrain $(TEX_DIR)/scene
	@: > $@

$(TEX_DIR)/start.o: $(HERE)/guest/start.S $(TEX_DIR)/empty.bin
	$(CC) $(ARCH) -DSCENE_BIN=\"$(TEX_DIR)/empty.bin\" -c -o $@ $<

# Named as the link script wants: gfx3d.o's text first.
$(TEX_DIR)/%/gfx3d.o: $(TEX_DEPS) $(TEX_DIR)/empty.bin
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) $(GFX3D_FLAGS) $(GUEST_INC) $(TEX_FLAGS) -include rig_alloc.h -DGFX3D_MALLOC=rig_malloc -DGFX3D_FREE=rig_free -c -o $@ $<

$(TEX_DIR)/terrain/gfx3d_main.o: $(TEX_DEPS) $(TEX_DIR)/empty.bin
	$(CC) $(ARCH) $(GFX3D_FLAGS) $(GUEST_INC) $(TEX_FLAGS) -c -o $@ $(HERE)/guest/gfx3d_tex_main.c

$(TEX_DIR)/scene/gfx3d_main.o: $(TEX_DEPS) $(TEX_DIR)/empty.bin
	$(CC) $(ARCH) $(GFX3D_FLAGS) $(GUEST_INC) $(TEX_FLAGS) -DTEX_WORK_SCENE -c -o $@ $(HERE)/guest/gfx3d_tex_main.c

$(TEX_DIR)/%.elf: $(TEX_DIR)/%/gfx3d.o $(TEX_DIR)/%/gfx3d_main.o $(TEX_DIR)/start.o $(HERE)/guest/guest.ld
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(TEX_DIR)/$*/gfx3d.o $(TEX_DIR)/$*/gfx3d_main.o $(TEX_DIR)/start.o -lm -lc -lgcc

run-tex: $(RIG) $(TEX_DIR)/terrain.elf
	@$(RIG) $(TEX_DIR)/terrain.elf --label "textured ground, gfx3d.c from $(GFX3D_SRC), map $(MAP)" $(RIG_ARGS)

$(TEX_DIR)/pipeline/gfx3d_main.o: $(TEX_DEPS) $(HERE)/guest/gfx3d_tmesh_main.c $(TEX_DIR)/empty.bin
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) $(GFX3D_FLAGS) $(GUEST_INC) $(TEX_FLAGS) $(LENT) -c -o $@ $(HERE)/guest/gfx3d_tmesh_main.c

run-tmesh: $(RIG) $(TEX_DIR)/pipeline.elf
	@$(RIG) $(TEX_DIR)/pipeline.elf --label "textured valley, gfx3d.c from $(GFX3D_SRC), map $(MAP)" $(RIG_ARGS)

$(TEX_DIR)/scene/native: $(TEX_DEPS) $(HERE)/guest/native.c $(TEX_DIR)/empty.bin
	$(HOSTCC) -O2 -std=gnu11 -ffp-contract=off -DRIG_NATIVE -DTEX_WORK_SCENE $(TEX_FLAGS) -DSCENE_BIN=\"$(TEX_DIR)/empty.bin\" $(GUEST_INC) -include rig_alloc.h -DGFX3D_MALLOC=rig_malloc -DGFX3D_FREE=rig_free -o $@ $(HERE)/guest/native.c $(HERE)/guest/gfx3d_tex_main.c $(GFX3D_SRC)/src/os/gfx3d.c -lm

# The same triangles on the M33's code and on this host's: the same bytes, both orders.
check-tex: $(RIG) $(TEX_DIR)/scene.elf $(TEX_DIR)/scene/native
	@a=$$($(TEX_DIR)/scene/native); b=$$($(RIG) $(TEX_DIR)/scene.elf | sed -n 's/.*buffer //p'); echo "textured pixels: native $$a, emulated $$b"; test "$$a" = "$$b"

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
$(O)/gfx3d.o: $(GFX3D_SRC)/src/os/gfx3d.c $(GFX3D_SRC)/src/os/gfx3d.h $(wildcard $(GFX3D_SRC)/src/os/gfx3d_tex.h) $(GFX3D_SRC)/src/drivers/display_clip.h $(HERE)/guest/rig_alloc.h
	@mkdir -p $(O)
	$(CC) $(ARCH) $(GFX3D_FLAGS) $(GUEST_INC) -include rig_alloc.h -DGFX3D_MALLOC=rig_malloc -DGFX3D_FREE=rig_free -c -o $@ $<

$(O)/gfx3d_main.o: $(HERE)/guest/gfx3d_main.c $(HERE)/guest/rig.h $(HERE)/guest/rig_alloc.h $(GFX3D_SRC)/src/os/gfx3d.h
	@mkdir -p $(O)
	$(CC) $(LENT) $(GUEST_API) $(ARCH) $(GFX3D_FLAGS) $(GUEST_INC) -c -o $@ $<

$(O)/start.o: $(HERE)/guest/start.S $(SCENE)
	@mkdir -p $(O)
	$(CC) $(ARCH) -DSCENE_BIN=\"$(SCENE)\" -c -o $@ $<

$(GUEST): $(O)/gfx3d.o $(O)/gfx3d_main.o $(O)/start.o $(HERE)/guest/guest.ld
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(O)/gfx3d.o $(O)/gfx3d_main.o $(O)/start.o -lm -lc -lgcc

clean:
	rm -rf $(HERE)/build
