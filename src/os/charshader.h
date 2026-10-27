#pragma once
// The chargrid shader (specs/2026-10-09-chargrid-design.md, "Lua API:
// shader"): a per-cell expression language compiled to bytecode and run
// over a grid's cells in strips. Pure C over chargrid.h and noise.h, so the
// firmware, the simulator and the host tests compile charshader_compile.c
// and charshader_vm.c unchanged. A shader is one allocation, and it keeps
// no pointer to a grid (each run gets its layers).
#include <stdbool.h>
#include <stddef.h>

#include "chargrid.h"

typedef struct charshader charshader_t;

// The outputs, in the order the output stage applies them.
enum {
  CS_OUT_KEEP, CS_OUT_GLYPH, CS_OUT_LEVEL, CS_OUT_FG, CS_OUT_R, CS_OUT_G, CS_OUT_B,
  CS_OUT_TONE, CS_OUT_BG, CS_OUT_BGTONE, CS_OUT_COUNT
};

#define CS_MAX_SOURCE 4096
#define CS_MAX_LAYERS 4
#define CS_MAX_ARRAY_LEN 1024

// Compiles src[0..len). On error returns NULL and writes the message into
// err (NUL-terminated, truncated to err_len): "line L:C: message"; or, with
// no position, "source is longer than 4096 bytes", "out of memory" and the
// limits found after optimisation.
charshader_t *charshader_compile(const char *src, size_t len, char *err, size_t err_len);
void charshader_free(charshader_t *s);  // NULL is fine

// false when the shader declares no uniform called name.
bool charshader_set_uniform(charshader_t *s, const char *name, float v);
// Gives array `name` n entries (0-CS_MAX_ARRAY_LEN) and returns its storage
// for the caller to fill; NULL when there is no such array or n is out of
// range (the array is then unchanged).
float *charshader_array(charshader_t *s, const char *name, int n);
// charshader_array, then a copy of v[0..n).
bool charshader_set_array(charshader_t *s, const char *name, const float *v, int n);
int charshader_layer_count(const charshader_t *s);

// Runs over cells [x, x+w) x [y, y+h) of g, clipped to g, and writes the
// outputs to g's planes, setting g->dirty when the program has any. layers:
// the declared layers in order; one missing (beyond nlayers, or NULL) reads
// 0. Returns the cells run.
int charshader_run(charshader_t *s, chargrid_t *g, int x, int y, int w, int h,
                   chargrid_t *const *layers, int nlayers);
// The same run without writing the grid: stores the final value of output
// `out` (CS_OUT_*) for each cell of the clipped rect, row by row, in values.
// Returns the cells, or -1 when the program never assigns `out`.
int charshader_run_values(charshader_t *s, chargrid_t *g, int x, int y, int w, int h,
                          chargrid_t *const *layers, int nlayers, int out, float *values);
// SRAM a run may borrow for its registers and scratch rows, *bytes long, or
// NULL: the run keeps them in the shader's block. The default (a weak
// symbol) lends none; the firmware lends the video decoder's while no video
// player exists (video_player.cpp). Asked on every run, on the core that runs.
void *charshader_sram(size_t *bytes);

typedef struct {
  int instructions, registers, scalars, uniforms, arrays, layers;
} charshader_info_t;
void charshader_info(const charshader_t *s, charshader_info_t *out);
