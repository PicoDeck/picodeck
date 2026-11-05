// The 256-colour table, read the two ways the OS reads it: the palette of a new
// chargrid and the terminal's ANSI colour codes. Values are host-order RGB565;
// both modules byte-swap at draw time.
#include "check.h"
#include "chargrid.h"
#include "terminal_parser.h"

// The parser's one call into the renderer, which needs the display.
void terminal_setCursorVisible(bool visible) { (void)visible; }

static chargrid_t *s_grid;
static terminal_t *s_term;
static terminal_parser_t s_parser;

static uint16_t term_fg(const char *fmt, int n) {
  char seq[24];
  uint16_t fg, bg;
  snprintf(seq, sizeof(seq), fmt, n);
  terminal_setColors(s_term, 0x1234, 0x4321);
  terminal_parser_write(&s_parser, seq);
  terminal_getColors(s_term, &fg, &bg);
  CHECK_EQ_U32(bg, 0x4321);  // only the foreground moved
  return fg;
}

static uint16_t term_bg(const char *fmt, int n) {
  char seq[24];
  uint16_t fg, bg;
  snprintf(seq, sizeof(seq), fmt, n);
  terminal_setColors(s_term, 0x1234, 0x4321);
  terminal_parser_write(&s_parser, seq);
  terminal_getColors(s_term, &fg, &bg);
  CHECK_EQ_U32(fg, 0x1234);  // only the background moved
  return bg;
}

static void test_pinned_entries(void) {
  static const struct {
    int index;
    uint16_t rgb565;
  } k_pins[] = {
      {0, 0x0000},    // black
      {1, 0xA800},    // red, 170,0,0
      {7, 0xAD55},    // white, 170,170,170
      {9, 0xFAAA},    // bright red, 255,85,85
      {16, 0x0000},   // cube (0,0,0)
      {231, 0xFFFF},  // cube (5,5,5)
      {232, 0x0841},  // grey 8
      {255, 0xEF7D},  // grey 238
  };
  for (size_t i = 0; i < sizeof(k_pins) / sizeof(k_pins[0]); i++) {
    const int n = k_pins[i].index;
    CHECK_EQ_U32(s_grid->palette[n], k_pins[i].rgb565);
    CHECK_EQ_U32(term_fg("\x1b[38;5;%dm", n), k_pins[i].rgb565);
    CHECK_EQ_U32(term_bg("\x1b[48;5;%dm", n), k_pins[i].rgb565);
  }
}

// The classic ANSI codes are colours 0-15 by another name.
static void test_ansi_codes(void) {
  for (int i = 0; i < 8; i++) {
    CHECK_EQ_U32(term_fg("\x1b[%dm", 30 + i), s_grid->palette[i]);
    CHECK_EQ_U32(term_bg("\x1b[%dm", 40 + i), s_grid->palette[i]);
    CHECK_EQ_U32(term_fg("\x1b[%dm", 90 + i), s_grid->palette[8 + i]);
    CHECK_EQ_U32(term_bg("\x1b[%dm", 100 + i), s_grid->palette[8 + i]);
  }
}

static void test_every_entry_agrees(void) {
  for (int i = 0; i < 256; i++) {
    CHECK_EQ_U32(term_fg("\x1b[38;5;%dm", i), s_grid->palette[i]);
    CHECK_EQ_U32(term_bg("\x1b[48;5;%dm", i), s_grid->palette[i]);
  }
}

static uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// The table is spelled out by hand: check it against the layout's formulas.
static void test_cube_and_greys(void) {
  static const int k_level[6] = {0, 95, 135, 175, 215, 255};
  for (int r = 0; r < 6; r++)
    for (int g = 0; g < 6; g++)
      for (int b = 0; b < 6; b++)
        CHECK_EQ_U32(s_grid->palette[16 + 36 * r + 6 * g + b],
                     rgb565(k_level[r], k_level[g], k_level[b]));
  for (int i = 0; i < 24; i++)
    CHECK_EQ_U32(s_grid->palette[232 + i], rgb565(8 + 10 * i, 8 + 10 * i, 8 + 10 * i));
}

// An index past the table leaves the colour alone.
static void test_out_of_range_index(void) {
  CHECK_EQ_U32(term_fg("\x1b[38;5;%dm", 256), 0x1234);
  CHECK_EQ_U32(term_bg("\x1b[48;5;%dm", 256), 0x4321);
}

int main(void) {
  s_grid = chargrid_create(1, 1, 6, 8);
  s_term = terminal_new(8, 2, 1);
  CHECK(s_grid != NULL);
  CHECK(s_term != NULL);
  terminal_parser_init(&s_parser, s_term);

  test_pinned_entries();
  test_ansi_codes();
  test_every_entry_agrees();
  test_cube_and_greys();
  test_out_of_range_index();

  terminal_free(s_term);
  chargrid_free(s_grid);
  return check_report("test_color256");
}
