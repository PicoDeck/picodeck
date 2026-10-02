# PicoDeck Makefile
# Automated setup, build, and deployment for ClockworkPi PicoCalc

.PHONY: help setup build clean flash flash-ota rebuild check-env test-lua test-unit test-numfmt-sweep fuzz fuzz-build simulator simulator-asan simulator-tsan simulator-net simulator-net-asan simulator-net-tsan simulator-run simulator-clean simulator-web

# ── Configuration ─────────────────────────────────────────────────────────────

PICO_BOARD ?= pimoroni_pico_plus2_w_rp2350
BUILD_DIR := build
LUA_VERSION := 5.4.7
FATFS_VERSION := R0.15
FATFS_URL := http://elm-chan.org/fsw/ff/arc/ff15.zip

# Lua download URL
LUA_URL := https://www.lua.org/ftp/lua-$(LUA_VERSION).tar.gz

# Third-party directories
LUA_DIR := third_party/lua-5.4
FATFS_DIR := third_party/fatfs

# ── Default target ────────────────────────────────────────────────────────────

help:
	@echo "PicoDeck Build System"
	@echo ""
	@echo "Hardware Targets:"
	@echo "  make setup          - Download dependencies (Lua, FatFS) and check environment"
	@echo "  make build          - Build the firmware (creates build/picodeck.uf2)"
	@echo "  make clean          - Remove build directory"
	@echo "  make rebuild        - Clean and rebuild from scratch"
	@echo "  make flash          - Show instructions for flashing the device (BOOTSEL)"
	@echo "  make flash-ota      - Build and push firmware to a USB-connected device (OTA)"
	@echo "  make check-env      - Verify build environment is ready"
	@echo ""
	@echo "Simulator Targets (PC):"
	@echo "  make simulator      - Build PC simulator for testing/debugging"
	@echo "  make simulator-asan - Simulator with ASan+UBSan (build_sim_asan/)"
	@echo "  make simulator-tsan - Simulator with TSan (build_sim_tsan/)"
	@echo "  make simulator-net  - Simulator on the firmware network stack (build_sim_net/;"
	@echo "                        also simulator-net-asan / simulator-net-tsan)"
	@echo "  make simulator-run  - Build and run the simulator"
	@echo "  make simulator-clean - Clean simulator build files"
	@echo ""
	@echo "Testing:"
	@echo "  make test-lua       - Test Lua app syntax before deployment"
	@echo "  make test-unit      - Host unit tests (ctest, build_unit/)"
	@echo "  make fuzz           - Run each libFuzzer target FUZZ_SECONDS (clang)"
	@echo ""
	@echo "Environment:"
	@echo "  PICO_BOARD          - Target board (default: $(PICO_BOARD))"
	@echo "  PICO_SDK_PATH       - Path to Pico SDK (must be set)"
	@echo ""

# ── Setup target ──────────────────────────────────────────────────────────────

setup: check-env download-lua download-fatfs
	@echo ""
	@echo "✓ Setup complete! Run 'make build' to compile the firmware."

check-env:
	@echo "Checking build environment..."
	@if [ -z "$(PICO_SDK_PATH)" ]; then \
		echo "ERROR: PICO_SDK_PATH environment variable not set."; \
		echo ""; \
		echo "To fix this:"; \
		echo "  1. Clone the Pico SDK:"; \
		echo "     git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk"; \
		echo "     cd ~/pico-sdk && git submodule update --init"; \
		echo ""; \
		echo "  2. Set the environment variable:"; \
		echo "     export PICO_SDK_PATH=~/pico-sdk"; \
		echo "     # Or add to ~/.bashrc for persistence"; \
		echo ""; \
		exit 1; \
	fi
	@if [ ! -d "$(PICO_SDK_PATH)" ]; then \
		echo "ERROR: PICO_SDK_PATH points to non-existent directory: $(PICO_SDK_PATH)"; \
		exit 1; \
	fi
	@if [ ! -f "$(PICO_SDK_PATH)/pico_sdk_init.cmake" ]; then \
		echo "ERROR: $(PICO_SDK_PATH) doesn't look like a valid Pico SDK"; \
		echo "       (missing pico_sdk_init.cmake)"; \
		exit 1; \
	fi
	@echo "  ✓ PICO_SDK_PATH: $(PICO_SDK_PATH)"
	@# Check for ARM toolchain
	@if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then \
		echo "WARNING: arm-none-eabi-gcc not found in PATH"; \
		echo "         Install ARM toolchain:"; \
		echo ""; \
		echo "  Fedora/RHEL:  sudo dnf install gcc-arm-none-eabi newlib-arm-none-eabi"; \
		echo "  Debian/Ubuntu: sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi"; \
		echo "  macOS:        brew install --cask gcc-arm-embedded"; \
		echo ""; \
	else \
		echo "  ✓ arm-none-eabi-gcc: $$(arm-none-eabi-gcc --version | head -n1)"; \
	fi
	@# Check for CMake
	@if ! command -v cmake >/dev/null 2>&1; then \
		echo "ERROR: cmake not found in PATH"; \
		echo "       Install: sudo dnf install cmake  # or apt/brew"; \
		exit 1; \
	else \
		echo "  ✓ cmake: $$(cmake --version | head -n1)"; \
	fi

download-lua:
	@if [ -d "$(LUA_DIR)/src" ] && [ -f "$(LUA_DIR)/src/lua.h" ]; then \
		echo "  ✓ Lua $(LUA_VERSION) already present at $(LUA_DIR)"; \
	else \
		echo "Downloading Lua $(LUA_VERSION)..."; \
		mkdir -p third_party; \
		cd third_party && \
		curl -LO $(LUA_URL) && \
		tar -xzf lua-$(LUA_VERSION).tar.gz && \
		mv lua-$(LUA_VERSION) lua-5.4 && \
		rm lua-$(LUA_VERSION).tar.gz && \
		echo "  ✓ Lua $(LUA_VERSION) extracted to $(LUA_DIR)"; \
	fi
	@# luaconf.h hard-codes LUA_32BITS/LUAI_MAXSTACK; patch it to honour the
	@# CMake config (idempotent — also fixes checkouts extracted before this).
	@cmake -DLUA_SRC_DIR=$(LUA_DIR)/src -P cmake/picodeck_lua.cmake

download-fatfs:
	@if [ -d "$(FATFS_DIR)" ] && [ -f "$(FATFS_DIR)/ff.h" ]; then \
		echo "  ✓ FatFS already present at $(FATFS_DIR)"; \
	else \
		echo "Downloading FatFS $(FATFS_VERSION)..."; \
		echo "NOTE: FatFS is pre-configured in third_party/fatfs/"; \
		echo "      If missing, manually download from http://elm-chan.org/fsw/ff/"; \
		echo "      and ensure diskio_spi.c is present in $(FATFS_DIR)/port/"; \
		if [ ! -d "$(FATFS_DIR)" ]; then \
			echo "ERROR: $(FATFS_DIR) not found. See README.md for setup instructions."; \
			exit 1; \
		fi; \
	fi

# ── Build targets ─────────────────────────────────────────────────────────────

build: check-env $(LUA_DIR) $(FATFS_DIR)
	@echo "Building PicoDeck for $(PICO_BOARD)..."
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && \
		cmake .. -DPICO_BOARD=$(PICO_BOARD) && \
		$(MAKE) -j$$(nproc 2>/dev/null || echo 4)
	@test -f $(BUILD_DIR)/picodeck.uf2 || { \
		echo "ERROR: build finished but $(BUILD_DIR)/picodeck.uf2 is missing"; \
		exit 1; \
	}
	@echo ""
	@echo "✓ Build complete!"
	@echo "  Firmware: $(BUILD_DIR)/picodeck.uf2"
	@echo ""
	@echo "To flash:"
	@echo "  1. Hold BOOTSEL button on Pico"
	@echo "  2. Connect USB cable"
	@echo "  3. Drag $(BUILD_DIR)/picodeck.uf2 to mounted drive"
	@echo ""

clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@echo "✓ Clean complete"

rebuild: clean build

# ── Flash helper ──────────────────────────────────────────────────────────────

flash:
	@if [ ! -f "$(BUILD_DIR)/picodeck.uf2" ]; then \
		echo "ERROR: $(BUILD_DIR)/picodeck.uf2 not found. Run 'make build' first."; \
		exit 1; \
	fi
	@echo "Flashing instructions:"
	@echo ""
	@echo "  1. Disconnect the Pico from USB"
	@echo "  2. Hold the BOOTSEL button on the Pico"
	@echo "  3. While holding BOOTSEL, connect USB cable"
	@echo "  4. Release BOOTSEL — Pico mounts as USB drive (RPI-RP2)"
	@echo "  5. Copy the firmware:"
	@echo ""
	@echo "     cp $(BUILD_DIR)/picodeck.uf2 /path/to/RPI-RP2/"
	@echo ""
	@echo "     Or drag-and-drop in your file manager"
	@echo ""
	@echo "  6. Device will reboot automatically"
	@echo ""
	@# Attempt auto-detection (Linux only)
	@if [ -d "/media/$$USER/RPI-RP2" ]; then \
		echo "✓ Detected RPI-RP2 at /media/$$USER/RPI-RP2"; \
		echo "  Run: cp $(BUILD_DIR)/picodeck.uf2 /media/$$USER/RPI-RP2/"; \
		echo ""; \
		read -p "Copy now? [y/N] " confirm && \
		[ "$$confirm" = "y" ] && cp $(BUILD_DIR)/picodeck.uf2 /media/$$USER/RPI-RP2/ && \
		echo "✓ Firmware copied! Device will reboot."; \
	fi

# ── OTA flash ─────────────────────────────────────────────────────────────────
# Builds, then pushes build/picodeck.bin to a USB-connected device over the
# SD-staged OTA path (signs the image, uploads it with its .sha256/.sig and
# reboots; the device checks the SHA-256 and ECDSA signature and reflashes
# itself on boot). The device must be at the launcher.
# Optional: FLASH_DEVICE=/dev/ttyACM0 to skip auto-detection;
#           OTA_KEY=<private.pem> to sign with a key other than the TEST key
#           (it must match the PICODECK_UPDATE_PUBKEY_PEM the RUNNING firmware
#           was built with).

flash-ota: build
	@python3 tools/ota_flash.py $(BUILD_DIR)/picodeck.bin \
		$(if $(FLASH_DEVICE),--device $(FLASH_DEVICE),) \
		$(if $(OTA_KEY),--key $(OTA_KEY),)

# ── Lua App Testing ───────────────────────────────────────────────────────────

test-lua:
	@if ! command -v lua >/dev/null 2>&1; then \
		echo "ERROR: 'lua' interpreter not found in PATH"; \
		echo "       Install: sudo dnf install lua  # or apt/brew"; \
		exit 1; \
	fi
	@echo "Testing Lua app syntax..."
	@lua tools/test_lua_apps.lua

# ── PC Simulator Targets ─────────────────────────────────────────────────────

SIM_BUILD_DIR := build_sim
SIM_BINARY := $(SIM_BUILD_DIR)/picodeck_simulator

simulator-check:
	@echo "Checking simulator build environment..."
	@if ! command -v cmake >/dev/null 2>&1; then \
		echo "ERROR: cmake not found in PATH"; \
		echo "       Install: sudo dnf install cmake  # or apt/brew"; \
		exit 1; \
	fi
	@if ! pkg-config --exists sdl2 2>/dev/null; then \
		echo "ERROR: SDL2 development libraries not found"; \
		echo ""; \
		echo "       Install SDL2:"; \
		echo "         Fedora:  sudo dnf install SDL2-devel"; \
		echo "         Ubuntu:  sudo apt install libsdl2-dev"; \
		echo "         macOS:   brew install sdl2"; \
		echo ""; \
		exit 1; \
	fi
	@echo "  ✓ cmake: $$(cmake --version | head -n1)"
	@echo "  ✓ SDL2: $$(pkg-config --modversion sdl2)"

simulator: simulator-check download-lua
	@echo "Building PicoDeck PC Simulator..."
	@mkdir -p $(SIM_BUILD_DIR)
	@if [ -f $(SIM_BUILD_DIR)/CMakeCache.txt ] && \
		! grep -q 'CMAKE_HOME_DIRECTORY.*simulator' $(SIM_BUILD_DIR)/CMakeCache.txt 2>/dev/null; then \
		echo "  Stale CMake cache detected, cleaning..."; \
		rm -rf $(SIM_BUILD_DIR)/*; \
	fi
	@cd $(SIM_BUILD_DIR) && \
		cmake ../simulator -DCMAKE_BUILD_TYPE=Release && \
		$(MAKE) -j$$(nproc 2>/dev/null || echo 4)
	@test -x $(SIM_BINARY) || { \
		echo "ERROR: build finished but $(SIM_BINARY) is missing"; \
		exit 1; \
	}
	@echo ""
	@echo "✓ Simulator build complete!"
	@echo "  Binary: $(SIM_BINARY)"
	@echo ""
	@echo "To run:"
	@echo "  $(SIM_BINARY) --sd-card ./apps"
	@echo ""
	@echo "For help:"
	@echo "  $(SIM_BINARY) --help"
	@echo ""

simulator-debug: simulator-check download-lua
	@echo "Building PicoDeck PC Simulator (Debug)..."
	@mkdir -p $(SIM_BUILD_DIR)
	@if [ -f $(SIM_BUILD_DIR)/CMakeCache.txt ] && \
		! grep -q 'CMAKE_HOME_DIRECTORY.*simulator' $(SIM_BUILD_DIR)/CMakeCache.txt 2>/dev/null; then \
		echo "  Stale CMake cache detected, cleaning..."; \
		rm -rf $(SIM_BUILD_DIR)/*; \
	fi
	@cd $(SIM_BUILD_DIR) && \
		cmake ../simulator -DCMAKE_BUILD_TYPE=Debug && \
		$(MAKE) -j$$(nproc 2>/dev/null || echo 4)
	@test -x $(SIM_BINARY) || { \
		echo "ERROR: build finished but $(SIM_BINARY) is missing"; \
		exit 1; \
	}
	@echo ""
	@echo "✓ Simulator build complete (Debug)!"
	@echo "  Binary: $(SIM_BINARY)"
	@echo ""

# Sanitizer builds of the simulator (own build dirs, so the release build stays
# untouched). Run the E2E suite against one with
#   PICODECK_SIM_BINARY=build_sim_asan/picodeck_simulator pytest tests/e2e -n auto
# Unicorn is not instrumented; its source is reused from build_sim if present.
# Clang by default: its compiler-rt ships the ASan/UBSan/TSan runtimes, while
# GCC's (libasan/libubsan/libtsan) are separate packages that are often absent.
SIM_SAN_CC ?= clang
SIM_SAN_CXX ?= clang++
SIM_UNICORN_SRC := $(CURDIR)/$(SIM_BUILD_DIR)/_deps/unicorn-src
define sim_sanitize_build
	@echo "Building PicoDeck PC Simulator ($(2) $(3))..."
	@mkdir -p $(1)
	@cd $(1) && \
		cmake ../simulator -DCMAKE_BUILD_TYPE=RelWithDebInfo \
			-DCMAKE_C_COMPILER=$(SIM_SAN_CC) -DCMAKE_CXX_COMPILER=$(SIM_SAN_CXX) \
			-DPICODECK_SIM_SANITIZE="$(2)" $(3) \
			$$( [ -d $(SIM_UNICORN_SRC) ] && echo -DFETCHCONTENT_SOURCE_DIR_UNICORN=$(SIM_UNICORN_SRC) ) && \
		$(MAKE) -j$$(nproc 2>/dev/null || echo 4)
	@test -x $(1)/picodeck_simulator || { \
		echo "ERROR: build finished but $(1)/picodeck_simulator is missing"; \
		exit 1; \
	}
	@echo "✓ $(1)/picodeck_simulator ($(2))"
endef

simulator-asan: simulator-check download-lua
	$(call sim_sanitize_build,build_sim_asan,address;undefined)

simulator-tsan: simulator-check download-lua
	$(call sim_sanitize_build,build_sim_tsan,thread)

# The firmware network stack in the simulator (SIM_FIRMWARE_NET: the real
# src/drivers/wifi.c, http.c, tcp.c on Mongoose/POSIX, see simulator/net/).
# Own build dirs; only tests/e2e/test_network_firmware.py needs them:
#   PICODECK_SIM_BINARY=build_sim_net/picodeck_simulator \
#       pytest tests/e2e/test_network_firmware.py -n auto
simulator-net: simulator-check download-lua
	@echo "Building PicoDeck PC Simulator (firmware network stack)..."
	@mkdir -p build_sim_net
	@cd build_sim_net && \
		cmake ../simulator -DCMAKE_BUILD_TYPE=Release -DSIM_FIRMWARE_NET=ON \
			$$( [ -d $(SIM_UNICORN_SRC) ] && echo -DFETCHCONTENT_SOURCE_DIR_UNICORN=$(SIM_UNICORN_SRC) ) && \
		$(MAKE) -j$$(nproc 2>/dev/null || echo 4)
	@test -x build_sim_net/picodeck_simulator || { \
		echo "ERROR: build finished but build_sim_net/picodeck_simulator is missing"; \
		exit 1; \
	}
	@echo "✓ build_sim_net/picodeck_simulator (firmware network stack)"

simulator-net-asan: simulator-check download-lua
	$(call sim_sanitize_build,build_sim_net_asan,address;undefined,-DSIM_FIRMWARE_NET=ON)

simulator-net-tsan: simulator-check download-lua
	$(call sim_sanitize_build,build_sim_net_tsan,thread,-DSIM_FIRMWARE_NET=ON)

simulator-run: simulator
	@echo "Running PicoDeck Simulator..."
	@echo ""
	@$(SIM_BINARY) --sd-card ./apps

simulator-clean:
	@echo "Cleaning simulator build..."
	@rm -rf $(SIM_BUILD_DIR) build_sim_asan build_sim_tsan \
		build_sim_net build_sim_net_asan build_sim_net_tsan
	@echo "✓ Simulator clean complete"

simulator-web:
	@echo "The browser build moved to PicoDeck/web-sim (https://github.com/PicoDeck/web-sim). Clone it next to this checkout and run: make -C ../web-sim build test"
	@exit 1

# ── Host unit tests ──────────────────────────────────────────────────────────

# One CMake/ctest project (tests/unit/CMakeLists.txt): one executable per
# module, ASan+UBSan when the compiler has them. Clang when available (its
# compiler-rt carries the sanitizer and libFuzzer runtimes).
UNIT_BUILD_DIR := build_unit
UNIT_CC ?= $(shell command -v clang 2>/dev/null || echo cc)

test-unit: download-lua download-fatfs
	@cmake -S tests/unit -B $(UNIT_BUILD_DIR) -DCMAKE_C_COMPILER=$(UNIT_CC) \
		-DCMAKE_BUILD_TYPE=Debug >/dev/null
	@cmake --build $(UNIT_BUILD_DIR) -j$$(nproc 2>/dev/null || echo 4)
	@ctest --test-dir $(UNIT_BUILD_DIR) --output-on-failure

# Exhaustive Lua float-format check vs glibc (~35 s), opt-in.
test-numfmt-sweep:
	@cmake -S tests/unit -B $(UNIT_BUILD_DIR) -DCMAKE_C_COMPILER=$(UNIT_CC) \
		-DCMAKE_BUILD_TYPE=Debug >/dev/null
	@cmake --build $(UNIT_BUILD_DIR) --target numfmt_sweep
	@./$(UNIT_BUILD_DIR)/numfmt_sweep

# libFuzzer targets (tests/fuzz): clang only. Each runs FUZZ_SECONDS on a
# working corpus in build_fuzz/corpus/<name>, seeded from tests/fuzz/corpus.
# Crashes land in build_fuzz/crash-fuzz_<name>-* and fail the target.
#   make fuzz FUZZ_TARGETS=wav FUZZ_SECONDS=120
FUZZ_BUILD_DIR := build_fuzz
FUZZ_SECONDS ?= 60
FUZZ_TARGETS ?= elf_plan app_manifest wav qoa hid_pad

fuzz-build:
	@cmake -S tests/unit -B $(FUZZ_BUILD_DIR) -DCMAKE_C_COMPILER=clang \
		-DCMAKE_BUILD_TYPE=Debug -DPICODECK_FUZZ=ON >/dev/null
	@cmake --build $(FUZZ_BUILD_DIR) -j$$(nproc 2>/dev/null || echo 4) \
		$(foreach t,$(FUZZ_TARGETS),--target fuzz_$(t))
	@for t in $(FUZZ_TARGETS); do test -x $(FUZZ_BUILD_DIR)/fuzz/fuzz_$$t || { \
		echo "ERROR: $(FUZZ_BUILD_DIR)/fuzz/fuzz_$$t not built (clang with libFuzzer needed)"; \
		exit 1; }; done

fuzz: fuzz-build
	@set -e; for name in $(FUZZ_TARGETS); do \
		mkdir -p $(FUZZ_BUILD_DIR)/corpus/$$name; \
		echo "== fuzz_$$name ($(FUZZ_SECONDS)s)"; \
		$(FUZZ_BUILD_DIR)/fuzz/fuzz_$$name -max_total_time=$(FUZZ_SECONDS) \
			-print_final_stats=1 \
			-artifact_prefix=$(FUZZ_BUILD_DIR)/crash-fuzz_$$name- \
			$(FUZZ_BUILD_DIR)/corpus/$$name tests/fuzz/corpus/$$name; \
	done
