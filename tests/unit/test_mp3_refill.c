// Host unit tests for mp3_player.c's decode loop at the edges of its input
// buffer: no frame may be lost where a refill ends, from a file or fed.
//
// libmad reads the next frame's header to know how much of this frame's
// data the next one keeps as its bit reservoir. The decoder used to hand it
// the buffer plus MAD_BUFFER_GUARD zero bytes every time, so a frame ending
// exactly at the end of the buffer was decoded against zeros: libmad kept
// no reservoir and the next frame failed (BADDATAPTR) and was skipped, a
// 26 ms jump. The buffer is refilled 4 KB at a time, and a constant bitrate
// comes back to the same alignment: at 128 kbps and 44.1 kHz 49 frames are
// exactly 20480 bytes, five refills, so a frame went every 1.28 s.
//
// fixtures/mp3/chord128.mp3: 3 s of test_mp3_fed's chord at 128 kbps
// (116 frames; frames 49 and 98 start at bytes 20480 and 40960, both using
// the reservoir), made with the same ffmpeg command at -b:a 128k -t 3.
// fixtures/mp3/chord.mp3 (96 kbps, 40 frames) is test_mp3_fed's.
#include "check.h"
#include "mp3_player.h"
#include "audio.h"
#include "pio_psram.h"
#include "fakes/sdcard_fake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX PICODECK_ROOT "/tests/unit/fixtures/mp3/"
#define SAMPLES_PER_FRAME 1152u

void hal_sleep_ms(uint32_t ms) { (void)ms; }
void hal_sleep_us(uint64_t us) { (void)us; }
uint64_t hal_get_time_us(void) { return 0; }
bool audio_output_running(void) { return false; }
void audio_output_ensure_running(void) {}
bool pio_psram_available(void) { return false; }
void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len) {
  (void)addr; (void)dst; (void)len; abort();
}
void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len) {
  (void)addr; (void)src; (void)len; abort();
}

typedef struct {
  uint8_t *data;
  uint32_t len;
  uint32_t frames;
} mp3_t;

static uint32_t frame_len(const uint8_t *p) {
  static const int kbps[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  uint32_t h = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
  if ((h & 0xFFE00000u) != 0xFFE00000u) return 0;
  return 144u * 1000u * (uint32_t)kbps[(h >> 12) & 15] / 44100u + ((h >> 9) & 1);
}

static mp3_t load_fixture(const char *name) {
  mp3_t m = {0};
  char path[512];
  snprintf(path, sizeof path, FIX "%s", name);
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  if (!f) exit(1);
  fseek(f, 0, SEEK_END);
  m.len = (uint32_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  m.data = malloc(m.len);
  CHECK(fread(m.data, 1, m.len, f) == m.len);
  fclose(f);
  for (uint32_t o = 0; o + 4 <= m.len; m.frames++) {
    uint32_t n = frame_len(m.data + o);
    CHECK(n > 0);
    if (!n) break;
    o += n;
  }
  return m;
}

static int32_t s_l[128], s_r[128];

static uint32_t position(void) { return mp3_player_get_position(mp3_player_create()); }

// Core 1's tick and a mixer render, `n` times.
static void tick(int n) {
  for (int i = 0; i < n; i++) {
    mp3_player_update();
    memset(s_l, 0, sizeof s_l);
    memset(s_r, 0, sizeof s_r);
    mp3_player_mix(s_l, s_r, 128);
  }
}

// Until the output stops moving (the input ran out, or the stream ended).
// Returns the furthest position: a stream that ends detaches, back to 0.
static uint32_t play_out(void) {
  int idle = 0;
  uint32_t furthest = position();
  while (idle < 50) {
    uint32_t before = position();
    tick(1);
    idle = position() == before ? idle + 1 : 0;
    if (position() > furthest) furthest = position();
  }
  return furthest;
}

static uint32_t diag(int i) {
  uint32_t d[11];
  mp3_player_get_diag(d);
  return d[i];
}
#define DIAG_FRAMES 6

// A file, read 4 KB at a time: every frame decodes and plays.
static void test_file_loses_no_frame(void) {
  mp3_t m = load_fixture("chord128.mp3");
  CHECK_EQ_U32(m.frames, 116);
  sdfake_reset();
  sdfake_put("/music.mp3", (const char *)m.data, m.len);
  mp3_player_t *p = mp3_player_create();
  mp3_player_reset_diag();
  CHECK(mp3_player_load(p, "/music.mp3"));
  CHECK(mp3_player_play(p, 1));
  CHECK_EQ_U32(play_out(), m.frames * SAMPLES_PER_FRAME);
  CHECK_EQ_U32(diag(DIAG_FRAMES), m.frames);
  CHECK(!mp3_player_is_playing(p));          // finished with the audio
  mp3_player_stop(p);
  free(m.data);
}

// Looping a file: each pass plays whole. Loop on for the first pass, off
// during the second, so the stream ends after exactly two.
static void test_looping_file_loses_no_frame(void) {
  mp3_t m = load_fixture("chord128.mp3");
  sdfake_reset();
  sdfake_put("/music.mp3", (const char *)m.data, m.len);
  mp3_player_t *p = mp3_player_create();
  mp3_player_reset_diag();
  CHECK(mp3_player_load(p, "/music.mp3"));
  mp3_player_set_loop(p, true);
  CHECK(mp3_player_play(p, 0));
  // The decoder (a PCM ring and a stage ahead of the output) has looped
  // once the output is 20 frames into the second pass.
  while (position() < (m.frames + 20) * SAMPLES_PER_FRAME) tick(1);
  mp3_player_set_loop(p, false);
  CHECK_EQ_U32(play_out(), 2 * m.frames * SAMPLES_PER_FRAME);
  CHECK_EQ_U32(diag(DIAG_FRAMES), 2 * m.frames);
  mp3_player_stop(p);
  free(m.data);
}

// Fed as the video player feeds: one frame per chunk, the ring kept full,
// the end marked once the last chunk is in.
static void test_fed_loses_no_frame(void) {
  mp3_t m = load_fixture("chord128.mp3");
  mp3_player_reset_diag();
  CHECK(mp3_player_start_fed(44100, 2));
  uint32_t off = 0, furthest = 0;
  bool started = false;
  int idle = 0;
  while (idle < 50) {
    while (off < m.len && frame_len(m.data + off) <= mp3_player_feed_space()) {
      uint32_t n = frame_len(m.data + off);
      CHECK_EQ_U32(mp3_player_feed(m.data + off, n), n);
      off += n;
    }
    if (off >= m.len) mp3_player_fed_end();
    if (!started) { mp3_player_start_fed_output(); started = true; }
    uint32_t before = position();
    tick(1);
    idle = off >= m.len && position() == before ? idle + 1 : 0;
    if (position() > furthest) furthest = position();
  }
  CHECK_EQ_U32(furthest, m.frames * SAMPLES_PER_FRAME);
  CHECK_EQ_U32(diag(DIAG_FRAMES), m.frames);
  mp3_player_stop_fed();
  free(m.data);
}

// A starved ring: one frame fed at a time, the decoder catching up with
// the feeder each time. It waits for the next frame instead of decoding
// the last one blind, and never stalls: every frame plays.
static void test_fed_frame_by_frame_loses_no_frame(void) {
  mp3_t m = load_fixture("chord.mp3");
  CHECK_EQ_U32(m.frames, 40);
  mp3_player_reset_diag();
  CHECK(mp3_player_start_fed(44100, 2));
  uint32_t off = 0;
  for (uint32_t f = 0; f < m.frames; f++) {
    uint32_t n = frame_len(m.data + off);
    CHECK_EQ_U32(mp3_player_feed(m.data + off, n), n);
    off += n;
    if (f == 0) mp3_player_start_fed_output();
    tick(40);                                   // the decoder catches up
    // Everything but the newest frame, which waits for its successor.
    CHECK_EQ_U32(diag(DIAG_FRAMES), f);
  }
  mp3_player_fed_end();
  CHECK_EQ_U32(play_out(), m.frames * SAMPLES_PER_FRAME);
  CHECK_EQ_U32(diag(DIAG_FRAMES), m.frames);
  mp3_player_stop_fed();
  free(m.data);
}

// Once everything fed has played after the end was marked, the stream is
// over: the mixer counts no underruns, and a restart plays again.
static void test_fed_end_finishes_the_stream(void) {
  mp3_t m = load_fixture("chord.mp3");
  CHECK(mp3_player_start_fed(44100, 2));
  CHECK_EQ_U32(mp3_player_feed(m.data, m.len), m.len);
  mp3_player_fed_end();
  mp3_player_start_fed_output();
  CHECK_EQ_U32(play_out(), m.frames * SAMPLES_PER_FRAME);
  mp3_player_reset_staging_underruns();
  tick(100);
  CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
  CHECK(!mp3_player_is_playing(mp3_player_create()));

  // A seek restarts it, and without an end marked the last frame waits
  // for a successor that may still come.
  CHECK(mp3_player_start_fed(44100, 2));
  CHECK_EQ_U32(mp3_player_feed(m.data, m.len), m.len);
  mp3_player_start_fed_output();
  CHECK_EQ_U32(play_out(), (m.frames - 1) * SAMPLES_PER_FRAME);
  CHECK(mp3_player_is_playing(mp3_player_create()));
  mp3_player_stop_fed();
  free(m.data);
}

// Feeding more after marking the end (loop switched on at the last
// moment) takes the end back while the decoder is still short of it.
static void test_feeding_takes_the_end_back(void) {
  mp3_t m = load_fixture("chord.mp3");
  mp3_player_reset_diag();
  CHECK(mp3_player_start_fed(44100, 2));
  CHECK_EQ_U32(mp3_player_feed(m.data, m.len), m.len);
  mp3_player_fed_end();
  mp3_player_start_fed_output();
  tick(5);
  CHECK_EQ_U32(mp3_player_feed(m.data, m.len), m.len);
  mp3_player_fed_end();
  CHECK_EQ_U32(play_out(), 2 * m.frames * SAMPLES_PER_FRAME);
  mp3_player_stop_fed();
  free(m.data);
}

int main(void) {
  CHECK(mp3_player_init());
  test_file_loses_no_frame();
  test_looping_file_loses_no_frame();
  test_fed_loses_no_frame();
  test_fed_frame_by_frame_loses_no_frame();
  test_fed_end_finishes_the_stream();
  test_feeding_takes_the_end_back();
  mp3_player_deinit();
  return check_report("test_mp3_refill");
}
