# libFuzzer targets, built from tests/unit/CMakeLists.txt with -DPICODECK_FUZZ=ON
# (see `make fuzz`).  Clang only: skipped with a warning when the compiler
# cannot link -fsanitize=fuzzer.
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
check_c_source_compiles(
    "#include <stdint.h>
     #include <stddef.h>
     int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { (void)d; (void)n; return 0; }"
    PICODECK_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if(NOT PICODECK_HAVE_LIBFUZZER)
    message(WARNING "PICODECK_FUZZ=ON but ${CMAKE_C_COMPILER} has no libFuzzer "
                    "(-fsanitize=fuzzer); fuzz targets not built. Use clang.")
    return()
endif()

set(PICODECK_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=all -fno-omit-frame-pointer)

# picodeck_fuzz_target(<name> SOURCES <files...> [INCLUDES <dirs...>])
function(picodeck_fuzz_target name)
    cmake_parse_arguments(F "" "" "SOURCES;INCLUDES" ${ARGN})
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.c ${F_SOURCES})
    target_include_directories(${name} PRIVATE ${PICODECK_ROOT}/src/os ${F_INCLUDES})
    target_compile_options(${name} PRIVATE ${PICODECK_FUZZ_FLAGS})
    target_link_options(${name} PRIVATE ${PICODECK_FUZZ_FLAGS})
endfunction()

picodeck_fuzz_target(fuzz_elf_plan SOURCES ${PICODECK_ROOT}/src/os/elf_plan.c)
picodeck_fuzz_target(fuzz_app_manifest SOURCES ${PICODECK_ROOT}/src/os/app_manifest.c)
picodeck_fuzz_target(fuzz_wav SOURCES ${PICODECK_ROOT}/src/drivers/wav.c INCLUDES ${PICODECK_ROOT}/src/drivers)
picodeck_fuzz_target(fuzz_qoa SOURCES ${PICODECK_ROOT}/src/drivers/qoa.c
                     ${PICODECK_ROOT}/third_party/qoa/qoa.c
                     INCLUDES ${PICODECK_ROOT}/src/drivers ${PICODECK_ROOT}/third_party)
target_compile_definitions(fuzz_qoa PRIVATE QOA_NO_ENCODER)
