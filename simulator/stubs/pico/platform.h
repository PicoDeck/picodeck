// pico/platform.h stub
#ifndef PICO_PLATFORM_H
#define PICO_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

// Platform identification
#define PICO_RP2040 1
#define PICO_RP2350 2

// For simulator, pretend to be RP2350
#define PICO_PLATFORM PICO_RP2350

// Compiler attributes
#define __not_in_flash(group)
#define __not_in_flash_func(func) func
#define __time_critical_func(func) func
#define __no_inline_not_in_flash_func(func) func
#define __in_flash(group)
#define __scratch_x(group)
#define __scratch_y(group)
#ifndef __force_inline
#define __force_inline inline __attribute__((always_inline))
#endif

// IO definitions
#define IO_BANK0_BASE 0x40014000u
#define PADS_BANK0_BASE 0x4001c000u

// Helper macros
#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))

#endif // PICO_PLATFORM_H
