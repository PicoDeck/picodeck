// The generated glyphs 0x01-0x1B of the chargrid code page (chargrid.h), at
// any cell size: dots, shades, half blocks, box drawing, triangles.
#include "chargrid.h"

static const uint8_t k_bayer16[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

// Bits [start, start + n).
static uint16_t span_bits(int start, int n) {
  if (n <= 0) return 0;
  return (uint16_t)(((1u << n) - 1u) << start);
}

bool chargrid_shape(int code, int cw, int ch, uint16_t *rows) {
  if (ch >= 1 && ch <= CG_MAX_CELL_H)
    for (int r = 0; r < ch; r++) rows[r] = 0;
  if (code < CG_DOT1 || code > CG_SHAPE_LAST || cw < 1 || ch < 1 || cw > CG_MAX_CELL_W ||
      ch > CG_MAX_CELL_H)
    return false;
  const uint16_t full = span_bits(0, cw);
  const int mx = (cw - 1) / 2, my = (ch - 1) / 2;
  switch (code) {
  case CG_DOT1: case CG_DOT2: case CG_DOT3: case CG_DOT4: {
    const int k = code - CG_DOT1 + 1, m = cw < ch ? cw : ch;
    int d = (m * k + 2) / 4;
    if (d < 1) d = 1;
    const int x0 = (cw - d) / 2, y0 = (ch - d) / 2;
    for (int j = 0; j < d; j++)
      for (int i = 0; i < d; i++) {
        const int dx = 2 * i - (d - 1), dy = 2 * j - (d - 1);
        if (d <= 2 || dx * dx + dy * dy < d * d - 1) rows[y0 + j] |= (uint16_t)(1u << (x0 + i));
      }
    break;
  }
  case CG_SHADE1: case CG_SHADE2: case CG_SHADE3: case CG_SHADE4: {
    const int threshold = (code - CG_SHADE1 + 1) * 4;  // of 16
    for (int r = 0; r < ch; r++)
      for (int c = 0; c < cw; c++)
        if (k_bayer16[(r & 3) * 4 + (c & 3)] < threshold) rows[r] |= (uint16_t)(1u << c);
    break;
  }
  case CG_HALF_TOP:
    for (int r = 0; r < ch / 2; r++) rows[r] = full;
    break;
  case CG_HALF_BOTTOM:
    for (int r = ch / 2; r < ch; r++) rows[r] = full;
    break;
  case CG_HALF_LEFT:
    for (int r = 0; r < ch; r++) rows[r] = span_bits(0, cw / 2);
    break;
  case CG_HALF_RIGHT:
    for (int r = 0; r < ch; r++) rows[r] = span_bits(cw / 2, cw - cw / 2);
    break;
  case CG_TRI_UP: case CG_TRI_DOWN:
    for (int r = 0; r < ch; r++) {
      int span = ((r + 1) * cw + ch - 1) / ch;
      if (span < 1) span = 1;
      rows[code == CG_TRI_UP ? r : ch - 1 - r] = span_bits((cw - span) / 2, span);
    }
    break;
  case CG_TRI_RIGHT: case CG_TRI_LEFT:
    for (int c = 0; c < cw; c++) {
      int span = ((cw - c) * ch + cw - 1) / cw;
      if (span < 1) span = 1;
      const int y0 = (ch - span) / 2, col = code == CG_TRI_RIGHT ? c : cw - 1 - c;
      for (int j = y0; j < y0 + span; j++) rows[j] |= (uint16_t)(1u << col);
    }
    break;
  default: {  // box drawing: which arms leave the centre
    bool left = false, right = false, up = false, down = false;
    switch (code) {
    case CG_BOX_H: left = right = true; break;
    case CG_BOX_V: up = down = true; break;
    case CG_BOX_TL: right = down = true; break;
    case CG_BOX_TR: left = down = true; break;
    case CG_BOX_BL: right = up = true; break;
    case CG_BOX_BR: left = up = true; break;
    case CG_BOX_LT: up = down = right = true; break;
    case CG_BOX_RT: up = down = left = true; break;
    case CG_BOX_TT: left = right = down = true; break;
    case CG_BOX_BT: left = right = up = true; break;
    default: left = right = up = down = true; break;  // CG_BOX_X
    }
    const int c0 = left ? 0 : mx, c1 = right ? cw - 1 : mx;
    rows[my] |= span_bits(c0, c1 - c0 + 1);
    const int r0 = up ? 0 : my, r1 = down ? ch - 1 : my;
    for (int r = r0; r <= r1; r++) rows[r] |= (uint16_t)(1u << mx);
    break;
  }
  }
  return true;
}
