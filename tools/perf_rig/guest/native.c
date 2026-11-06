// Host side of `make check`: gfx3d_main.c and gfx3d.c on the host's own floats.
// gfx3d is built to give bit-identical pixels across targets, so a hash that
// differs from the emulated one means the emulator got an instruction wrong, or
// that a libm function differs (tanf: newlib here, glibc there; README.md).
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "rig_alloc.h"

__asm__(".section .rodata\n.global scene_blob\n.balign 4\nscene_blob:\n.incbin \"" SCENE_BIN "\"\n.text\n");
char __heap_base[64 << 20];
uint16_t __fb_base[320 * 320];

static uint64_t s_fnv = 0xcbf29ce484222325ull;
void native_frame_end(const void *fb) {
  const uint8_t *b = fb;
  for (size_t i = 0; i < sizeof __fb_base; i++) {
    s_fnv ^= b[i];
    s_fnv *= 0x100000001b3ull;
  }
}
static void done(void) { printf("%016" PRIx64 "\n", s_fnv); }
__attribute__((constructor)) static void init(void) { atexit(done); }
