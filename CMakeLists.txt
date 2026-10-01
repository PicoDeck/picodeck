cmake_minimum_required(VERSION 3.13)

# ── Target board ──────────────────────────────────────────────────────────────
# Change to "pico2" for standard Pico 2, "pico_w" for Pico W (original),
# or "pico" for original Pico (note: very little RAM for Lua + framebuffer)
set(PICO_BOARD pimoroni_pico_plus2_w_rp2350 CACHE STRING "Target board")

# ── Custom mbedtls config (must be set before pico_sdk_init) ──────────────────
# Routes mbedtls allocations to PSRAM via umm_malloc instead of SRAM
set(PICO_MBEDTLS_CONFIG_FILE "${CMAKE_SOURCE_DIR}/src/mbedtls_config.h" CACHE STRING "Custom mbedtls config" FORCE)

# Pull in Pico SDK (must be done before project())
include(pico_sdk_import.cmake)

project(picodeck C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ── Version from git tags ────────────────────────────────────────────────────
execute_process(
    COMMAND git describe --tags --always
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
# Strip leading 'v' from tag (v0.0.7 → 0.0.7)
if(GIT_VERSION)
    string(REGEX REPLACE "^v" "" PICODECK_VERSION "${GIT_VERSION}")
else()
    set(PICODECK_VERSION "0.0.0-unknown")
endif()
message(STATUS "PicoDeck version: ${PICODECK_VERSION}")

# ── OTA update signing key (public half, embedded in flash) ──────────────────
# The firmware flashes /system/update.bin only if /system/update.sig verifies
# against this P-256 key (src/os/ota_verify.c).  The default is the TEST key
# in tests/keys, whose private half is in the repo: fine for local builds and
# `make flash-ota` (tools/ota_flash.py signs with it), never for a release.
# Release builds pass the real key and PICODECK_REQUIRE_RELEASE_UPDATE_KEY=ON.
include(cmake/picodeck_update_key.cmake)
set(PICODECK_UPDATE_PUBKEY_PEM "${PICODECK_TEST_UPDATE_PUBKEY}" CACHE FILEPATH
    "P-256 public key (PEM) that OTA images must be signed with")
option(PICODECK_REQUIRE_RELEASE_UPDATE_KEY
       "Refuse to build with the TEST update key (release builds)" OFF)
file(READ "${PICODECK_UPDATE_PUBKEY_PEM}" _picodeck_key_text)
file(READ "${PICODECK_TEST_UPDATE_PUBKEY}" _picodeck_test_key_text)
if(_picodeck_key_text STREQUAL _picodeck_test_key_text)
    if(PICODECK_REQUIRE_RELEASE_UPDATE_KEY)
        message(FATAL_ERROR "PICODECK_REQUIRE_RELEASE_UPDATE_KEY is ON but "
            "PICODECK_UPDATE_PUBKEY_PEM is the TEST key (${PICODECK_UPDATE_PUBKEY_PEM})")
    endif()
    message(STATUS "OTA update key: TEST key (dev build — its private key is public)")
else()
    message(STATUS "OTA update key: ${PICODECK_UPDATE_PUBKEY_PEM}")
endif()
picodeck_generate_update_pubkey(${CMAKE_BINARY_DIR}/generated/ota_pubkey.c
                             ${PICODECK_UPDATE_PUBKEY_PEM})

# ── Lua 5.4 ──────────────────────────────────────────────────────────────────
# Lua is added as a git submodule or downloaded into third_party/lua-5.4/
# See README.md for setup instructions.

set(LUA_DIR ${CMAKE_SOURCE_DIR}/third_party/lua-5.4/src)

set(LUA_SOURCES
    ${LUA_DIR}/lapi.c
    ${LUA_DIR}/lcode.c
    ${LUA_DIR}/lctype.c
    ${LUA_DIR}/ldebug.c
    ${LUA_DIR}/ldo.c
    ${LUA_DIR}/ldump.c
    ${LUA_DIR}/lfunc.c
    ${LUA_DIR}/lgc.c
    ${LUA_DIR}/linit.c
    ${LUA_DIR}/llex.c
    ${LUA_DIR}/lmem.c
    ${LUA_DIR}/lobject.c
    ${LUA_DIR}/lopcodes.c
    ${LUA_DIR}/lparser.c
    ${LUA_DIR}/lstate.c
    ${LUA_DIR}/lstring.c
    ${LUA_DIR}/ltable.c
    ${LUA_DIR}/ltm.c
    ${LUA_DIR}/lundump.c
    ${LUA_DIR}/lvm.c
    ${LUA_DIR}/lzio.c
    # Standard libraries (selective — skip io/os/package for sandboxing)
    ${LUA_DIR}/lauxlib.c
    ${LUA_DIR}/lbaselib.c
    ${LUA_DIR}/lmathlib.c
    ${LUA_DIR}/lstrlib.c
    ${LUA_DIR}/ltablib.c
    ${LUA_DIR}/lutf8lib.c
    # Include corolib if you want coroutine support in apps
    ${LUA_DIR}/lcorolib.c
)

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${LUA_DIR})

# Lua config (shared with the simulator). luaconf.h is patched so these
# -D values actually take effect; PUBLIC so the bridge sees the same types.
include(${CMAKE_SOURCE_DIR}/cmake/picodeck_lua.cmake)
picodeck_patch_luaconf(${LUA_DIR})
target_compile_definitions(lua PUBLIC ${PICODECK_LUA_DEFINITIONS})
target_sources(lua PRIVATE ${PICODECK_LUA_SOURCES})  # picodeck_lua_sprintf (float formatting)
# -O2, not -Os: hook-free, the VM gains ~10% on bytecode and 20-45% on C-call
# paths at -O2 for +23 KB of flash (P0, specs/2026-09-27-p0-lua-perf-findings.md).
target_compile_options(lua PRIVATE -O2)

# ── FatFS ─────────────────────────────────────────────────────────────────────
# Option A: Use pico-extras (recommended):
#   Add to your ~/.gitconfig or clone pico-extras and add to PICO_SDK_PATH
#   See: https://github.com/raspberrypi/pico-extras
#
# Option B: Vendor FatFS directly (what this CMake assumes by default):
#   Download FatFS from http://elm-chan.org/fsw/ff/
#   Place source in third_party/fatfs/
#   Provide your own diskio.c for SPI-based SD (see README for a template)

set(FATFS_DIR ${CMAKE_SOURCE_DIR}/third_party/fatfs)

add_library(fatfs STATIC
    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffsystem.c
    ${FATFS_DIR}/ffunicode.c
    ${FATFS_DIR}/port/diskio_spi.c    # our SPI port layer
)
# diskio_spi.c uses hardware.h and Pico SDK SPI — expose both include paths
target_include_directories(fatfs PUBLIC
    ${FATFS_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fatfs pico_stdlib hardware_spi hardware_gpio)

# ── Image Decoders ────────────────────────────────────────────────────────────
set(TGX_DIR ${CMAKE_SOURCE_DIR}/third_party/tgx)
set(JPEGDEC_DIR ${CMAKE_SOURCE_DIR}/third_party/JPEGDEC)
set(PNGDEC_DIR ${CMAKE_SOURCE_DIR}/third_party/PNGdec)
set(GIF_DIR ${CMAKE_SOURCE_DIR}/third_party/AnimatedGIF)

# PNGdec's zlib with its unaligned word copies made alignment-safe (patched
# copies in the build tree; the submodule stays pristine). See the module.
include(${CMAKE_SOURCE_DIR}/cmake/picodeck_pngdec.cmake)
picodeck_pngdec_sources(PICODECK_PNGDEC_SOURCES ${PNGDEC_DIR}/src)

# JPEGDEC as separate lib with -O2 for faster decode loops (IDCT, Huffman VLC,
# pixel conversion).  Code grows from ~17KB (-Os) to ~24KB (-O2), but it already
# exceeded the 16KB XIP cache at -Os, so the extra size doesn't meaningfully
# worsen cache behavior while each cache-hit instruction runs faster.
add_library(jpegdec_lib STATIC ${JPEGDEC_DIR}/src/JPEGDEC.cpp)
target_include_directories(jpegdec_lib PUBLIC ${JPEGDEC_DIR}/src)
target_compile_definitions(jpegdec_lib PUBLIC __LINUX__=1 ALLOWS_UNALIGNED=1 ARM_MATH_CM33=1)
target_compile_options(jpegdec_lib PRIVATE -O2 -ffunction-sections -fdata-sections)

add_library(image_decoders STATIC
    ${TGX_DIR}/src/Color.cpp
    ${TGX_DIR}/src/Fonts.cpp
    ${TGX_DIR}/src/Renderer3D.cpp
    ${PICODECK_PNGDEC_SOURCES}
    ${GIF_DIR}/src/AnimatedGIF.cpp
)

target_include_directories(image_decoders PUBLIC
    ${TGX_DIR}/src
    ${JPEGDEC_DIR}/src
    ${PNGDEC_DIR}/src
    ${GIF_DIR}/src
)

target_compile_definitions(image_decoders PUBLIC __LINUX__=1 ALLOWS_UNALIGNED=1 ARM_MATH_CM33=1)
target_compile_options(image_decoders PRIVATE -O2)


# ── libmad (MP3 decoder) ─────────────────────────────────────────────────────
set(LIBMAD_DIR ${CMAKE_SOURCE_DIR}/third_party/libmad)

# Hot path — placed in SRAM via .time_critical section rename.
# synth.c (~4.9KB) is the final PCM synthesis step that runs every frame
# on Core 1; keeping it in SRAM eliminates XIP cache thrashing with
# Core 0's Lua VM.  layer3.c (~11.6KB) stays in flash — it's called less
# frequently and would exhaust the SRAM heap if included.
add_library(libmad_ram STATIC
    ${LIBMAD_DIR}/synth.c
)
target_include_directories(libmad_ram PUBLIC ${LIBMAD_DIR})
target_compile_definitions(libmad_ram PRIVATE PROGMEM=)
# Place all code in .time_critical section via -fno-function-sections (single .text)
# then rename via objcopy post-build.  -O2 for speed-critical decode path.
target_compile_options(libmad_ram PRIVATE -O2 -fno-function-sections -Wno-unused-parameter -Wno-unused-variable)

# Cold path — stays in Flash
add_library(libmad STATIC
    ${LIBMAD_DIR}/bit.c
    ${LIBMAD_DIR}/fixed.c
    ${LIBMAD_DIR}/frame.c
    ${LIBMAD_DIR}/huffman.c
    ${LIBMAD_DIR}/layer3.c
    ${LIBMAD_DIR}/stream.c
    ${LIBMAD_DIR}/timer.c
    ${LIBMAD_DIR}/version.c
)
target_include_directories(libmad PUBLIC ${LIBMAD_DIR})
target_compile_definitions(libmad PRIVATE PROGMEM=)
target_compile_options(libmad PRIVATE -O2 -Wno-unused-parameter -Wno-unused-variable)

# ── PIO PSRAM (mainboard 8MB PSRAM via PIO1 SPI) ─────────────────────────────
# Uses the polpo rp2040-psram library (third_party/rp2040-psram/).
# Pin defines are passed via compile definitions so the library source
# stays unmodified.  PIO1 is used (PIO0 is the LCD).
set(PIO_PSRAM_DIR ${CMAKE_SOURCE_DIR}/third_party/rp2040-psram)

add_library(pio_psram_lib STATIC
    ${PIO_PSRAM_DIR}/psram_spi.c
)
target_include_directories(pio_psram_lib PUBLIC ${PIO_PSRAM_DIR})
target_compile_definitions(pio_psram_lib PUBLIC
    PSRAM_PIN_CS=20
    PSRAM_PIN_SCK=21
    PSRAM_PIN_MOSI=2
    PSRAM_PIN_MISO=3
    PSRAM_MUTEX=1
)
target_link_libraries(pio_psram_lib
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_gpio
)
target_compile_options(pio_psram_lib PRIVATE -O2)
pico_generate_pio_header(pio_psram_lib ${PIO_PSRAM_DIR}/psram_spi.pio)

# ── Main firmware target ───────────────────────────────────────────────────────

add_executable(picodeck
    src/main.c
    src/dev_commands.c
    src/dev_ops.c
    src/os/launcher.c
    src/os/lua_runner.c
    src/os/app_stack.c
    src/os/native_loader.c
    src/os/elf_plan.c
    src/os/fs_path.c
    src/os/app_identity.c
    src/os/app_manifest.c
    src/os/lua_bridge.c
    src/os/lua_bridge_display.c
    src/os/lua_bridge_input.c
    src/os/lua_bridge_gamepad.c
    src/os/gamepad.c
    src/os/gamepad_map.c
    src/os/gamepad_edit.c
    src/os/lua_bridge_sys.c
    src/os/lua_bridge_fs.c
    src/os/lua_bridge_network.c
    src/os/lua_bridge_tcp.c
    src/os/lua_bridge_config.c
    src/os/appconfig.c
    src/os/lua_bridge_appconfig.c
    src/os/lua_bridge_perf.c
    src/os/perf.c
    src/os/fps_counter.c
    src/os/lua_bridge_graphics.c
    src/os/lua_bridge_3d.c
    src/os/gfx3d.c
    src/os/lua_bridge_gfx3d.c
    src/os/module_path.c
    src/os/lua_bridge_require.c
    src/os/lua_corolib.c
    src/os/lua_bridge_ui.c
    src/os/ui_widgets.c
    src/os/toast.c
    src/os/os_overlay.c
    src/os/text_wrap.c
    src/os/lua_bridge_audio.c
    src/os/lua_bridge_sound.c
    src/os/lua_bridge_repl.c
    src/os/lua_bridge_video.c
    src/os/lua_bridge_game.c
    src/os/lua_bridge_game_camera.c
    src/os/lua_bridge_game_scene.c
    src/os/lua_bridge_game_save.c
    src/os/lua_psram_alloc.c
    src/os/small_alloc.c
    src/os/core1_alloc.c
    src/os/system_menu.c
    src/os/idle_dim.c
    src/os/screenshot.c
    src/os/terminal.c
    src/os/terminal_parser.c
    src/os/terminal_render.c
    src/os/lua_bridge_terminal.c
    src/os/crypto.c
    src/os/lua_bridge_crypto.c
    src/os/lua_bridge_mod.c
    src/os/lua_bridge_zip.c
    src/os/zip_archive.c
    src/os/zip_util.c
    src/os/zip_name.c
    src/os/lua_bridge_json.c
    src/os/ota_update.c
    src/os/ota_verify.c
    ${CMAKE_BINARY_DIR}/generated/ota_pubkey.c
    src/fonts/font.c
    src/fonts/font_registry.c
    src/fonts/font_6x8.c
    src/fonts/font_8x12.c
    src/fonts/font_scientifica.c
    src/os/text_input.c
    src/os/file_browser.c
    src/os/config.c
    src/os/crashlog.c
    src/os/clock.c
    src/os/tz_picker.c
    src/os/ui.c
    src/usb/usb_descriptors.c
    src/usb/usb_msc.c
    src/os/image_decoders.cpp
    src/drivers/display.c
    src/drivers/image_api.c
    src/drivers/image_preload.c
    src/drivers/audio.c
    src/drivers/audio_mix.c
    src/drivers/sound.c
    src/drivers/fileplayer.c
    src/drivers/qoa.c
    src/drivers/wav.c
    src/drivers/mp3_player.c
    src/drivers/video_player.cpp
    src/drivers/keyboard.c
    src/drivers/kbd_bus.c
    src/drivers/kbd_i2c.c
    src/drivers/pad_source.c
    src/drivers/sdcard.c
    src/drivers/wifi.c
    src/drivers/rng.c
    src/drivers/ca_bundle.c
    src/drivers/http.c
    src/drivers/tcp.c
    src/drivers/mod_player.c
    src/drivers/pio_psram.c
    src/drivers/qmi_psram.c
    src/drivers/pio_psram_bulk.c
    src/drivers/pio_psram_qpi.c
    third_party/mongoose/mongoose.c
    third_party/umm_malloc/src/umm_malloc.c
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_zip.c
)

target_include_directories(picodeck PRIVATE
    src/usb/
    src/
    src/os/
    src/fonts/
    src/drivers/
    third_party/mongoose/
    third_party/umm_malloc/src/
    third_party/miniz/
    ${CMAKE_BINARY_DIR}/generated/miniz/
    third_party/libmad/
    third_party/pocketmod/
    data/
)

target_link_libraries(picodeck
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_i2c
    hardware_gpio
    hardware_pwm
    hardware_dma
    hardware_interp
    hardware_clocks
    hardware_pio
    hardware_xip_cache
    hardware_flash
    tinyusb_device
    lua
    fatfs
    libmad_ram
    libmad
    jpegdec_lib
    image_decoders
    pio_psram_lib
)

# ── Generate miniz_export.h stub (miniz's own CMake does this, but we
#    compile sources directly so we need to create it ourselves). Written to
#    the build tree — never into third_party/miniz, which would dirty the
#    submodule. The generated dir is on the include path above. ─────────────
if(NOT EXISTS "${CMAKE_BINARY_DIR}/generated/miniz/miniz_export.h")
    file(WRITE "${CMAKE_BINARY_DIR}/generated/miniz/miniz_export.h"
        "#ifndef MINIZ_EXPORT_H\n"
        "#define MINIZ_EXPORT_H\n"
        "#define MINIZ_EXPORT\n"
        "#endif\n"
    )
endif()

# ── miniz compile options (decompression only, no stdio, PSRAM allocator) ────
# Redirect miniz heap allocations to umm_malloc (8MB PSRAM) instead of
# standard malloc (~28KB SRAM) so ZIP extraction can handle real app sizes.
# Force-include miniz_picodeck.h which defines MZ_MALLOC/MZ_FREE/MZ_REALLOC
# before miniz_common.h's #ifndef guards.
set_source_files_properties(
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_zip.c
    PROPERTIES COMPILE_FLAGS
    "-DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -include ${CMAKE_SOURCE_DIR}/third_party/miniz_picodeck.h"
)

# Text rendering (drawText, the HUD of every game): -O2 like the blitters.
set_source_files_properties(src/fonts/font.c PROPERTIES COMPILE_OPTIONS "-O2")

# gfx3d: -O2 like the display blitters (DISP_HOT), and no FMA contraction so a
# triangle edge shared by two triangles evaluates bit-identically in both.
set_source_files_properties(src/os/gfx3d.c PROPERTIES
    COMPILE_OPTIONS "-O2;-ffp-contract=off;-Wdouble-promotion")

# ── PIO PSRAM Bulk Transfer (improved driver with 16-bit counters) ─────────────
# Supports up to 8187 bytes per transaction (vs 27 bytes in polpo library).
# Uses a custom PIO program in src/drivers/pio_psram_bulk.pio.
pico_generate_pio_header(picodeck ${CMAKE_SOURCE_DIR}/src/drivers/pio_psram_bulk.pio)
pico_generate_pio_header(picodeck ${CMAKE_SOURCE_DIR}/src/drivers/pio_psram_qpi.pio)

# Rename .text sections in libmad_ram objects to .time_critical.libmad
# so the SDK linker script places them in SRAM (the default RP2350 linker
# has *(.time_critical*) in the .data output section → RAM).
# objcopy on a .a archive processes each member object.
add_custom_command(TARGET libmad_ram POST_BUILD
    COMMAND ${CMAKE_OBJCOPY}
        --rename-section .text=.time_critical.libmad
        $<TARGET_FILE:libmad_ram>
    COMMENT "Renaming libmad_ram .text -> .time_critical.libmad (SRAM)"
    VERBATIM
)

# ── WiFi support (Pico W / Pimoroni Pico Plus 2W boards) ─────────────────────
if(PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350" OR
   PICO_BOARD STREQUAL "pico_w" OR
   PICO_BOARD STREQUAL "pico2_w")
    target_link_libraries(picodeck 
        pico_cyw43_arch_none
        pico_mbedtls
        pico_rand
    )
    target_compile_definitions(picodeck PRIVATE 
        WIFI_ENABLED=1
        MG_ENABLE_TCPIP=1
        MG_ENABLE_DRIVER_PICO_W=1
        MG_ENABLE_TCPIP_DRIVER_INIT=0
        CYW43_LWIP=0
    )
    message(STATUS "WiFi enabled for board: ${PICO_BOARD} (Mongoose TCPIP)")
else()
    message(STATUS "WiFi disabled for board: ${PICO_BOARD}")
endif()

# Enable USB serial for stdio (printf goes to USB-C serial — handy for debug)
pico_enable_stdio_usb(picodeck 1)
pico_enable_stdio_uart(picodeck 1)

pico_generate_pio_header(picodeck ${CMAKE_CURRENT_SOURCE_DIR}/src/drivers/lcd_spi.pio)

# Generate UF2, map, and disassembly
pico_add_extra_outputs(picodeck)

# ── Compiler flags ────────────────────────────────────────────────────────────

target_compile_options(picodeck PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Os                    # Optimise for size (Lua + framebuffer is large)
)

# Put the framebuffer in PSRAM section (only effective on Pimoroni boards
# with a linker script that defines .psram → PSRAM address space)
# On standard Pico 2 the attribute is silently ignored and FB goes in SRAM.
target_compile_definitions(picodeck PRIVATE
    PICODECK_VERSION="${PICODECK_VERSION}"
    PICO_RP2350=1
    PICO_MALLOC_PANIC=0   # Return NULL on SRAM OOM instead of HardFault
    PICO_STACK_SIZE=4096  # Reduced to 4KB to fit in SCRATCH_X/Y
    # MSPLIM on both cores; main.c supplies the hook to add a margin.
    PICO_USE_STACK_GUARDS=1
    PICO_RUNTIME_NO_INIT_PER_CORE_INSTALL_STACK_GUARD=1
    MG_ENABLE_MBEDTLS=1
    # Re-enable pico_stdio_usb features that get auto-disabled when
    # tinyusb_device is linked directly. We provide our own descriptors
    # (tusb_config.h + usb_descriptors.c with CDC+MSC) but still want
    # the SDK to handle tusb_init() and the background tud_task() IRQ.
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=1
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
)

# ── Print firmware size after build ───────────────────────────────────────────

add_custom_command(TARGET picodeck POST_BUILD
    COMMAND arm-none-eabi-size $<TARGET_FILE:picodeck>
    COMMENT "Firmware size:"
)
