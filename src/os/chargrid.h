#pragma once
// Character-cell grids for picocalc.chargrid
// (specs/2026-10-09-chargrid-design.md): glyph / fg / bg planes, a
// 256-entry RGB565 palette, a glyph mask cache, ramps, UTF-8 text and a
// renderer. Pure C (no display, SD card or Lua), so the firmware, the
// simulator and the host unit tests compile chargrid.c, chargrid_shapes.c,
// chargrid_text.c and chargrid_render.c unchanged; chargrid_display.c is the
// one display-facing file.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../fonts/font.h"

#define CG_MAX_DIM 1024     // cols and rows
#define CG_MAX_CELLS 65536  // cols * rows
#define CG_MAX_CELL_W 16
#define CG_MAX_CELL_H 32
#define CG_RAMP_MAX 256
#define CG_KEEP (-1)        // as a glyph or colour argument: leave that plane alone
#define CG_DEFAULT_FG 15    // xterm-256 white
#define CG_DEFAULT_BG 0     // black

enum { CG_PLANE_GLYPH = 0, CG_PLANE_FG = 1, CG_PLANE_BG = 2 };

// The code page. 0x01-0x1B are generated at the grid's cell size
// (chargrid_shapes.c); 0x1C-0x1F are reserved (empty); 0x20-0xFF come from
// the grid's font (a code the font lacks draws its hollow box).
enum {
  CG_EMPTY = 0x00,
  CG_DOT1 = 0x01, CG_DOT2 = 0x02, CG_DOT3 = 0x03, CG_DOT4 = 0x04,
  CG_SHADE1 = 0x05, CG_SHADE2 = 0x06, CG_SHADE3 = 0x07, CG_SHADE4 = 0x08,
  CG_HALF_TOP = 0x09, CG_HALF_BOTTOM = 0x0A, CG_HALF_LEFT = 0x0B, CG_HALF_RIGHT = 0x0C,
  CG_BOX_H = 0x0D, CG_BOX_V = 0x0E, CG_BOX_TL = 0x0F, CG_BOX_TR = 0x10,
  CG_BOX_BL = 0x11, CG_BOX_BR = 0x12, CG_BOX_LT = 0x13, CG_BOX_RT = 0x14,
  CG_BOX_TT = 0x15, CG_BOX_BT = 0x16, CG_BOX_X = 0x17,
  CG_TRI_UP = 0x18, CG_TRI_RIGHT = 0x19, CG_TRI_DOWN = 0x1A, CG_TRI_LEFT = 0x1B,
  CG_SHAPE_LAST = 0x1B,
};

// chargrid_box styles.
enum { CG_STYLE_SINGLE = 0, CG_STYLE_BLOCK = 1, CG_STYLE_ASCII = 2 };

typedef struct {
  int cols, rows;
  int cell_w, cell_h;
  uint8_t *glyph, *fg, *bg;    // cols * rows bytes each, row-major
  uint16_t *masks;             // 256 * cell_h rows; bit c = column c (bit 0 leftmost)
  uint16_t palette[256];       // RGB565, host order
  uint8_t ramp[CG_RAMP_MAX];   // level -> glyph code
  uint8_t cramp[CG_RAMP_MAX];  // tone -> palette index
  uint16_t ramp_n, cramp_n;    // 0 = not set
  bool ramp_dither, cramp_dither;
  bool dirty;                  // written since the last render
  uint8_t defined[32];         // one bit per code set by chargrid_define_glyph
} chargrid_t;

// A render target. The clip rect is inclusive and must lie inside the buffer.
typedef struct {
  uint16_t *fb;
  int stride;  // pixels per row
  int clip_x0, clip_y0, clip_x1, clip_y1;
  bool swap;   // store byte-swapped pixels (the device framebuffer)
} chargrid_target_t;

// ── chargrid.c: the grid ─────────────────────────────────────────────────────

// NULL when the sizes are valid, else a static message naming the limit.
const char *chargrid_check_size(int cols, int rows, int cell_w, int cell_h);
// A grid of empty cells (glyph 0, fg 15, bg 0) with the xterm-256 palette and
// the generated shapes; one allocation. NULL on bad sizes or no memory.
chargrid_t *chargrid_create(int cols, int rows, int cell_w, int cell_h);
void chargrid_free(chargrid_t *g);  // NULL is fine
// The xterm-256 palette as RGB565.
void chargrid_default_palette(uint16_t pal[256]);

static inline uint8_t *chargrid_plane(chargrid_t *g, int plane) {
  return plane == CG_PLANE_GLYPH ? g->glyph : plane == CG_PLANE_FG ? g->fg : g->bg;
}

// Cells. Writes outside the grid are dropped; CG_KEEP leaves a plane alone.
// Every write sets g->dirty.
void chargrid_set(chargrid_t *g, int x, int y, int glyph, int fg, int bg);
// false (and zeros) outside the grid.
bool chargrid_get(const chargrid_t *g, int x, int y, uint8_t *glyph, uint8_t *fg, uint8_t *bg);
void chargrid_fill(chargrid_t *g, int x, int y, int w, int h, int glyph, int fg, int bg);
// The part of the span of n cells at (x, y) inside the grid: returns the
// visible count (0 if none) and sets *first to the span index of the first
// visible cell.
int chargrid_clip_span(const chargrid_t *g, int x, int y, int n, int *first);

// Glyph cache. set_font rebuilds every code not set by define_glyph from the
// shapes and f (NULL: shapes and defined glyphs only). The masks are copies:
// f may be unloaded afterwards.
void chargrid_set_font(chargrid_t *g, const pc_font_t *f);
// rows: cell_h masks, bit (cell_w - 1) the leftmost pixel. Code 1..255.
void chargrid_define_glyph(chargrid_t *g, int code, const uint16_t *rows);

// Ramps: n entries (0 clears). A level or tone v picks entry
// chargrid_ramp_step(v, n, dither, x, y).
void chargrid_set_ramp(chargrid_t *g, const uint8_t *codes, int n, bool dither);
void chargrid_set_color_ramp(chargrid_t *g, const uint8_t *indices, int n, bool dither);
// v clamped to 0..1 (NaN is 0), plus the 4x4 Bayer offset at (x, y) when
// dither; always 0..n-1.
int chargrid_ramp_step(float v, int n, bool dither, int x, int y);
// One cell: level -> glyph through the ramp, tone -> fg through the colour
// ramp. A plane is left alone when its flag is false or its ramp is unset.
void chargrid_shade(chargrid_t *g, int x, int y, float level, bool has_level,
                    float tone, bool has_tone);

// Copies cells [sx, sx+w) x [sy, sy+h) of src to (dx, dy) in dst, all three
// planes, clipped to both grids. src may be dst (overlap-safe). transparent:
// a glyph code whose source cells are skipped, or CG_KEEP for none.
void chargrid_blit(chargrid_t *dst, const chargrid_t *src, int sx, int sy, int w, int h,
                   int dx, int dy, int transparent);
// Shifts the contents by (dx, dy) and fills the exposed cells.
void chargrid_scroll(chargrid_t *g, int dx, int dy, int glyph, int fg, int bg);

// ── chargrid_shapes.c ────────────────────────────────────────────────────────

// Fills rows[0..ch-1] (bit c = column c) with shape `code` (0x01-0x1B) at a
// cw x ch cell; false (rows zeroed) for any other code or size.
bool chargrid_shape(int code, int cw, int ch, uint16_t *rows);

// ── chargrid_text.c ──────────────────────────────────────────────────────────

// Decodes one character from s[0..len): returns the bytes consumed (1-4; 0
// only when len is 0) and its cell code in *code. Mapping: the shape
// characters; ASCII as itself (control bytes included); U+00A0-U+00FF as
// the Latin-1 byte; else '?'. A byte that does not begin a valid UTF-8
// sequence is consumed alone as a raw code.
int chargrid_utf8_code(const char *s, size_t len, uint8_t *code);
// Writes text from (x, y); '\n' continues at (x, y + 1), other bytes below
// 0x20 and 0x7F are skipped. Returns the x after the last character.
int chargrid_print(chargrid_t *g, int x, int y, const char *s, size_t len, int fg, int bg);
// Word-wraps text into rows of w cells from (x, y), at most max_rows rows
// (<= 0: no limit). Returns the rows used; a trailing '\n' adds none.
int chargrid_print_wrapped(chargrid_t *g, int x, int y, int w, int max_rows,
                           const char *s, size_t len, int fg, int bg);
// A w x h frame (nothing when w or h < 2); the inside is untouched.
void chargrid_box(chargrid_t *g, int x, int y, int w, int h, int style, int fg, int bg);
// Multi-line art like print; cells whose code is `transparent` are skipped.
void chargrid_stamp(chargrid_t *g, int x, int y, const char *s, size_t len, int fg, int bg,
                    int transparent);

// ── chargrid_render.c ────────────────────────────────────────────────────────

// Draws cells [sx, sx+w) x [sy, sy+h) with the top-left of cell (sx, sy) at
// pixel (px, py), inside t's clip rect. Returns the cells drawn wholly or in
// part. Clears g->dirty.
int chargrid_render(chargrid_t *g, const chargrid_target_t *t, int px, int py,
                    int sx, int sy, int w, int h);

// ── chargrid_display.c (firmware and simulator only) ─────────────────────────

// chargrid_render into the display's back buffer and clip rect.
int chargrid_draw(chargrid_t *g, int px, int py, int sx, int sy, int w, int h);
