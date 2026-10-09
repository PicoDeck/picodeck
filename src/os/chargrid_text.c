// UTF-8 text on a chargrid (chargrid.h): the character-to-code mapping,
// print, word wrap, boxes and stamped art.
#include "chargrid.h"
#include "text_wrap.h"

typedef struct {
  uint16_t cp;
  uint8_t code;
} cg_map_t;

// Sorted by code point (binary search).
static const cg_map_t k_map[] = {
    {0x00B7, CG_DOT1},        {0x2022, CG_DOT2},
    {0x2500, CG_BOX_H},       {0x2501, CG_BOX_H},       {0x2502, CG_BOX_V},     {0x2503, CG_BOX_V},
    {0x250C, CG_BOX_TL},      {0x250F, CG_BOX_TL},      {0x2510, CG_BOX_TR},    {0x2513, CG_BOX_TR},
    {0x2514, CG_BOX_BL},      {0x2517, CG_BOX_BL},      {0x2518, CG_BOX_BR},    {0x251B, CG_BOX_BR},
    {0x251C, CG_BOX_LT},      {0x2523, CG_BOX_LT},      {0x2524, CG_BOX_RT},    {0x252B, CG_BOX_RT},
    {0x252C, CG_BOX_TT},      {0x2533, CG_BOX_TT},      {0x2534, CG_BOX_BT},    {0x253B, CG_BOX_BT},
    {0x253C, CG_BOX_X},       {0x254B, CG_BOX_X},
    {0x2550, CG_BOX_H},       {0x2551, CG_BOX_V},       {0x2554, CG_BOX_TL},    {0x2557, CG_BOX_TR},
    {0x255A, CG_BOX_BL},      {0x255D, CG_BOX_BR},      {0x2560, CG_BOX_LT},    {0x2563, CG_BOX_RT},
    {0x2566, CG_BOX_TT},      {0x2569, CG_BOX_BT},      {0x256C, CG_BOX_X},
    {0x256D, CG_BOX_TL},      {0x256E, CG_BOX_TR},      {0x256F, CG_BOX_BR},    {0x2570, CG_BOX_BL},
    {0x2580, CG_HALF_TOP},    {0x2584, CG_HALF_BOTTOM}, {0x2588, CG_SHADE4},    {0x258C, CG_HALF_LEFT},
    {0x2590, CG_HALF_RIGHT},  {0x2591, CG_SHADE1},      {0x2592, CG_SHADE2},    {0x2593, CG_SHADE3},
    {0x25B2, CG_TRI_UP},      {0x25B6, CG_TRI_RIGHT},   {0x25BC, CG_TRI_DOWN},  {0x25C0, CG_TRI_LEFT},
    {0x25CF, CG_DOT3},        {0x2B24, CG_DOT4},
};

static uint8_t map_cp(uint32_t cp) {
  int lo = 0, hi = (int)(sizeof k_map / sizeof k_map[0]) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (k_map[mid].cp == cp) return k_map[mid].code;
    if (k_map[mid].cp < cp)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  if (cp >= 0xA0 && cp <= 0xFF) return (uint8_t)cp;
  return '?';
}

int chargrid_utf8_code(const char *s, size_t len, uint8_t *code) {
  const uint8_t *u = (const uint8_t *)s;
  if (len == 0) {
    *code = 0;
    return 0;
  }
  const uint8_t b0 = u[0];
  *code = b0;  // the raw-byte answer for every early return
  if (b0 < 0x80) return 1;
  int n;
  uint32_t cp, min;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    n = 2; cp = b0 & 0x1Fu; min = 0x80;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    n = 3; cp = b0 & 0x0Fu; min = 0x800;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    n = 4; cp = b0 & 0x07u; min = 0x10000;
  } else {
    return 1;
  }
  if ((size_t)n > len) return 1;
  for (int i = 1; i < n; i++) {
    if ((u[i] & 0xC0u) != 0x80u) return 1;
    cp = (cp << 6) | (u[i] & 0x3Fu);
  }
  if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 1;
  *code = map_cp(cp);
  return n;
}

static inline bool skipped(unsigned char b) { return b < 0x20 || b == 0x7F; }

static int put_text(chargrid_t *g, int x, int y, const char *s, size_t len, int fg, int bg,
                    int transparent) {
  int cx = x, cy = y;
  size_t p = 0;
  while (p < len) {
    const unsigned char b = (unsigned char)s[p];
    if (b == '\n') {
      cx = x;
      cy++;
      p++;
      continue;
    }
    if (skipped(b)) {
      p++;
      continue;
    }
    uint8_t c;
    p += (size_t)chargrid_utf8_code(s + p, len - p, &c);
    if ((int)c != transparent) chargrid_set(g, cx, cy, c, fg, bg);
    cx++;
  }
  return cx;
}

int chargrid_print(chargrid_t *g, int x, int y, const char *s, size_t len, int fg, int bg) {
  return put_text(g, x, y, s, len, fg, bg, CG_KEEP);
}

void chargrid_stamp(chargrid_t *g, int x, int y, const char *s, size_t len, int fg, int bg,
                    int transparent) {
  (void)put_text(g, x, y, s, len, fg, bg, transparent);
}

int chargrid_print_wrapped(chargrid_t *g, int x, int y, int w, int max_rows, const char *s,
                           size_t len, int fg, int bg) {
  if (w < 1) return 0;
  if (w > CG_MAX_DIM) w = CG_MAX_DIM;
  char codes[CG_MAX_DIM + 1];
  uint32_t ends[CG_MAX_DIM + 1];  // byte offset just after code k, from the row start
  size_t p = 0;
  int rows = 0;
  while (p < len) {
    if (max_rows > 0 && rows >= max_rows) break;
    int n = 0;
    size_t q = p;
    while (q < len && n <= w) {  // up to w + 1 codes of this paragraph
      const unsigned char b = (unsigned char)s[q];
      if (b == '\n') break;
      if (skipped(b)) {
        q++;
        continue;
      }
      uint8_t c;
      q += (size_t)chargrid_utf8_code(s + q, len - q, &c);
      codes[n] = (char)c;
      ends[n] = (uint32_t)(q - p);
      n++;
    }
    const int take = n > w ? text_wrap_find_break(codes, n, w) : n;
    for (int k = 0; k < take; k++) chargrid_set(g, x + k, y + rows, (uint8_t)codes[k], fg, bg);
    rows++;
    if (take == n) {  // the paragraph's end: step over its newline
      p = q;
      if (p < len && s[p] == '\n') p++;
    } else {
      p += ends[take - 1];
      while (p < len && s[p] == ' ') p++;  // a wrapped row never starts with spaces
      if (p < len && s[p] == '\n') p++;    // the row already broke here
    }
  }
  return rows;
}

void chargrid_box(chargrid_t *g, int x, int y, int w, int h, int style, int fg, int bg) {
  if (w < 2 || h < 2) return;
  int tl = CG_BOX_TL, tr = CG_BOX_TR, bl = CG_BOX_BL, br = CG_BOX_BR, hz = CG_BOX_H,
      vt = CG_BOX_V;
  if (style == CG_STYLE_BLOCK) {
    tl = tr = bl = br = hz = vt = CG_SHADE4;
  } else if (style == CG_STYLE_ASCII) {
    tl = tr = bl = br = '+';
    hz = '-';
    vt = '|';
  }
  const int64_t x1 = (int64_t)x + w - 1, y1 = (int64_t)y + h - 1;
  const bool x1_in = x1 < g->cols, y1_in = y1 < g->rows;
  chargrid_set(g, x, y, tl, fg, bg);
  if (x1_in) chargrid_set(g, (int)x1, y, tr, fg, bg);
  if (y1_in) chargrid_set(g, x, (int)y1, bl, fg, bg);
  if (x1_in && y1_in) chargrid_set(g, (int)x1, (int)y1, br, fg, bg);
  // edges, clipped to the grid so a huge box costs O(grid)
  int64_t i0 = (int64_t)x + 1, i1 = x1;  // [i0, i1)
  if (i0 < 0) i0 = 0;
  if (i1 > g->cols) i1 = g->cols;
  for (int64_t i = i0; i < i1; i++) {
    chargrid_set(g, (int)i, y, hz, fg, bg);
    if (y1_in) chargrid_set(g, (int)i, (int)y1, hz, fg, bg);
  }
  int64_t j0 = (int64_t)y + 1, j1 = y1;
  if (j0 < 0) j0 = 0;
  if (j1 > g->rows) j1 = g->rows;
  for (int64_t j = j0; j < j1; j++) {
    chargrid_set(g, x, (int)j, vt, fg, bg);
    if (x1_in) chargrid_set(g, (int)x1, (int)j, vt, fg, bg);
  }
}
