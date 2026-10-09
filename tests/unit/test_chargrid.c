// Host unit tests for the chargrid core (src/os/chargrid*.c).
#include "check.h"
#include "chargrid.h"

#include <stdlib.h>
#include <string.h>

static void test_sizes(void) {
  CHECK(chargrid_check_size(1, 1, 1, 1) == NULL);
  CHECK(chargrid_check_size(1024, 64, 16, 32) == NULL);
  CHECK(chargrid_check_size(256, 256, 6, 8) == NULL);  // exactly 65536 cells
  CHECK(chargrid_check_size(0, 1, 6, 8) != NULL);
  CHECK(chargrid_check_size(1, 1025, 6, 8) != NULL);
  CHECK(chargrid_check_size(257, 256, 6, 8) != NULL);
  CHECK(chargrid_check_size(10, 10, 0, 8) != NULL);
  CHECK(chargrid_check_size(10, 10, 17, 8) != NULL);
  CHECK(chargrid_check_size(10, 10, 6, 33) != NULL);
  CHECK(chargrid_create(0, 1, 6, 8) == NULL);
}

static void test_create_defaults(void) {
  chargrid_t *g = chargrid_create(5, 3, 6, 8);
  CHECK(g != NULL);
  CHECK_EQ_INT(g->cols, 5);
  CHECK_EQ_INT(g->rows, 3);
  CHECK_EQ_INT(g->cell_w, 6);
  CHECK_EQ_INT(g->cell_h, 8);
  for (int i = 0; i < 15; i++) {
    CHECK_EQ_INT(g->glyph[i], 0);
    CHECK_EQ_INT(g->fg[i], CG_DEFAULT_FG);
    CHECK_EQ_INT(g->bg[i], CG_DEFAULT_BG);
  }
  CHECK(g->dirty);
  CHECK_EQ_INT(g->ramp_n, 0);
  CHECK_EQ_INT(g->cramp_n, 0);
  CHECK_EQ_U32(g->palette[9], 0xF800);
  chargrid_free(g);
  chargrid_free(NULL);
}

static void test_palette(void) {
  uint16_t p[256];
  chargrid_default_palette(p);
  CHECK_EQ_U32(p[0], 0x0000);
  CHECK_EQ_U32(p[1], 0x8000);    // maroon 800000
  CHECK_EQ_U32(p[7], 0xC618);    // silver c0c0c0
  CHECK_EQ_U32(p[9], 0xF800);    // red
  CHECK_EQ_U32(p[15], 0xFFFF);   // white
  CHECK_EQ_U32(p[16], 0x0000);   // cube (0,0,0)
  CHECK_EQ_U32(p[21], 0x001F);   // cube (0,0,5) = 0000ff
  CHECK_EQ_U32(p[196], 0xF800);  // cube (5,0,0)
  CHECK_EQ_U32(p[231], 0xFFFF);  // cube (5,5,5)
  CHECK_EQ_U32(p[232], 0x0841);  // grey 080808
  CHECK_EQ_U32(p[255], 0xEF7D);  // grey eeeeee
}

static void test_set_get_fill(void) {
  chargrid_t *g = chargrid_create(4, 3, 6, 8);
  uint8_t c, f, b;
  g->dirty = false;
  chargrid_set(g, 1, 2, 'A', 3, 4);
  CHECK(g->dirty);
  CHECK(chargrid_get(g, 1, 2, &c, &f, &b));
  CHECK_EQ_INT(c, 'A');
  CHECK_EQ_INT(f, 3);
  CHECK_EQ_INT(b, 4);
  chargrid_set(g, 1, 2, 'B', CG_KEEP, CG_KEEP);
  chargrid_get(g, 1, 2, &c, &f, &b);
  CHECK_EQ_INT(c, 'B');
  CHECK_EQ_INT(f, 3);
  CHECK_EQ_INT(b, 4);
  chargrid_set(g, -1, 0, 'X', 1, 1);  // outside: dropped
  chargrid_set(g, 4, 0, 'X', 1, 1);
  chargrid_set(g, 0, 3, 'X', 1, 1);
  CHECK(!chargrid_get(g, 4, 0, &c, &f, &b));
  CHECK_EQ_INT(c, 0);
  CHECK_EQ_INT(f, 0);
  CHECK_EQ_INT(b, 0);
  chargrid_fill(g, -2, -2, 4, 4, '#', CG_KEEP, 9);  // lands on (0..1, 0..1)
  chargrid_get(g, 0, 0, &c, &f, &b);
  CHECK_EQ_INT(c, '#');
  CHECK_EQ_INT(f, CG_DEFAULT_FG);
  CHECK_EQ_INT(b, 9);
  chargrid_get(g, 1, 1, &c, &f, &b);
  CHECK_EQ_INT(c, '#');
  chargrid_get(g, 2, 0, &c, &f, &b);
  CHECK_EQ_INT(c, 0);
  chargrid_fill(g, 0, 0, 1 << 24, 1 << 24, '.', 1, 2);  // huge: the whole grid
  for (int i = 0; i < 12; i++) {
    CHECK_EQ_INT(g->glyph[i], '.');
    CHECK_EQ_INT(g->fg[i], 1);
    CHECK_EQ_INT(g->bg[i], 2);
  }
  chargrid_fill(g, 0, 0, 0, 3, 'Z', 0, 0);   // empty
  chargrid_fill(g, 0, 0, -5, 3, 'Z', 0, 0);  // negative
  chargrid_fill(g, -2000000000, -2000000000, 5, 5, 'Z', 0, 0);  // far away
  CHECK_EQ_INT(g->glyph[0], '.');
  chargrid_free(g);
}

static void test_clip_span(void) {
  chargrid_t *g = chargrid_create(10, 2, 6, 8);
  int first = -1;
  CHECK_EQ_INT(chargrid_clip_span(g, 0, 0, 10, &first), 10);
  CHECK_EQ_INT(first, 0);
  CHECK_EQ_INT(chargrid_clip_span(g, -3, 1, 5, &first), 2);
  CHECK_EQ_INT(first, 3);
  CHECK_EQ_INT(chargrid_clip_span(g, 8, 0, 5, &first), 2);
  CHECK_EQ_INT(first, 0);
  CHECK_EQ_INT(chargrid_clip_span(g, 0, 2, 5, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, 0, -1, 5, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, 10, 0, 5, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, -5, 0, 5, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, 0, 0, 0, &first), 0);
  CHECK_EQ_INT(chargrid_clip_span(g, -2000000000, 0, 2000000010, &first), 10);
  CHECK_EQ_INT(first, 2000000000);
  chargrid_free(g);
}

int main(void) {
  test_sizes();
  test_create_defaults();
  test_palette();
  test_set_get_fill();
  test_clip_span();
  return check_report("test_chargrid");
}
