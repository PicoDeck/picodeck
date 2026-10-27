// Host unit tests for mp3_id3.h: the length of an ID3v2 tag at the front
// of an MP3 file, which mp3_player.c starts and loops past.
#include "check.h"
#include "mp3_id3.h"

static void header(uint8_t *p, uint8_t major, uint8_t flags, uint32_t size) {
  p[0] = 'I'; p[1] = 'D'; p[2] = '3'; p[3] = major; p[4] = 0; p[5] = flags;
  p[6] = (uint8_t)((size >> 21) & 0x7f); p[7] = (uint8_t)((size >> 14) & 0x7f);
  p[8] = (uint8_t)((size >> 7) & 0x7f);  p[9] = (uint8_t)(size & 0x7f);
}

static void test_a_tag_is_its_header_and_its_size(void) {
  uint8_t p[10];
  header(p, 3, 0, 0);
  CHECK_EQ_U32(mp3_id3v2_size(p, 10), 10);
  header(p, 3, 0, 4534);
  CHECK_EQ_U32(mp3_id3v2_size(p, 10), 4544);
  header(p, 4, 0, 0x0FFFFFFF);                 // the largest size field
  CHECK_EQ_U32(mp3_id3v2_size(p, 10), 10u + 0x0FFFFFFFu);
}

static void test_a_v24_footer_adds_ten_bytes(void) {
  uint8_t p[10];
  header(p, 4, 0x10, 100);
  CHECK_EQ_U32(mp3_id3v2_size(p, 10), 120);
  header(p, 3, 0x10, 100);                     // v2.3 has no footer
  CHECK_EQ_U32(mp3_id3v2_size(p, 10), 110);
}

static void test_anything_else_is_no_tag(void) {
  uint8_t p[10];
  header(p, 3, 0, 100);
  CHECK_EQ_U32(mp3_id3v2_size(p, 9), 0);       // too short to tell
  p[0] = 'T';
  CHECK_EQ_U32(mp3_id3v2_size(p, 10), 0);      // "TAG" is ID3v1, at the end
  static const uint8_t frame[10] = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0, 0, 0};
  CHECK_EQ_U32(mp3_id3v2_size(frame, 10), 0);  // an MP3 frame header
  header(p, 0xFF, 0, 100);
  CHECK_EQ_U32(mp3_id3v2_size(p, 10), 0);      // version 0xFF
  header(p, 3, 0, 100);
  p[7] |= 0x80;
  CHECK_EQ_U32(mp3_id3v2_size(p, 10), 0);      // not a 7-bit size
}

int main(void) {
  test_a_tag_is_its_header_and_its_size();
  test_a_v24_footer_adds_ten_bytes();
  test_anything_else_is_no_tag();
  return check_report("test_mp3_id3");
}
