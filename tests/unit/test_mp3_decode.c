// Host unit tests for what mp3_player.c's decoder writes (issue #28).
//
// - Mono: every sample reaches the mixer, in order (the decoder packs a
//   frame's samples and writes the ring once, not once per sample).
//
// fixtures/mp3/chord22m.mp3: 1 s of test_mp3_fed's chord, 22.05 kHz mono
// MPEG-2 at 32 kbps (39 frames of 576 samples, no ID3 tag, no Xing frame),
//   ffmpeg -f lavfi -i "aevalsrc=<the chord>:s=22050:d=1" -ac 1
//     -c:a libmp3lame -b:a 32k -write_xing 0 -id3v2_version 0
//     -fflags +bitexact -flags:a +bitexact -f mp3 chord22m.mp3
#include "check.h"
#include "mp3_player.h"
#include "audio.h"
#include "pio_psram.h"
#include "fakes/sdcard_fake.h"
#define FPM_DEFAULT  // as mp3_player.c
#include "mad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX PICODECK_ROOT "/tests/unit/fixtures/mp3/"

// ── What mp3_player.c needs from the rest of the firmware ───────────────────
static uint64_t s_now_us;  // the clock: moved only by the tests
void hal_sleep_ms(uint32_t ms) { (void)ms; }
void hal_sleep_us(uint64_t us) { (void)us; }
uint64_t hal_get_time_us(void) { return s_now_us; }
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
} blob_t;

static blob_t load_fixture(const char *name) {
  blob_t b = {0};
  char path[512];
  snprintf(path, sizeof path, FIX "%s", name);
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  if (!f) exit(1);
  fseek(f, 0, SEEK_END);
  b.len = (uint32_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  b.data = malloc(b.len);
  CHECK(fread(b.data, 1, b.len, f) == b.len);
  fclose(f);
  return b;
}

static void put_file(const char *path, blob_t b) {
  sdfake_reset();
  sdfake_put(path, (const char *)b.data, b.len);
}

// libmad straight over the whole file: channel 0 of every frame.
static uint32_t reference_decode(blob_t b, int16_t *out, uint32_t cap) {
  uint8_t *buf = calloc(1, b.len + MAD_BUFFER_GUARD);
  memcpy(buf, b.data, b.len);
  struct mad_stream *st = malloc(sizeof *st);
  struct mad_frame *fr = malloc(sizeof *fr);
  struct mad_synth *sy = malloc(sizeof *sy);
  mad_stream_init(st);
  mad_frame_init(fr);
  mad_synth_init(sy);
  mad_stream_buffer(st, buf, b.len + MAD_BUFFER_GUARD);
  uint32_t n = 0;
  for (;;) {
    if (mad_frame_decode(fr, st) != 0) {
      if (MAD_RECOVERABLE(st->error)) continue;
      break;
    }
    mad_synth_frame(sy, fr);
    for (unsigned i = 0; i < sy->pcm.length && n < cap; i++)
      out[n++] = sy->pcm.samplesX[i][0];
  }
  free(st); free(fr); free(sy); free(buf);
  return n;
}

// A mono file: the mixer hears every sample of every frame, in order. At
// the mixer's 44.1 kHz each 22.05 kHz sample plays twice, on both sides;
// the first 64 output frames are the fade-in.
static void test_mono_plays_every_sample_in_order(void) {
  blob_t b = load_fixture("chord22m.mp3");
  static int16_t want[64 * 576];
  uint32_t nwant = reference_decode(b, want, 64 * 576);
  CHECK(nwant >= 36 * 576);

  put_file("/mono.mp3", b);
  mp3_player_t *p = mp3_player_create();
  CHECK(mp3_player_load(p, "/mono.mp3"));
  CHECK_EQ_U32(mp3_player_get_sample_rate(p), 22050);
  CHECK(mp3_player_play(p, 1));
  static int32_t l[128], r[128];
  uint32_t out = 0, bad = 0, first_bad = 0;
  for (int i = 0; i < 2000 && mp3_player_is_playing(p); i++) {
    mp3_player_update();
    memset(l, 0, sizeof l);
    memset(r, 0, sizeof r);
    mp3_player_mix(l, r, 128);
    for (int k = 0; k < 128; k++, out++) {
      if (out < 64 || out / 2 >= nwant) continue;
      if (l[k] != want[out / 2] || r[k] != want[out / 2]) {
        if (!bad) first_bad = out;
        bad++;
      }
    }
  }
  if (bad)
    printf("  mono: %u output frames differ, the first at %u\n", bad, first_bad);
  CHECK_EQ_U32(bad, 0);
  CHECK(out / 2 >= nwant);
  CHECK(!mp3_player_is_playing(p));
  mp3_player_stop(p);
  free(b.data);
}

int main(void) {
  CHECK(mp3_player_init());
  test_mono_plays_every_sample_in_order();
  mp3_player_deinit();
  return check_report("test_mp3_decode");
}
