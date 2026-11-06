// The terminal's ANSI parser (src/os/terminal_parser.c) over the real terminal:
// what a NUL byte does. A terminal ignores it wherever it comes, inside an escape
// sequence too (the DEC parser "executes" it, and that is nothing), and a server's
// output can hold them.
#include "check.h"
#include "terminal_parser.h"

// The parser's one call into the renderer, which needs the display.
void terminal_setCursorVisible(bool visible) { (void)visible; }

static terminal_t *s_term;
static terminal_parser_t s_parser;

static void reset(void) {
  terminal_clear(s_term);
  terminal_parser_init(&s_parser, s_term);
}

static void parse(const char *data, int len) {
  terminal_parser_parse(&s_parser, data, len);
}

#define PARSE(lit) parse((lit), (int)sizeof(lit) - 1)

static void check_row(int y, const char *want) {
  for (int x = 0; want[x]; x++)
    CHECK_EQ_U32(terminal_getCell(s_term, x, y) & 0xFF, (uint8_t)want[x]);
}

static void check_cursor(int want_x, int want_y) {
  int x, y;
  terminal_getCursor(s_term, &x, &y);
  CHECK_EQ_INT(x, want_x);
  CHECK_EQ_INT(y, want_y);
}

static void test_nul_in_text_is_nothing(void) {
  reset();
  PARSE("AB\0CD");
  check_row(0, "ABCD");
  check_cursor(4, 0);

  reset();
  PARSE("\0\0\0");
  check_cursor(0, 0);
}

static void test_nul_inside_a_sequence_is_nothing(void) {
  uint16_t fg, bg;

  // After the ESC, it must not end the sequence and print "[31m".
  reset();
  terminal_setColors(s_term, 0x1234, 0x4321);
  PARSE("\x1b\0[31mX");
  terminal_getColors(s_term, &fg, &bg);
  CHECK(fg != 0x1234);
  check_row(0, "X");
  check_cursor(1, 0);

  // Among the parameters.
  reset();
  terminal_setColors(s_term, 0x1234, 0x4321);
  PARSE("\x1b[3\0" "1mY");
  terminal_getColors(s_term, &fg, &bg);
  CHECK(fg != 0x1234);
  check_row(0, "Y");
  check_cursor(1, 0);

  // In an OSC, up to its end.
  reset();
  PARSE("\x1b]0;ti\0tle\x07Z");
  check_row(0, "Z");
  check_cursor(1, 0);
}

// The C-string entry is the native API's, and stops at the first NUL as ever.
static void test_c_string_entry_stops_at_a_nul(void) {
  reset();
  terminal_parser_write(&s_parser, "AB\0CD");
  check_row(0, "AB");
  check_cursor(2, 0);
}

int main(void) {
  s_term = terminal_new(20, 4, 1);
  CHECK(s_term != NULL);
  if (!s_term)
    return check_report("test_terminal_parser");
  test_nul_in_text_is_nothing();
  test_nul_inside_a_sequence_is_nothing();
  test_c_string_entry_stops_at_a_nul();
  terminal_free(s_term);
  return check_report("test_terminal_parser");
}
