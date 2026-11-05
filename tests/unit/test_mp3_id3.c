// Host unit tests for mp3_id3.h: the length of an ID3v2 tag at the front
// of an MP3 file, which mp3_player.c starts and loops past, and what it takes
// for bytes further on to count as a tag (issue #63).
#include "check.h"
#include "mp3_id3.h"

#include <string.h>

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

// ── Further on in the file ──────────────────────────────────────────────────

static void test_a_tag_further_on_is_sized_as_at_the_front(void) {
  uint8_t p[10];
  header(p, 2, 0, 100);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 110);
  header(p, 3, 0, 4534);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 4544);
  header(p, 4, 0, 0x0FFFFFFF);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 10u + 0x0FFFFFFFu);
  header(p, 4, 0x10, 100);                     // with its footer
  CHECK_EQ_U32(mp3_tag_size(p, 10), 120);
  header(p, 3, 0xE0, 100);                     // v2.3's three flags
  CHECK_EQ_U32(mp3_tag_size(p, 10), 110);
  header(p, 4, 0xF0, 100);                     // v2.4's four
  CHECK_EQ_U32(mp3_tag_size(p, 10), 120);
  header(p, 2, 0xC0, 100);                     // v2.2's two
  CHECK_EQ_U32(mp3_tag_size(p, 10), 110);
}

static void test_a_tag_further_on_has_every_field_valid(void) {
  uint8_t p[10];
  header(p, 1, 0, 100);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);        // no ID3v2.1
  header(p, 5, 0, 100);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);        // nor v2.5
  header(p, 0xFF, 0, 100);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);
  header(p, 3, 0, 100);
  p[4] = 0xFF;
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);        // revision
  header(p, 2, 0x20, 100);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);        // a flag v2.2 does not have
  header(p, 3, 0x10, 100);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);        // ...nor v2.3
  header(p, 4, 0x08, 100);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);        // ...nor v2.4
  for (int i = 6; i < 10; i++) {
    header(p, 3, 0, 100);
    p[i] |= 0x80;
    CHECK_EQ_U32(mp3_tag_size(p, 10), 0);      // not a 7-bit size
  }
  header(p, 3, 0, 100);
  CHECK_EQ_U32(mp3_tag_size(p, 9), 0);         // too short to tell
  p[0] = 'i';
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);
}

static void test_a_footer_alone_is_its_ten_bytes(void) {
  uint8_t p[10];
  header(p, 4, 0x10, 100);
  memcpy(p, "3DI", 3);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 10);
  CHECK_EQ_U32(mp3_tag_size(p, 9), 0);
  p[5] = 0;
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);        // a footer says it has one
  header(p, 3, 0x10, 100);
  memcpy(p, "3DI", 3);
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);        // v2.4 only
  header(p, 4, 0x10, 100);
  memcpy(p, "3DI", 3);
  p[9] = 0x80;
  CHECK_EQ_U32(mp3_tag_size(p, 10), 0);
}

static void test_an_id3v1_tag_is_128_bytes(void) {
  uint8_t p[128] = "TAG";
  CHECK_EQ_U32(mp3_tag_size(p, 128), 128);
  CHECK_EQ_U32(mp3_tag_size(p, 3), 128);       // nothing more to tell it by
  CHECK_EQ_U32(mp3_tag_size(p, 2), 0);
  p[2] = 'g';
  CHECK_EQ_U32(mp3_tag_size(p, 128), 0);
}

static void test_a_header_cut_short_is_told_apart(void) {
  uint8_t p[10];
  header(p, 3, 0, 100);
  for (size_t n = 0; n < 10; n++)
    CHECK(mp3_tag_cut_short(p, n));
  CHECK(!mp3_tag_cut_short(p, 10));
  memcpy(p, "3DI", 3);
  CHECK(mp3_tag_cut_short(p, 9));
  memcpy(p, "ID4", 3);
  CHECK(mp3_tag_cut_short(p, 2));              // "ID": it may be
  CHECK(!mp3_tag_cut_short(p, 3));             // "ID4": it is not
  memcpy(p, "TAG", 3);
  CHECK(!mp3_tag_cut_short(p, 8));             // needs no more than this
  static const uint8_t frame[4] = {0xFF, 0xFB, 0x90, 0x64};
  CHECK(!mp3_tag_cut_short(frame, 4));
}

static void test_a_frame_header_is_one_the_decoder_plays(void) {
  static const uint8_t mpeg1[4] = {0xFF, 0xFB, 0x90, 0x64};   // 128 kbps, 44.1 kHz
  static const uint8_t mpeg2[4] = {0xFF, 0xF3, 0x40, 0xC4};   // 32 kbps, 22.05 kHz
  static const uint8_t mpeg25[4] = {0xFF, 0xE3, 0x40, 0xC4};  // MPEG 2.5
  static const uint8_t crc[4] = {0xFF, 0xFA, 0x90, 0x64};     // with a CRC
  static const uint8_t freefmt[4] = {0xFF, 0xFB, 0x00, 0x64}; // free format
  CHECK(mp3_frame_header_ok(mpeg1));
  CHECK(mp3_frame_header_ok(mpeg2));
  CHECK(mp3_frame_header_ok(mpeg25));
  CHECK(mp3_frame_header_ok(crc));
  CHECK(mp3_frame_header_ok(freefmt));
  uint8_t h[4];
  memcpy(h, mpeg1, 4);
  h[0] = 0xFE;
  CHECK(!mp3_frame_header_ok(h));              // no sync word
  memcpy(h, mpeg1, 4);
  h[1] = 0xDB;
  CHECK(!mp3_frame_header_ok(h));              // ...nor here
  memcpy(h, mpeg1, 4);
  h[1] = 0xEB;
  CHECK(!mp3_frame_header_ok(h));              // MPEG version: reserved
  memcpy(h, mpeg1, 4);
  h[1] = 0xFD;
  CHECK(!mp3_frame_header_ok(h));              // Layer II
  memcpy(h, mpeg1, 4);
  h[1] = 0xF9;
  CHECK(!mp3_frame_header_ok(h));              // layer: reserved
  memcpy(h, mpeg1, 4);
  h[2] = 0xF0;
  CHECK(!mp3_frame_header_ok(h));              // bitrate: not allowed
  memcpy(h, mpeg1, 4);
  h[2] = 0x9C;
  CHECK(!mp3_frame_header_ok(h));              // sample rate: reserved
}

static void test_the_stream_goes_on_at_a_frame_or_a_tag(void) {
  static const uint8_t frame[4] = {0xFF, 0xFB, 0x90, 0x64};
  CHECK(mp3_stream_resumes(frame));
  static const uint8_t id3[4] = {'I', 'D', '3', 3};
  CHECK(mp3_stream_resumes(id3));
  static const uint8_t id3_v5[4] = {'I', 'D', '3', 5};
  CHECK(!mp3_stream_resumes(id3_v5));
  static const uint8_t foot[4] = {'3', 'D', 'I', 4};
  CHECK(mp3_stream_resumes(foot));
  static const uint8_t tag[4] = {'T', 'A', 'G', 'x'};
  CHECK(mp3_stream_resumes(tag));
  static const uint8_t text[4] = {'t', 'e', 'x', 't'};
  CHECK(!mp3_stream_resumes(text));
  static const uint8_t zeros[4] = {0, 0, 0, 0};
  CHECK(!mp3_stream_resumes(zeros));
}

int main(void) {
  test_a_tag_is_its_header_and_its_size();
  test_a_v24_footer_adds_ten_bytes();
  test_anything_else_is_no_tag();
  test_a_tag_further_on_is_sized_as_at_the_front();
  test_a_tag_further_on_has_every_field_valid();
  test_a_footer_alone_is_its_ten_bytes();
  test_an_id3v1_tag_is_128_bytes();
  test_a_header_cut_short_is_told_apart();
  test_a_frame_header_is_one_the_decoder_plays();
  test_the_stream_goes_on_at_a_frame_or_a_tag();
  return check_report("test_mp3_id3");
}
