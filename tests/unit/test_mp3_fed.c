// Host unit tests for mp3_player.c's fed mode, the video player's audio.
//
// Issue #20: every loop of a video restarted its audio (tear the fed
// session down, read 50 chunks from SD, warm the decoder up), ~75 ms of
// silence each time. A looping video now feeds its audio on past the last
// chunk from the first one again and marks the loop point there
// (mp3_player_fed_mark); the decoder plays straight through it, and the
// video restarts when the output reaches it (mp3_player_fed_mark_reached).
// These check the decoder side: libmad decodes across the loop point with
// nothing lost (the bit reservoir included), and the mark reports the
// output's distance from the loop point exactly.
//
// fixtures/mp3/chord.mp3: 1 s of a chord, 44.1 kHz stereo at 96 kbps, 40
// raw MPEG frames as an AVI holds them (no ID3 tag, no Xing frame; the
// reservoir in use from the second frame on), made with
//   ffmpeg -f lavfi -i "aevalsrc=<the chord of test_audio_hw.music_mp3>:s=44100:d=1"
//     -c:a libmp3lame -b:a 96k -write_xing 0 -id3v2_version 0
//     -fflags +bitexact -flags:a +bitexact -f mp3 chord.mp3
// mp3_player_update() stands in for Core 1's tick, mp3_player_mix() for the
// mixer (the device's refill interrupt).
#include "check.h"
#include "mp3_player.h"
#include "audio.h"
#include "pio_psram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX PICODECK_ROOT "/tests/unit/fixtures/mp3/"
#define FIXTURE_FRAMES 40u
#define SAMPLES_PER_FRAME 1152u

// ── What mp3_player.c needs from the rest of the firmware ───────────────────
void hal_sleep_ms(uint32_t ms) { (void)ms; }
void hal_sleep_us(uint64_t us) { (void)us; }
uint64_t hal_get_time_us(void) { return 0; }
// The output is off: fades complete at once (nothing would render them).
bool audio_output_running(void) { return false; }
void audio_output_ensure_running(void) {}
// No PIO PSRAM: the PCM ring comes from umm (the fake, on the host heap).
bool pio_psram_available(void) { return false; }
void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len) {
  (void)addr; (void)dst; (void)len; abort();
}
void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len) {
  (void)addr; (void)src; (void)len; abort();
}

static uint8_t *s_mp3;
static uint32_t s_mp3_len;
static int32_t s_l[128], s_r[128];

static void load_fixture(void) {
  FILE *f = fopen(FIX "chord.mp3", "rb");
  CHECK(f != NULL);
  if (!f) exit(1);
  fseek(f, 0, SEEK_END);
  s_mp3_len = (uint32_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  s_mp3 = malloc(s_mp3_len);
  CHECK(fread(s_mp3, 1, s_mp3_len, f) == s_mp3_len);
  fclose(f);
}

static uint32_t position(void) { return mp3_player_get_position(mp3_player_create()); }

// Core 1's tick and one 128-frame mixer render, until `frames` content
// frames have played or the output stops moving (the fed data ran out).
static uint32_t play_until(uint32_t frames) {
  int idle = 0;
  while (position() < frames && idle < 50) {
    uint32_t before = position();
    mp3_player_update();
    memset(s_l, 0, sizeof s_l);
    memset(s_r, 0, sizeof s_r);
    mp3_player_mix(s_l, s_r, 128);
    idle = position() == before ? idle + 1 : 0;
  }
  return position();
}

static uint32_t diag(int i) {
  uint32_t d[11];
  mp3_player_get_diag(d);
  return d[i];
}
#define DIAG_FRAMES 6
#define DIAG_ERRORS 7

static void start(void) {
  mp3_player_reset_diag();
  CHECK(mp3_player_start_fed(44100, 2));
}

static void feed_pass(void) { CHECK_EQ_U32(mp3_player_feed(s_mp3, s_mp3_len), s_mp3_len); }

static const uint32_t ONE_PASS = FIXTURE_FRAMES * SAMPLES_PER_FRAME;
// The decoder runs at most the PCM ring plus the stage ahead of the output
// (< 12000 stereo frames). Its error count includes a lost sync on an empty
// buffer (the first decode of a session, and every tick once it has
// decoded everything fed), so the tests compare the count after the start
// with the count this far before the end, while there is still data left.
#define BEFORE_THE_END 12000u

// The fixture on its own: every frame decodes, and plays out.
static void test_one_pass(void) {
  start();
  feed_pass();
  mp3_player_start_fed_output();
  uint32_t errors = diag(DIAG_ERRORS);
  play_until(ONE_PASS - BEFORE_THE_END);
  CHECK_EQ_U32(diag(DIAG_ERRORS), errors);
  CHECK_EQ_U32(play_until(UINT32_MAX), ONE_PASS);
  CHECK_EQ_U32(diag(DIAG_FRAMES), FIXTURE_FRAMES);
  mp3_player_stop_fed();
}

// The audio fed twice over with the loop point marked between: the decoder
// plays straight through it, nothing lost, and the mark reports where the
// output is against it, before (negative) and after the loop point.
static void test_plays_through_the_loop_point(void) {
  int32_t late = 12345;
  start();
  feed_pass();
  mp3_player_fed_mark();
  feed_pass();
  mp3_player_start_fed_output();
  uint32_t errors = diag(DIAG_ERRORS);
  CHECK(!mp3_player_fed_mark_reached(&late));   // the decoder is not there yet
  CHECK_EQ_INT(late, 12345);

  // 1000 frames before the loop point the decoder (the PCM ring and the
  // stage ahead of the output) is past it.
  uint32_t pos = play_until(ONE_PASS - 1000);
  CHECK(pos < ONE_PASS);
  CHECK(mp3_player_fed_mark_reached(&late));
  CHECK_EQ_INT(late, (int32_t)(pos - ONE_PASS));
  CHECK(late < 0);
  CHECK(!mp3_player_fed_mark_reached(&late));   // taken

  play_until(2 * ONE_PASS - BEFORE_THE_END);
  CHECK_EQ_U32(diag(DIAG_ERRORS), errors);      // none at the loop point
  CHECK_EQ_U32(play_until(UINT32_MAX), 2 * ONE_PASS);
  CHECK_EQ_U32(diag(DIAG_FRAMES), 2 * FIXTURE_FRAMES);
  mp3_player_stop_fed();
}

// Taken after the output passed the loop point: a positive distance.
static void test_reports_how_far_past(void) {
  int32_t late = 0;
  start();
  feed_pass();
  mp3_player_fed_mark();
  feed_pass();
  mp3_player_start_fed_output();
  uint32_t pos = play_until(ONE_PASS + 3000);
  CHECK(mp3_player_fed_mark_reached(&late));
  CHECK_EQ_INT(late, (int32_t)(pos - ONE_PASS));
  CHECK(late >= 3000);
  mp3_player_stop_fed();
}

// Marked with nothing fed after it: the decoder never gets there.
static void test_unfed_mark_is_never_reached(void) {
  int32_t late;
  start();
  feed_pass();
  mp3_player_fed_mark();
  mp3_player_start_fed_output();
  CHECK_EQ_U32(play_until(UINT32_MAX), ONE_PASS);
  CHECK(!mp3_player_fed_mark_reached(&late));
  mp3_player_stop_fed();
}

// A restart (a seek: start_fed on a running session) and a stop clear it.
static void test_restart_and_stop_clear_the_mark(void) {
  int32_t late;
  start();
  feed_pass();
  mp3_player_fed_mark();
  feed_pass();
  mp3_player_start_fed_output();
  play_until(ONE_PASS + 500);
  CHECK(mp3_player_start_fed(44100, 2));        // restart, mark not taken
  CHECK(!mp3_player_fed_mark_reached(&late));
  feed_pass();                                  // the new session plays as new
  mp3_player_start_fed_output();
  CHECK_EQ_U32(play_until(UINT32_MAX), ONE_PASS);

  CHECK(mp3_player_start_fed(44100, 2));
  feed_pass();
  mp3_player_fed_mark();
  feed_pass();
  mp3_player_start_fed_output();
  play_until(ONE_PASS + 500);
  mp3_player_stop_fed();
  CHECK(!mp3_player_fed_mark_reached(&late));
}

// The mark counts the loop point from the session's start, whatever was
// fed before it in that session (a later loop, or a start mid-file).
static void test_mark_after_a_partial_pass(void) {
  int32_t late;
  start();
  // From frame 10 of the fixture on, then the whole of it (a video started
  // a quarter of the way in, then looping).
  uint32_t off = 0;
  for (int f = 0; f < 10; f++) {
    static const int kbps[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    uint32_t h = (uint32_t)s_mp3[off] << 24 | (uint32_t)s_mp3[off + 1] << 16 |
                 (uint32_t)s_mp3[off + 2] << 8 | s_mp3[off + 3];
    off += 144u * 1000u * (uint32_t)kbps[(h >> 12) & 15] / 44100u + ((h >> 9) & 1);
  }
  CHECK(off < s_mp3_len);
  CHECK_EQ_U32(mp3_player_feed(s_mp3 + off, s_mp3_len - off), s_mp3_len - off);
  mp3_player_fed_mark();
  feed_pass();
  mp3_player_start_fed_output();
  // Frame 10 leans on the reservoir the skipped frames filled: libmad
  // drops it (and maybe the next), so count what the partial pass gave.
  uint32_t pos = play_until(UINT32_MAX);
  uint32_t partial = pos - ONE_PASS;
  CHECK(partial <= (FIXTURE_FRAMES - 10) * SAMPLES_PER_FRAME);
  CHECK(partial >= (FIXTURE_FRAMES - 12) * SAMPLES_PER_FRAME);
  CHECK(mp3_player_fed_mark_reached(&late));
  CHECK_EQ_INT(late, (int32_t)(pos - partial));
  mp3_player_stop_fed();
}

int main(void) {
  load_fixture();
  CHECK(mp3_player_init());
  test_one_pass();
  test_plays_through_the_loop_point();
  test_reports_how_far_past();
  test_unfed_mark_is_never_reached();
  test_restart_and_stop_clear_the_mark();
  test_mark_after_a_partial_pass();
  mp3_player_deinit();
  free(s_mp3);
  return check_report("test_mp3_fed");
}
