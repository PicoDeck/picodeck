# Host unit tests for PicoDeck pure-logic modules (no Pico SDK, no SDL).
#
#   make test-unit          # configure build_unit/, build, ctest
#   make fuzz               # libFuzzer targets (clang), see tests/fuzz/
#
# Each test is one executable (tests/unit/test_<module>.c + the module's
# source).  Assertions come from check.h; a test exits non-zero on failure.
# Built with ASan+UBSan when the compiler supports them
# (-DPICODECK_UNIT_SANITIZE=OFF to disable).
cmake_minimum_required(VERSION 3.16)
project(picodeck_unit C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PICODECK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

option(PICODECK_UNIT_SANITIZE "Build unit tests with ASan+UBSan" ON)
option(PICODECK_FUZZ "Also build the libFuzzer targets (tests/fuzz)" OFF)

include(CheckCSourceCompiles)

add_compile_options(-Wall -Wextra -Werror -g -O1)

set(PICODECK_UNIT_SAN_FLAGS "")
if(PICODECK_UNIT_SANITIZE)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" PICODECK_HAVE_ASAN_UBSAN)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(PICODECK_HAVE_ASAN_UBSAN)
        set(PICODECK_UNIT_SAN_FLAGS -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
    else()
        message(WARNING "ASan/UBSan not available with ${CMAKE_C_COMPILER}; "
                        "unit tests run without sanitizers")
    endif()
endif()

enable_testing()

# picodeck_unit_test(<name> SOURCES <files...> [INCLUDES <dirs...>] [DEFINES ...])
function(picodeck_unit_test name)
    cmake_parse_arguments(T "" "" "SOURCES;INCLUDES;DEFINES" ${ARGN})
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.c ${T_SOURCES})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${PICODECK_ROOT}/src/os ${T_INCLUDES})
    target_compile_definitions(${name} PRIVATE
        PICODECK_ROOT="${PICODECK_ROOT}" PICODECK_HOST_TEST ${T_DEFINES})
    target_compile_options(${name} PRIVATE ${PICODECK_UNIT_SAN_FLAGS})
    target_link_options(${name} PRIVATE ${PICODECK_UNIT_SAN_FLAGS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endfunction()

set(SRC ${PICODECK_ROOT}/src)

picodeck_unit_test(test_font SOURCES ${SRC}/fonts/font.c ${SRC}/fonts/font_6x8.c
                   ${SRC}/fonts/font_8x12.c INCLUDES ${SRC}/fonts)
picodeck_unit_test(test_elf_plan SOURCES ${SRC}/os/elf_plan.c)
picodeck_unit_test(test_fs_path SOURCES ${SRC}/os/fs_path.c)
picodeck_unit_test(test_app_manifest SOURCES ${SRC}/os/app_manifest.c)
picodeck_unit_test(test_wav SOURCES ${SRC}/drivers/wav.c INCLUDES ${SRC}/drivers)
# QOA probing/decode for the fileplayer, against the vendored reference
# (encoder included here for in-process fixtures; firmware builds it out).
picodeck_unit_test(test_qoa SOURCES ${SRC}/drivers/qoa.c
                ${PICODECK_ROOT}/third_party/qoa/qoa.c
                INCLUDES ${SRC}/drivers ${PICODECK_ROOT}/third_party
                ${PICODECK_ROOT}/simulator/stubs)
set_source_files_properties(${PICODECK_ROOT}/third_party/qoa/qoa.c
                PROPERTIES COMPILE_OPTIONS "-Wno-error")
picodeck_unit_test(test_audio_ring INCLUDES ${SRC}/drivers)
# The MP3 staging buffer as a mixer source (header-only, like audio_ring.h).
picodeck_unit_test(test_pcm_stage INCLUDES ${SRC}/drivers)
# When Core 1's MP3 decoder decodes (header-only), with a model of the ring,
# the stage, the mixer and a paced or unpaced Core 0.
picodeck_unit_test(test_mp3_sched INCLUDES ${SRC}/drivers)
# The video player's audio chunk index: sizing and growth (header-only).
picodeck_unit_test(test_avi_index INCLUDES ${SRC}/drivers)
picodeck_unit_test(test_kbd_event_queue INCLUDES ${SRC}/drivers)
# The gamepad layer of the same decode (keys -> PAD_* through the pad map).
picodeck_unit_test(test_kbd_pad INCLUDES ${SRC}/drivers)
# Pad sources (the `pad` dev command, the simulator's controller, later a BT
# or USB pad) ORed into the same gamepad.
picodeck_unit_test(test_pad_source SOURCES ${SRC}/drivers/pad_source.c INCLUDES ${SRC}/drivers)
picodeck_unit_test(test_kbd_bus SOURCES ${SRC}/drivers/kbd_bus.c INCLUDES ${SRC}/drivers)
# The default alarm pool fix (cmake/picodeck_pico_time.cmake, #58): the
# cancellation pass cut out of the patched pico_time the firmware build
# generates, from the SDK's time.c when PICO_SDK_PATH has one the patch
# applies to, else from SDK 2.2.0's pass in fixtures/pico_time (the CI unit
# job has no SDK). The configure fails if the patch does not apply.
include(${PICODECK_ROOT}/cmake/picodeck_pico_time.cmake)
set(_pt_src "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pico_time/cancel_pass_2_2_0.c")
if(DEFINED ENV{PICO_SDK_PATH} AND
   EXISTS "$ENV{PICO_SDK_PATH}/src/common/pico_time/time.c")
    file(READ "$ENV{PICO_SDK_PATH}/src/common/pico_time/time.c" _pt_text)
    picodeck_pico_time_patch_text(_pt_text _pt_applied)
    if(_pt_applied)
        set(_pt_src "$ENV{PICO_SDK_PATH}/src/common/pico_time/time.c")
    endif()
endif()
file(READ "${_pt_src}" _pt_orig)
string(REPLACE "\r\n" "\n" _pt_orig "${_pt_orig}")
set(_pt_patched "${_pt_orig}")
picodeck_pico_time_patch_text(_pt_patched _pt_applied)
if(NOT _pt_applied)
    message(FATAL_ERROR "cmake/picodeck_pico_time.cmake does not apply to ${_pt_src}")
endif()
picodeck_pico_time_cut_pass("${_pt_orig}" _pt_pass_orig)
picodeck_pico_time_cut_pass("${_pt_patched}" _pt_pass_patched)
set(_pt_dir "${CMAKE_CURRENT_BINARY_DIR}/pico_time_pass")
file(MAKE_DIRECTORY "${_pt_dir}")
_picodeck_pico_time_write("${_pt_dir}/pass_orig.inc" "${_pt_pass_orig}")
_picodeck_pico_time_write("${_pt_dir}/pass_patched.inc" "${_pt_pass_patched}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pt_src}"
             "${PICODECK_ROOT}/cmake/picodeck_pico_time.cmake")
message(STATUS "test_pico_time_pass: the pass from ${_pt_src}")
picodeck_unit_test(test_pico_time_pass INCLUDES ${_pt_dir})
picodeck_unit_test(test_pio_psram_range INCLUDES ${SRC}/drivers)
picodeck_unit_test(test_pio_psram_xip INCLUDES ${SRC}/drivers)
picodeck_unit_test(test_qmi_psram_clkdiv INCLUDES ${SRC}/drivers)
picodeck_unit_test(test_zip_name SOURCES ${SRC}/os/zip_name.c)
picodeck_unit_test(test_text_wrap SOURCES ${SRC}/os/text_wrap.c)
# The OS FPS counter's counting rules (Show FPS; os_overlay.c draws it).
picodeck_unit_test(test_fps_counter SOURCES ${SRC}/os/fps_counter.c)
picodeck_unit_test(test_module_path SOURCES ${SRC}/os/module_path.c ${SRC}/os/fs_path.c)
# Clip-once rasterisers shared by the firmware display and the simulator.
picodeck_unit_test(test_display_clip INCLUDES ${SRC}/drivers)
target_link_libraries(test_display_clip PRIVATE m)
target_compile_options(test_display_clip PRIVATE -ffp-contract=off)
# The 3D pipeline (src/os/gfx3d.c) over host malloc; -ffp-contract=off as on
# the device so shared edges stay bit-identical.
picodeck_unit_test(test_gfx3d SOURCES ${SRC}/os/gfx3d.c INCLUDES ${SRC}/drivers
                   DEFINES GFX3D_MALLOC=malloc GFX3D_FREE=free)
target_link_libraries(test_gfx3d PRIVATE m)
target_compile_options(test_gfx3d PRIVATE -ffp-contract=off -Wdouble-promotion)
# Lua float formatting vs glibc (built without PICODECK_LUA_SPRINTF: no luaconf).
picodeck_unit_test(test_numfmt SOURCES ${SRC}/os/lua_numfmt.c)
target_link_libraries(test_numfmt PRIVATE m)
# The exhaustive sweep: opt-in (make test-numfmt-sweep), optimised, no sanitizers.
add_executable(numfmt_sweep EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/numfmt_sweep.c ${SRC}/os/lua_numfmt.c)
target_include_directories(numfmt_sweep PRIVATE ${SRC}/os)
target_compile_options(numfmt_sweep PRIVATE -O2 -Wno-error)
target_link_libraries(numfmt_sweep PRIVATE m)

# The Lua core as the firmware builds it (cmake/picodeck_lua.cmake: the VM
# configuration plus PicoDeck's sources in the Lua library), for tests of code
# that uses VM internals. `make test-unit` fetches it (download-lua).
set(LUA_SRC ${PICODECK_ROOT}/third_party/lua-5.4/src)
if(EXISTS ${LUA_SRC}/lua.h)
    include(${PICODECK_ROOT}/cmake/picodeck_lua.cmake)
    picodeck_patch_luaconf(${LUA_SRC})
    file(GLOB LUA_CORE_SOURCES ${LUA_SRC}/l*.c)
    list(FILTER LUA_CORE_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
    add_library(lua_host STATIC ${LUA_CORE_SOURCES} ${PICODECK_LUA_SOURCES})
    target_include_directories(lua_host PUBLIC ${LUA_SRC} ${SRC}/os)
    target_compile_definitions(lua_host PUBLIC ${PICODECK_LUA_DEFINITIONS})
    target_compile_options(lua_host PRIVATE ${PICODECK_UNIT_SAN_FLAGS})
    set_source_files_properties(${LUA_CORE_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(lua_host PUBLIC m)
    # The service hook's interrupt-safe, depth-independent arming (issue #21).
    picodeck_unit_test(test_lua_hook_arm)
    target_link_libraries(test_lua_hook_arm PRIVATE lua_host)
else()
    message(WARNING "Lua sources not found at ${LUA_SRC} (run `make download-lua`): "
                    "test_lua_hook_arm is not built")
endif()

# The simulator's SD-root containment (host filesystem; sim_log_err faked).
picodeck_unit_test(test_sim_sdcard SOURCES ${PICODECK_ROOT}/simulator/hal/hal_sdcard.c
                INCLUDES ${PICODECK_ROOT}/simulator/hal ${SRC}
                DEFINES PICODECK_SIMULATOR)

# Modules that use the SD card and umm_*: in-memory SD fake, umm on malloc.
# simulator/stubs first so umm_malloc.h / pico/mutex.h resolve to the stubs.
set(SD_FAKE ${CMAKE_CURRENT_SOURCE_DIR}/fakes/sdcard_fake.c
            ${CMAKE_CURRENT_SOURCE_DIR}/fakes/umm_fake.c)
set(SD_INCLUDES ${PICODECK_ROOT}/simulator/stubs ${SRC}/drivers)
picodeck_unit_test(test_config SOURCES ${SRC}/os/config.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
picodeck_unit_test(test_appconfig SOURCES ${SRC}/os/appconfig.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
# Gamepad bindings: key names, the one-button-per-key rule, the JSON files.
picodeck_unit_test(test_gamepad_map SOURCES ${SRC}/os/gamepad_map.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
# The Settings -> Controls page's state (system_menu.c draws it).
picodeck_unit_test(test_gamepad_edit SOURCES ${SRC}/os/gamepad_edit.c
                ${SRC}/os/gamepad_map.c ${SD_FAKE} INCLUDES ${SD_INCLUDES})
picodeck_unit_test(test_app_files SOURCES ${SRC}/os/app_identity.c
                ${SRC}/os/app_manifest.c ${SRC}/os/appconfig.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
if(NOT EXISTS "${CMAKE_BINARY_DIR}/generated/miniz/miniz_export.h")
    file(WRITE "${CMAKE_BINARY_DIR}/generated/miniz/miniz_export.h"
        "#ifndef MINIZ_EXPORT_H\n#define MINIZ_EXPORT_H\n#define MINIZ_EXPORT\n#endif\n")
endif()
# The g_api.zip entry points over the real zip engine and miniz (host malloc).
picodeck_unit_test(test_zip_archive SOURCES ${SRC}/os/zip_archive.c
                ${SRC}/os/zip_util.c ${SRC}/os/zip_name.c
                ${PICODECK_ROOT}/third_party/miniz/miniz.c
                ${PICODECK_ROOT}/third_party/miniz/miniz_tinfl.c
                ${PICODECK_ROOT}/third_party/miniz/miniz_tdef.c
                ${PICODECK_ROOT}/third_party/miniz/miniz_zip.c
                ${SD_FAKE}
                INCLUDES ${SD_INCLUDES} ${PICODECK_ROOT}/third_party/miniz
                ${CMAKE_BINARY_DIR}/generated/miniz
                DEFINES PICODECK_SIMULATOR MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_WRITING_APIS
                MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_options(test_zip_archive PRIVATE -Wno-unused-parameter)
set_source_files_properties(
                ${PICODECK_ROOT}/third_party/miniz/miniz.c
                ${PICODECK_ROOT}/third_party/miniz/miniz_tinfl.c
                ${PICODECK_ROOT}/third_party/miniz/miniz_tdef.c
                ${PICODECK_ROOT}/third_party/miniz/miniz_zip.c
                PROPERTIES COMPILE_OPTIONS "-Wno-error;-w")
# The firmware sample mixer (sound.c): sample/player lifetimes and slots.
picodeck_unit_test(test_sound SOURCES ${SRC}/drivers/sound.c ${SRC}/drivers/wav.c
                ${SD_FAKE} INCLUDES ${SD_INCLUDES})
# The pico/critical_section.h stub leaves a parameter unused.
target_compile_options(test_sound PRIVATE -Wno-unused-parameter)
# The one mixer (audio_mix.c) over the real sample mixer, with the device
# output faked in the test.
picodeck_unit_test(test_audio_mix SOURCES ${SRC}/drivers/audio_mix.c
                ${SRC}/drivers/sound.c ${SRC}/drivers/wav.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
target_compile_options(test_audio_mix PRIVATE -Wno-unused-parameter)
# The firmware WAV streamer (fileplayer.c, which the simulator also runs):
# flow control against a model of the stream ring, per-player files, and
# Core 0 stop vs Core 1 update on two threads.
picodeck_unit_test(test_fileplayer SOURCES ${SRC}/drivers/fileplayer.c
                ${SRC}/drivers/wav.c ${SRC}/drivers/qoa.c
                ${PICODECK_ROOT}/third_party/qoa/qoa.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES} ${PICODECK_ROOT}/third_party)
target_link_libraries(test_fileplayer PRIVATE pthread)
# The MP3 player's fed mode (the video player's audio: its loop point) over
# libmad, built as the simulator builds it. Third-party: no sanitizers (its
# synthesis reads D[][] across a row, which -fsanitize=bounds reports), no
# warnings.
set(LIBMAD_DIR ${PICODECK_ROOT}/third_party/libmad)
add_library(libmad_host STATIC ${LIBMAD_DIR}/bit.c ${LIBMAD_DIR}/fixed.c
            ${LIBMAD_DIR}/frame.c ${LIBMAD_DIR}/huffman.c ${LIBMAD_DIR}/layer3.c
            ${LIBMAD_DIR}/stream.c ${LIBMAD_DIR}/synth.c ${LIBMAD_DIR}/timer.c
            ${LIBMAD_DIR}/version.c)
target_include_directories(libmad_host PUBLIC ${LIBMAD_DIR})
target_compile_definitions(libmad_host PRIVATE FPM_64BIT HAVE_STDINT_H=1
            SIZEOF_INT=4 SIZEOF_LONG=${CMAKE_SIZEOF_VOID_P} SIZEOF_LONG_LONG=8
            NDEBUG=1 OPT_SPEED=1 OPT_SSO=1 PROGMEM=)
target_compile_options(libmad_host PRIVATE -O2 -w)
picodeck_unit_test(test_mp3_fed SOURCES ${SRC}/drivers/mp3_player.c
                ${SRC}/os/core0_idle.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
target_link_libraries(test_mp3_fed PRIVATE libmad_host)
# The pico/critical_section.h stub leaves a parameter unused.
target_compile_options(test_mp3_fed PRIVATE -Wno-unused-parameter)
# The restart tests model a frame's decode time: the test's
# __wrap_mad_synth_frame renders output after each frame.
target_link_options(test_mp3_fed PRIVATE -Wl,--wrap=mad_synth_frame)
# ...and its decode loop at the ends of the input buffer (no frame lost
# where a refill ends), from a file (the SD fake) and fed.
picodeck_unit_test(test_mp3_refill SOURCES ${SRC}/drivers/mp3_player.c
                ${SRC}/os/core0_idle.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
target_link_libraries(test_mp3_refill PRIVATE libmad_host)
target_compile_options(test_mp3_refill PRIVATE -Wno-unused-parameter)
# ...and what its decoder writes (a mono frame's samples, in order) and when
# (ahead, in Core 0's idle windows).
picodeck_unit_test(test_mp3_decode SOURCES ${SRC}/drivers/mp3_player.c
                ${SRC}/os/core0_idle.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
target_link_libraries(test_mp3_decode PRIVATE libmad_host)
target_compile_options(test_mp3_decode PRIVATE -Wno-unused-parameter)
# The MOD player (mod_player.c + pocketmod): Core 0 load/free vs Core 1
# update on two threads.
picodeck_unit_test(test_mod_player SOURCES ${SRC}/drivers/mod_player.c ${SD_FAKE}
                INCLUDES ${SD_INCLUDES} ${PICODECK_ROOT}/third_party/pocketmod)
target_link_libraries(test_mod_player PRIVATE pthread m)
# pocketmod.h is third-party: not held to -Werror's extra warnings.
target_compile_options(test_mod_player PRIVATE -Wno-error -Wno-unused-parameter
                       -Wno-sign-compare -Wno-format)

# OTA image authentication (ECDSA P-256 + strict .sha256), against the same
# mbedTLS the firmware links (host build, cmake/picodeck_host_mbedtls.cmake) and
# the TEST public key the dev firmware embeds.
include(${PICODECK_ROOT}/cmake/picodeck_host_mbedtls.cmake)
include(${PICODECK_ROOT}/cmake/picodeck_update_key.cmake)
picodeck_generate_update_pubkey(${CMAKE_CURRENT_BINARY_DIR}/ota_pubkey.c
                             ${PICODECK_TEST_UPDATE_PUBKEY})
picodeck_unit_test(test_ota_verify
                SOURCES ${SRC}/os/ota_verify.c ${CMAKE_CURRENT_BINARY_DIR}/ota_pubkey.c
                        ${SD_FAKE}
                INCLUDES ${SD_INCLUDES})
target_link_libraries(test_ota_verify PRIVATE picodeck_host_mbedtls)
# The TLS root bundle parses whole and verifies the chains PicoDeck relies on.
picodeck_unit_test(test_ca_bundle SOURCES ${SRC}/drivers/ca_bundle.c
                INCLUDES ${SRC}/drivers)
target_link_libraries(test_ca_bundle PRIVATE picodeck_host_mbedtls)

picodeck_unit_test(test_small_alloc SOURCES ${SRC}/os/small_alloc.c)

if(PICODECK_FUZZ)
    add_subdirectory(${PICODECK_ROOT}/tests/fuzz ${CMAKE_CURRENT_BINARY_DIR}/fuzz)
endif()
