cmake_minimum_required(VERSION 3.13)

# PicoDeck PC Simulator
# Build with: cmake -DPICODECK_SIMULATOR=ON ..

project(picodeck_simulator C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# SIM_FIRMWARE_NET (see "Simulator Network Library" below) changes what
# hardware/sync.h means for every translation unit (real spinlocks instead of
# no-ops, simulator/stubs/hardware/sync.h), so the define is global.
option(SIM_FIRMWARE_NET
       "Simulator runs the firmware network stack (wifi.c/http.c/tcp.c on Mongoose/POSIX)"
       OFF)
if(SIM_FIRMWARE_NET)
    add_compile_definitions(PICODECK_SIM_FIRMWARE_NET=1)
endif()

# Parent directory (PicoDeck root)
set(PICODECK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Find required packages
if(EMSCRIPTEN)
    # Web demo build (emcmake cmake ..): SDL2 comes from Emscripten's ports,
    # and there are no threads, libcurl or Unicorn in a browser. See web/.
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        INTERFACE_COMPILE_OPTIONS "-sUSE_SDL=2"
        INTERFACE_LINK_OPTIONS "-sUSE_SDL=2")
    add_library(Threads::Threads INTERFACE IMPORTED)
    # musl only exposes the glibc-style `uint` typedef the SDK stubs use here
    add_compile_definitions(_GNU_SOURCE)
    set(SIM_SIZEOF_LONG 4)
    # The web project's half of the build (simulator/CLAUDE.md, "Web build
    # seam"): PICODECK_WEB_DIR holds web.cmake, which sets the platform and
    # network-stub sources and defines picodeck_web_package(target).
    if(NOT PICODECK_WEB_DIR)
        set(PICODECK_WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
    endif()
    get_filename_component(PICODECK_WEB_DIR "${PICODECK_WEB_DIR}" ABSOLUTE)
    if(NOT EXISTS ${PICODECK_WEB_DIR}/web.cmake)
        message(FATAL_ERROR "PICODECK_WEB_DIR=${PICODECK_WEB_DIR} has no web.cmake")
    endif()
    include(${PICODECK_WEB_DIR}/web.cmake)
else()
    find_package(SDL2 REQUIRED)
    find_package(SDL2_image REQUIRED)
    find_package(Threads REQUIRED)
    find_package(CURL REQUIRED)
    set(SIM_SIZEOF_LONG 8)
endif()

# Version from git (same as main project)
execute_process(
    COMMAND git describe --tags --always
    WORKING_DIRECTORY ${PICODECK_ROOT}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_VERSION)
    string(REGEX REPLACE "^v" "" PICODECK_VERSION "${GIT_VERSION}")
else()
    set(PICODECK_VERSION "0.0.0-sim")
endif()
message(STATUS "PicoDeck Simulator version: ${PICODECK_VERSION}")

# ── Lua 5.4 ─────────────────────────────────────────────────────────────────
set(LUA_DIR ${PICODECK_ROOT}/third_party/lua-5.4/src)

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
    ${LUA_DIR}/lauxlib.c
    ${LUA_DIR}/lbaselib.c
    ${LUA_DIR}/lmathlib.c
    ${LUA_DIR}/lstrlib.c
    ${LUA_DIR}/ltablib.c
    ${LUA_DIR}/lutf8lib.c
    ${LUA_DIR}/lcorolib.c
)

add_library(lua_sim STATIC ${LUA_SOURCES})
target_include_directories(lua_sim PUBLIC ${LUA_DIR})
# Same Lua config as the firmware (cmake/picodeck_lua.cmake) so the simulator VM
# has identical numeric semantics and stack limit. PUBLIC so the bridge agrees.
include(${PICODECK_ROOT}/cmake/picodeck_lua.cmake)
picodeck_patch_luaconf(${LUA_DIR})
target_compile_definitions(lua_sim PUBLIC ${PICODECK_LUA_DEFINITIONS})
target_sources(lua_sim PRIVATE ${PICODECK_LUA_SOURCES})  # picodeck_lua_sprintf, picodeck_lua_arm_hook
target_compile_definitions(lua_sim PRIVATE PICODECK_SIMULATOR=1)
target_compile_options(lua_sim PRIVATE -O2 -Wall)
# The string hash seed comes from sim_test_control.c (sim_lua_seed.h): fixed
# under --test-mode so pairs() order over string keys repeats run to run
# (simulator only; the firmware keeps stock luai_makeseed).
set_source_files_properties(${LUA_DIR}/lstate.c PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/sim_lua_seed.h")

# ── libmad (MP3 decoder) ──────────────────────────────────────────────────
set(LIBMAD_DIR ${PICODECK_ROOT}/third_party/libmad)

add_library(libmad_sim STATIC
    ${LIBMAD_DIR}/bit.c
    ${LIBMAD_DIR}/fixed.c
    ${LIBMAD_DIR}/frame.c
    ${LIBMAD_DIR}/huffman.c
    ${LIBMAD_DIR}/layer3.c
    ${LIBMAD_DIR}/stream.c
    ${LIBMAD_DIR}/synth.c
    ${LIBMAD_DIR}/timer.c
    ${LIBMAD_DIR}/version.c
)
target_include_directories(libmad_sim PUBLIC ${LIBMAD_DIR})
target_compile_definitions(libmad_sim PRIVATE
    FPM_64BIT
    HAVE_STDINT_H=1
    SIZEOF_INT=4
    SIZEOF_LONG=${SIM_SIZEOF_LONG}
    SIZEOF_LONG_LONG=8
    NDEBUG=1
    OPT_SPEED=1
    OPT_SSO=1
    PROGMEM=             # empty — no flash storage on simulator
)
target_compile_options(libmad_sim PRIVATE -O2 -Wall -Wno-unused-parameter -Wno-unused-result)

# ── Image Decoders (same as main) ───────────────────────────────────────────
set(TGX_DIR ${PICODECK_ROOT}/third_party/tgx)
set(JPEGDEC_DIR ${PICODECK_ROOT}/third_party/JPEGDEC)
set(PNGDEC_DIR ${PICODECK_ROOT}/third_party/PNGdec)
set(GIF_DIR ${PICODECK_ROOT}/third_party/AnimatedGIF)

# PNGdec's zlib with its unaligned word copies made alignment-safe (patched
# copies in the build tree; the submodule stays pristine). Shared with the
# firmware build — see the module for why.
include(${PICODECK_ROOT}/cmake/picodeck_pngdec.cmake)
picodeck_pngdec_sources(PICODECK_PNGDEC_SOURCES ${PNGDEC_DIR}/src)

add_library(image_decoders_sim STATIC
    ${TGX_DIR}/src/Color.cpp
    ${TGX_DIR}/src/Fonts.cpp
    ${TGX_DIR}/src/Renderer3D.cpp
    ${JPEGDEC_DIR}/src/JPEGDEC.cpp
    ${PICODECK_PNGDEC_SOURCES}
    ${GIF_DIR}/src/AnimatedGIF.cpp
)

# PNGdec bundles its own zlib, whose crc32() clashes with Unicorn's crc32c.c.
# Rename it on every PNGdec source so the two never meet at link time.
set_source_files_properties(
    ${PICODECK_PNGDEC_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS "crc32=pngdec_crc32"
)

target_include_directories(image_decoders_sim PUBLIC
    ${TGX_DIR}/src
    ${JPEGDEC_DIR}/src
    ${PNGDEC_DIR}/src
    ${GIF_DIR}/src
)
target_compile_definitions(image_decoders_sim PUBLIC __LINUX__=1 PICODECK_SIMULATOR=1)
target_compile_options(image_decoders_sim PRIVATE -O2)

# ── Simulator HAL ───────────────────────────────────────────────────────────
add_library(sim_hal STATIC
    hal/hal_display.c
    hal/hal_input.c
    hal/hal_pad.c
    ${PICODECK_ROOT}/src/drivers/pad_source.c
    hal/hal_sdcard.c
    hal/hal_psram.c
    hal/hal_timing.c
    hal/hal_audio.c
    hal/hal_threading.c
    sim_audio.c
    stubs/pico_sdk_stubs.c
    stubs/hardware_stubs.c
    stubs/driver_stubs.c
    stubs/sim_real_umm.c
    stubs/keyboard_stub.c
)

target_include_directories(sim_hal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${PICODECK_ROOT}/src
    ${PICODECK_ROOT}/src/os
    ${PICODECK_ROOT}/src/fonts
    ${PICODECK_ROOT}/src/drivers
    ${PICODECK_ROOT}/third_party/umm_malloc/src
    ${LIBMAD_DIR}
)

target_link_libraries(sim_hal PUBLIC
    SDL2::SDL2
    Threads::Threads
    libmad_sim
)

target_compile_definitions(sim_hal PUBLIC
    PICODECK_SIMULATOR=1
    PICODECK_VERSION="${PICODECK_VERSION}"
    DISPLAY_SCALE=3
    SIMULATOR_BUILD=1
)

# ── Simulator Network Library ───────────────────────────────────────────────
# Default: the simulator's own libcurl/POSIX layer (sim_wifi.c, sim_http.c,
# sim_tcp.c) behind the firmware's http.h/tcp.h/wifi.h API.
#
# SIM_FIRMWARE_NET=ON (make simulator-net): the firmware's own
# src/drivers/wifi.c, http.c and tcp.c instead, on the vendored Mongoose
# built for the host (MG_ARCH_UNIX: BSD sockets, no built-in TCP/IP stack, no
# TLS), with Core 0 = the main thread and Core 1 = the sim's core1_thread.
# The host shims live in simulator/net/ (see sim_net_shim.c); the firmware
# files carry only PICODECK_SIM_FIRMWARE_NET guards. This is the build that can
# reproduce the Core 0/Core 1 close races (specs/test-audit-2026-09-24.md
# R13); tests/e2e/test_network_firmware.py runs only against it.
if(EMSCRIPTEN)
    # Web build: no network in the browser; the web project's stubs
    # (PICODECK_WEB_NET_SOURCES) return NULL from every connection allocation.
    add_library(sim_network STATIC sim_wifi.c ${PICODECK_WEB_NET_SOURCES})
elseif(SIM_FIRMWARE_NET)
    add_library(sim_network STATIC
        ${PICODECK_ROOT}/src/drivers/wifi.c
        ${PICODECK_ROOT}/src/drivers/http.c
        ${PICODECK_ROOT}/src/drivers/tcp.c
        ${PICODECK_ROOT}/third_party/mongoose/mongoose.c
        net/sim_net_shim.c
    )
    # net/include first: its mongoose.h shadows the real one for the firmware
    # sources (stand-in Wi-Fi interface) and it carries pico/multicore.h.
    target_include_directories(sim_network BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/net/include
        ${CMAKE_CURRENT_SOURCE_DIR}/net
    )
    target_include_directories(sim_network PRIVATE
        ${PICODECK_ROOT}/third_party/mongoose
    )
    # Mongoose as the firmware configures it, minus what a host build can't
    # have (third_party/mongoose/mongoose_config.h): mg_calloc/mg_free come
    # from http.c (core1_calloc → host calloc), mg_random from the shim, and
    # the same 8 KB IO granularity.
    target_compile_definitions(sim_network PRIVATE
        MG_ENABLE_CUSTOM_CALLOC=1
        MG_ENABLE_CUSTOM_RANDOM=1
        MG_IO_SIZE=8192
        # The same loopback stand-ins for wifi.c's SNTP server and
        # connectivity-check target (net/include/mongoose.h).
        "WIFI_SNTP_URL=sim_net_sntp_url()"
        "WIFI_CHECK_URL=sim_net_check_url()"
        # http.c/tcp.c reach rx_buf through the RP2350's uncached PSRAM alias
        # (+0x04000000); the host heap has none.
        PSRAM_UNCACHED_OFFSET=0
    )
    # -g: crash-log backtraces from Core 1 then name the firmware lines.
    target_compile_options(sim_network PRIVATE -Wall -Wextra -Wno-unused-parameter -g)
    set_source_files_properties(${PICODECK_ROOT}/third_party/mongoose/mongoose.c
        PROPERTIES COMPILE_OPTIONS "-w")
    # wifi.c prints an int64_t with %lld: right on the ARM target (long long),
    # a -Wformat warning on LP64 hosts (long). Firmware code stays untouched.
    set_source_files_properties(${PICODECK_ROOT}/src/drivers/wifi.c
        PROPERTIES COMPILE_OPTIONS "-Wno-format")
    # http.c includes mbedtls/platform.h (declarations only); wifi.c reaches
    # lua.h through os/system_menu.h and needs the project's Lua and its
    # config (lua_sim), never a host's /usr/include/lua.h.
    target_link_libraries(sim_network PRIVATE picodeck_host_mbedtls lua_sim)
else()
    add_library(sim_network STATIC
        sim_wifi.c
        sim_http.c
        sim_tcp.c
    )
endif()
target_include_directories(sim_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${PICODECK_ROOT}/src
    ${PICODECK_ROOT}/src/os
    ${PICODECK_ROOT}/src/drivers
    ${PICODECK_ROOT}/third_party/umm_malloc/src
)
if(NOT EMSCRIPTEN)
    target_link_libraries(sim_network PUBLIC CURL::libcurl Threads::Threads)
endif()
target_compile_definitions(sim_network PUBLIC PICODECK_SIMULATOR=1 WIFI_ENABLED=1)

# ── Unicorn Engine (ARM emulation for native apps) ─────────────────────────
if(NOT EMSCRIPTEN)
include(FetchContent)
FetchContent_Declare(
    unicorn
    GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
    GIT_TAG        2.1.1
    GIT_SHALLOW    TRUE
)
# Build Unicorn as static library, disable unneeded architectures
set(UNICORN_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(unicorn)
endif()

# ── Generate miniz_export.h stub (miniz's own CMake does this, but we
#    compile sources directly so we need to create it ourselves). Written to
#    the build tree — never into third_party/miniz, which would dirty the
#    submodule. The generated dir is on picodeck_simulator's include path. ──────
if(NOT EXISTS "${CMAKE_BINARY_DIR}/generated/miniz/miniz_export.h")
    file(WRITE "${CMAKE_BINARY_DIR}/generated/miniz/miniz_export.h"
        "#ifndef MINIZ_EXPORT_H\n"
        "#define MINIZ_EXPORT_H\n"
        "#define MINIZ_EXPORT\n"
        "#endif\n"
    )
endif()

# ── Main Simulator Executable ───────────────────────────────────────────────
set(PICODECK_CORE_SOURCES
    ${PICODECK_ROOT}/src/os/launcher.c
    ${PICODECK_ROOT}/src/os/lua_runner.c
    ${PICODECK_ROOT}/src/os/app_stack.c
    ${PICODECK_ROOT}/src/os/native_loader.c
    ${PICODECK_ROOT}/src/os/elf_plan.c
    ${PICODECK_ROOT}/src/os/fs_path.c
    ${PICODECK_ROOT}/src/os/app_identity.c
    ${PICODECK_ROOT}/src/os/app_manifest.c
    ${PICODECK_ROOT}/src/os/lua_bridge.c
    ${PICODECK_ROOT}/src/os/lua_bridge_display.c
    ${PICODECK_ROOT}/src/os/lua_bridge_input.c
    ${PICODECK_ROOT}/src/os/lua_bridge_gamepad.c
    ${PICODECK_ROOT}/src/os/gamepad.c
    ${PICODECK_ROOT}/src/os/gamepad_map.c
    ${PICODECK_ROOT}/src/os/gamepad_edit.c
    ${PICODECK_ROOT}/src/os/lua_bridge_sys.c
    ${PICODECK_ROOT}/src/os/lua_bridge_fs.c
    ${PICODECK_ROOT}/src/os/lua_bridge_config.c
    ${PICODECK_ROOT}/src/os/appconfig.c
    ${PICODECK_ROOT}/src/os/lua_bridge_appconfig.c
    ${PICODECK_ROOT}/src/os/lua_bridge_perf.c
    ${PICODECK_ROOT}/src/os/perf.c
    ${PICODECK_ROOT}/src/os/fps_counter.c
    ${PICODECK_ROOT}/src/os/lua_bridge_graphics.c
    ${PICODECK_ROOT}/src/os/lua_bridge_3d.c
    ${PICODECK_ROOT}/src/os/gfx3d.c
    ${PICODECK_ROOT}/src/os/lua_bridge_gfx3d.c
    ${PICODECK_ROOT}/src/os/module_path.c
    ${PICODECK_ROOT}/src/os/lua_bridge_require.c
    ${PICODECK_ROOT}/src/os/lua_corolib.c
    ${PICODECK_ROOT}/src/os/lua_bridge_ui.c
    ${PICODECK_ROOT}/src/os/toast.c
    ${PICODECK_ROOT}/src/os/os_overlay.c
    ${PICODECK_ROOT}/src/os/lua_bridge_audio.c
    ${PICODECK_ROOT}/src/os/lua_bridge_sound.c
    ${PICODECK_ROOT}/src/os/lua_bridge_repl.c
    ${PICODECK_ROOT}/src/os/lua_bridge_network.c
    ${PICODECK_ROOT}/src/os/lua_bridge_tcp.c
    ${PICODECK_ROOT}/src/os/lua_bridge_video.c
    ${PICODECK_ROOT}/src/os/lua_bridge_game.c
    ${PICODECK_ROOT}/src/os/lua_bridge_game_camera.c
    ${PICODECK_ROOT}/src/os/lua_bridge_game_scene.c
    ${PICODECK_ROOT}/src/os/lua_bridge_game_save.c
    ${PICODECK_ROOT}/src/os/lua_psram_alloc.c
    ${PICODECK_ROOT}/src/os/small_alloc.c
    ${PICODECK_ROOT}/src/os/crashlog.c
    ${PICODECK_ROOT}/src/os/system_menu.c
    ${PICODECK_ROOT}/src/os/idle_dim.c
    ${PICODECK_ROOT}/src/os/screenshot.c
    ${PICODECK_ROOT}/src/os/terminal.c
    ${PICODECK_ROOT}/src/os/terminal_parser.c
    ${PICODECK_ROOT}/src/os/terminal_render.c
    ${PICODECK_ROOT}/src/os/lua_bridge_terminal.c
    ${PICODECK_ROOT}/src/os/lua_bridge_crypto.c
    ${PICODECK_ROOT}/src/os/lua_bridge_mod.c
    ${PICODECK_ROOT}/src/os/lua_bridge_zip.c
    ${PICODECK_ROOT}/src/os/zip_archive.c
    ${PICODECK_ROOT}/src/os/zip_util.c
    ${PICODECK_ROOT}/src/os/zip_name.c
    ${PICODECK_ROOT}/src/os/lua_bridge_json.c
    ${PICODECK_ROOT}/src/drivers/mod_player.c
    ${PICODECK_ROOT}/src/drivers/fileplayer.c
    ${PICODECK_ROOT}/src/drivers/audio_mix.c
    ${PICODECK_ROOT}/src/drivers/sound.c
    ${PICODECK_ROOT}/src/drivers/mp3_player.c
    ${PICODECK_ROOT}/src/drivers/wav.c
    ${PICODECK_ROOT}/src/drivers/qoa.c
    ${PICODECK_ROOT}/third_party/miniz/miniz.c
    ${PICODECK_ROOT}/third_party/miniz/miniz_tinfl.c
    ${PICODECK_ROOT}/third_party/miniz/miniz_tdef.c
    ${PICODECK_ROOT}/third_party/miniz/miniz_zip.c
    ${PICODECK_ROOT}/src/os/text_input.c
    ${PICODECK_ROOT}/src/os/file_browser.c
    ${PICODECK_ROOT}/src/os/config.c
    ${PICODECK_ROOT}/src/os/clock.c
    ${PICODECK_ROOT}/src/os/tz_picker.c
    ${PICODECK_ROOT}/src/os/ui.c
    ${PICODECK_ROOT}/src/os/ui_widgets.c
    ${PICODECK_ROOT}/src/os/text_wrap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/dev_commands_stub.c
    ${PICODECK_ROOT}/src/dev_ops.c
    ${PICODECK_ROOT}/src/fonts/font.c
    ${PICODECK_ROOT}/src/fonts/font_registry.c
    ${PICODECK_ROOT}/src/fonts/font_6x8.c
    ${PICODECK_ROOT}/src/fonts/font_8x12.c
    ${PICODECK_ROOT}/src/fonts/font_scientifica.c
    ${PICODECK_ROOT}/src/os/image_decoders.cpp
    ${PICODECK_ROOT}/src/drivers/image_api.c
    ${PICODECK_ROOT}/src/os/text_wrap.c
    ${PICODECK_ROOT}/src/os/ui_widgets.c
)

# Filter out crypto (not yet implemented in simulator)
# Network and TCP are now provided by sim_network + real lua_bridge files
list(REMOVE_ITEM PICODECK_CORE_SOURCES
    ${PICODECK_ROOT}/src/os/lua_bridge_crypto.c
)

# OTA image checks (sys.applyUpdate): the firmware's own ota_verify.c against a
# host mbedTLS and the TEST update key, so the simulator refuses exactly what
# the device refuses (flashing itself stays firmware-only).
include(${PICODECK_ROOT}/cmake/picodeck_host_mbedtls.cmake)
include(${PICODECK_ROOT}/cmake/picodeck_update_key.cmake)
picodeck_generate_update_pubkey(${CMAKE_BINARY_DIR}/generated/ota_pubkey.c
                             ${PICODECK_TEST_UPDATE_PUBKEY})
list(APPEND PICODECK_CORE_SOURCES
    ${PICODECK_ROOT}/src/os/ota_verify.c
    ${CMAKE_BINARY_DIR}/generated/ota_pubkey.c
)

if(EMSCRIPTEN)
    set(SIM_PLATFORM_SOURCES ${PICODECK_WEB_PLATFORM_SOURCES})
else()
    set(SIM_PLATFORM_SOURCES
        sim_socket.c
        sim_socket_handler.c
        unicorn_runner.c
        unicorn_trampolines.c
    )
endif()

add_executable(picodeck_simulator
    main.c
    sim_test_control.c
    ${SIM_PLATFORM_SOURCES}
    ${PICODECK_CORE_SOURCES}
)

set_source_files_properties(${PICODECK_ROOT}/src/os/gfx3d.c PROPERTIES
    COMPILE_OPTIONS "-ffp-contract=off")

target_include_directories(picodeck_simulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${PICODECK_ROOT}/src
    ${PICODECK_ROOT}/src/os
    ${PICODECK_ROOT}/src/fonts
    ${PICODECK_ROOT}/src/drivers
    ${PICODECK_ROOT}/src/usb
    ${PICODECK_ROOT}/third_party/umm_malloc/src
    ${PICODECK_ROOT}/third_party/miniz
    ${CMAKE_BINARY_DIR}/generated/miniz
    ${PICODECK_ROOT}/third_party/stb
    ${PICODECK_ROOT}/third_party/pocketmod
    ${unicorn_SOURCE_DIR}/include
)

target_link_libraries(picodeck_simulator PRIVATE
    sim_hal
    sim_network
    lua_sim
    image_decoders_sim
    SDL2::SDL2
    Threads::Threads
    picodeck_host_mbedtls
)
if(NOT EMSCRIPTEN)
    target_link_libraries(picodeck_simulator PRIVATE
        unicorn
        SDL2_image::SDL2_image
        CURL::libcurl
    )
endif()

target_compile_definitions(picodeck_simulator PRIVATE
    PICODECK_SIMULATOR=1
    PICODECK_VERSION="${PICODECK_VERSION}"
    DISPLAY_SCALE=3
    SIMULATOR_BUILD=1
    WIFI_ENABLED=1
    PICODECK_PROJECT_ROOT="${PICODECK_ROOT}"
)

target_compile_options(picodeck_simulator PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -O2
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:-g>
)

# miniz: decompression only, no stdio, use standard malloc on simulator (no umm_malloc)
set_source_files_properties(
    ${PICODECK_ROOT}/third_party/miniz/miniz.c
    ${PICODECK_ROOT}/third_party/miniz/miniz_tinfl.c
    ${PICODECK_ROOT}/third_party/miniz/miniz_tdef.c
    ${PICODECK_ROOT}/third_party/miniz/miniz_zip.c
    PROPERTIES COMPILE_DEFINITIONS
    "MINIZ_NO_STDIO;MINIZ_NO_ARCHIVE_WRITING_APIS;MINIZ_NO_ZLIB_COMPATIBLE_NAMES"
)

# No --allow-multiple-definition: it silently let simulator stubs win over
# the real implementations (audio_ring_free, image_*), so a duplicate symbol
# must now be a link error. The one legitimate clash — Unicorn's crc32c.c and
# PNGdec's bundled zlib both defining crc32 — is resolved by renaming
# PNGdec's copy on its sources (the set_source_files_properties on the PNGdec sources).

# ── Sanitizer builds (make simulator-asan / simulator-tsan) ─────────────────
# PICODECK_SIM_SANITIZE is a list of -fsanitize= kinds, e.g. "address;undefined"
# or "thread". It instruments the simulator and everything it compiles from
# the PicoDeck tree (OS, bridge, Lua, decoders, HAL, network) but NOT Unicorn,
# which is third-party JIT code and only slows the run down. Since the sim maps
# umm_* and the Lua heap onto malloc, ASan sees every bridge allocation.
set(PICODECK_SIM_SANITIZE "" CACHE STRING
    "Sanitizers for the simulator: '' (off), 'address;undefined' or 'thread'")
set(_picodeck_san "")
if(PICODECK_SIM_SANITIZE)
    string(REPLACE ";" "," _picodeck_san "${PICODECK_SIM_SANITIZE}")
    set(_picodeck_san_flags -fsanitize=${_picodeck_san} -fno-omit-frame-pointer -g)
    if("undefined" IN_LIST PICODECK_SIM_SANITIZE)
        # Every UB report is fatal, so the harness sees it as a dead sim.
        list(APPEND _picodeck_san_flags -fno-sanitize-recover=undefined)
    endif()
    foreach(_t picodeck_simulator sim_hal sim_network lua_sim libmad_sim image_decoders_sim)
        target_compile_options(${_t} PRIVATE ${_picodeck_san_flags})
    endforeach()
    if("undefined" IN_LIST PICODECK_SIM_SANITIZE)
        # libmad's polyphase synthesis reads a row of D[17][32] through a
        # pointer offset back from the row start (synth.c synth_full: ptr =
        # *Dptr - po; ptr[31 - n]): inside D, outside the row that
        # -fsanitize=bounds checks. Decoding any MP3 tripped it.
        set_source_files_properties(${LIBMAD_DIR}/synth.c PROPERTIES
            COMPILE_OPTIONS -fno-sanitize=bounds)
    endif()
    target_link_options(picodeck_simulator PRIVATE -fsanitize=${_picodeck_san})
    message(STATUS "  - Sanitizers: ${_picodeck_san}")
endif()
# Reported by `picodeck_simulator --build-info` so the E2E harness can tell a
# sanitizer build from a release one (asan_only tests).
target_compile_definitions(picodeck_simulator PRIVATE
    PICODECK_SIM_SANITIZE_STR="${_picodeck_san}")
if(SIM_FIRMWARE_NET)
    message(STATUS "  - Network: firmware wifi.c/http.c/tcp.c on Mongoose/POSIX (SIM_FIRMWARE_NET)")
endif()

# ── Install / Run ───────────────────────────────────────────────────────────
set_target_properties(picodeck_simulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Web demo packaging ──────────────────────────────────────────────────────
# The SD image, link options and page come from the web project's web.cmake.
if(EMSCRIPTEN)
    picodeck_web_package(picodeck_simulator)
endif()

# Copy SDL2 DLL on Windows (if needed)
if(WIN32)
    add_custom_command(TARGET picodeck_simulator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL2::SDL2>
        $<TARGET_FILE_DIR:picodeck_simulator>
    )
endif()

message(STATUS "Simulator configured:")
message(STATUS "  - Display: 960x960 (3x scale)")
message(STATUS "  - Audio: SDL2 enabled")
message(STATUS "  - Threading: Dual-core simulation")
message(STATUS "  - SD Card: Host filesystem")
if(SIM_FIRMWARE_NET)
    message(STATUS "  - WiFi/HTTP/TCP: firmware drivers on Mongoose/POSIX (TLS off)")
else()
    message(STATUS "  - WiFi: Mock (always connected)")
    message(STATUS "  - HTTP/TCP: libcurl")
endif()
