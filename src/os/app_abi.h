#pragma once
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// PicoDeck Native App ABI
//
// Native apps are Position-Independent ELF32 binaries placed at
// /apps/<name>/main.elf on the SD card.  The OS loads them into PSRAM at
// runtime and calls the entry point below.
//
// Symbol name:  picodeck_main   (set via --entry=picodeck_main in the linker)
// Calling conv: ARM AAPCS (C calling convention)
// =============================================================================

// Forward declaration — app_abi.h is dependency-free; include os.h for the
// full PicoCalcAPI definition.
typedef struct PicoCalcAPI PicoCalcAPI;

// Primary entry point function type for native apps.
typedef void (*picodeck_app_entry_t)(const PicoCalcAPI *api,
                                   const char        *app_dir,
                                   const char        *app_id,
                                   const char        *app_name);
