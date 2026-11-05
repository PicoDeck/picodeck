// Stand-in for the Pico SDK header, so a gfx3d.c that places code in RAM still
// builds here. guest.ld puts .time_critical* in SRAM, as the firmware does.
#pragma once
#define __not_in_flash_func(f) __attribute__((section(".time_critical." #f))) f
#define __time_critical_func(f) __not_in_flash_func(f)
#define __not_in_flash(g) __attribute__((section(".time_critical." g)))
#define __force_inline inline __attribute__((always_inline))
