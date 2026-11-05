// Host unit tests for what mp3_player.c's decoder writes and when it
// decodes (issue #28).
//
// - Mono: every sample reaches the mixer, in order (the decoder packs a
//   frame's samples and writes the ring once, not once per sample).
// - Decoding ahead: while Core 0 waits in a paced app's frame deadline
//   (os/core0_idle.h), the decoder fills the PCM ring past the half-full
//   mark it otherwise stops at, losing and repeating nothing. Without a
//   window, with decoding ahead switched off, and in fed mode (the video
//   player), it never does.
// - ID3v2 tags: a tagged file plays and loops exactly as the same file
//   untagged, with no decode error: play() and every loop start past the
//   tags (in the first 8 KB read, ending just short of it, longer than it,
//   two in a row), mono and stereo.
// - Tags after the audio or between two streams (issue #63): a tag the file
//   ends with, or one between a stream and the next, costs nothing: the file
//   plays and loops as it would without it, and the tag's bytes are never
//   read. Only a tag that proves itself is skipped: one that lies about its
//   size, or tag-shaped bytes in the audio, loses no audio.
//
// fixtures/mp3/chord22m.mp3: 1 s of test_mp3_fed's chord, 22.05 kHz mono
// MPEG-2 at 32 kbps (39 frames of 576 samples, no ID3 tag, no Xing frame),
//   ffmpeg -f lavfi -i "aevalsrc=<the chord>:s=22050:d=1" -ac 1
//     -c:a libmp3lame -b:a 32k -write_xing 0 -id3v2_version 0
//     -fflags +bitexact -flags:a +bitexact -f mp3 chord22m.mp3
// fixtures/mp3/chord128.mp3: test_mp3_refill's (44.1 kHz stereo, 116 frames).
#include "check.h"
#include "mp3_player.h"
#include "mp3_sched.h"
#include "audio.h"
#include "pio_psram.h"
#include "core0_idle.h"
#include "mp3_id3.h"
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

static uint32_t diag(int i) {
  uint32_t d[11];
  mp3_player_get_diag(d);
  return d[i];
}
#define DIAG_FRAMES 6
#define DIAG_ERRS   7

// Counts taken when a play was stopped: a look at the card past a tag ends an
// update early, so a decoder ahead of the mixer may stand a frame apart (and
// across a loop, the error it counts there). The audio is what must match.
static uint32_t apart(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

static mp3_sched_stats_t sched(void) {
  mp3_sched_stats_t s;
  mp3_player_get_sched_stats(&s);
  return s;
}

// libmad straight over the whole file: channel 0 of every granule.
static uint32_t reference_decode(blob_t b, int16_t *out, uint32_t cap) {
  uint8_t *buf = calloc(1, b.len + MAD_BUFFER_GUARD);
  memcpy(buf, b.data, b.len);
  struct mad_stream *st = malloc(sizeof *st);
  struct mad_frame *fr = malloc(sizeof *fr);
  struct mad_frame_mem *fm = malloc(sizeof *fm);
  struct mad_synth *sy = malloc(sizeof *sy);
  mad_stream_init(st);
  mad_frame_bind(fr, fm);
  mad_frame_init(fr);
  mad_synth_init(sy);
  mad_stream_buffer(st, buf, b.len + MAD_BUFFER_GUARD);
  uint32_t n = 0;
  for (;;) {
    if (mad_frame_decode_begin(fr, st) != 0) {
      if (MAD_RECOVERABLE(st->error)) continue;
      break;
    }
    for (unsigned gr = 0; gr < mad_frame_granules(fr); gr++) {
      if (mad_frame_decode_granule(fr, st, gr) != 0) break;
      mad_synth_granule(sy, fr, gr);
      for (unsigned i = 0; i < sy->pcm.length && n < cap; i++)
        out[n++] = sy->pcm.samplesX[i][0];
    }
    mad_frame_decode_end(fr, st);
  }
  free(st); free(fr); free(fm); free(sy); free(buf);
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
  // Core 1's tick gate (main.c) skips mp3_player_update while this reads idle.
  CHECK(mp3_player_core1_idle());
  CHECK(mp3_player_play(p, 1));
  CHECK(!mp3_player_core1_idle());
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

// Frames decoded by `updates` Core 1 updates after play(), with nothing
// mixed meanwhile (so the ring only fills).
static uint32_t frames_after(int updates) {
  uint32_t before = diag(DIAG_FRAMES);
  for (int i = 0; i < updates; i++)
    mp3_player_update();
  return diag(DIAG_FRAMES) - before;
}

static void start_music(void) {
  mp3_player_t *p = mp3_player_create();
  CHECK(mp3_player_load(p, "/music.mp3"));
  CHECK(mp3_player_play(p, 1));
  mp3_player_reset_sched_stats();
}

static void test_decoding_ahead_needs_core0_idle(void) {
  blob_t b = load_fixture("chord128.mp3");
  put_file("/music.mp3", b);
  mp3_player_t *p = mp3_player_create();

  // Core 0 working: the ring stops filling past half.
  s_now_us = 1000000;
  start_music();
  uint32_t working = frames_after(20);
  CHECK_EQ_U32(sched().idle_frames, 0);
  mp3_player_stop(p);

  // Core 0 idle for the next 20 ms: the decoder fills the ring.
  start_music();
  core0_idle_begin(s_now_us, s_now_us + 20000);
  uint32_t idle = frames_after(20);
  core0_idle_end(s_now_us);
  printf("  frames decoded ahead: %u with Core 0 working, %u idle\n", working, idle);
  CHECK(idle > working);
  CHECK(sched().idle_frames > 0);
  // (The decoder's unit is a granule: two in each of these frames.)
  CHECK_EQ_U32(sched().idle_frames, 2 * (idle - working));
  mp3_player_stop(p);

  // A window shorter than a frame's estimate starts nothing.
  start_music();
  core0_idle_begin(s_now_us, s_now_us + MP3_SCHED_SEED_US - 1);
  CHECK_EQ_U32(frames_after(20), working);
  core0_idle_end(s_now_us);
  mp3_player_stop(p);

  // Switched off (xipstat mp3idle off): as if Core 0 never idled.
  mp3_player_set_decode_ahead(false);
  start_music();
  core0_idle_begin(s_now_us, s_now_us + 20000);
  CHECK_EQ_U32(frames_after(20), working);
  CHECK_EQ_U32(sched().idle_frames, 0);
  core0_idle_end(s_now_us);
  mp3_player_stop(p);
  mp3_player_set_decode_ahead(true);
  free(b.data);
}

// Decoding ahead the whole way through loses and repeats nothing.
static void test_decoding_ahead_loses_no_frame(void) {
  blob_t b = load_fixture("chord128.mp3");
  put_file("/music.mp3", b);
  mp3_player_t *p = mp3_player_create();
  mp3_player_reset_diag();
  start_music();
  core0_idle_begin(s_now_us, s_now_us + 1000000);
  static int32_t l[128], r[128];
  uint32_t furthest = 0;
  for (int i = 0; i < 5000 && mp3_player_is_playing(p); i++) {
    mp3_player_update();
    mp3_player_mix(l, r, 128);
    uint32_t pos = mp3_player_get_position(p);
    if (pos > furthest) furthest = pos;
  }
  core0_idle_end(s_now_us);
  CHECK_EQ_U32(furthest, 116u * 1152u);
  CHECK_EQ_U32(diag(DIAG_FRAMES), 116);
  CHECK(sched().idle_frames > 0);
  CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
  mp3_player_stop(p);
  free(b.data);
}

// The video player's fed session decodes as before, window or not.
static void test_fed_mode_never_decodes_ahead(void) {
  blob_t b = load_fixture("chord128.mp3");
  CHECK(mp3_player_start_fed(44100, 2));
  CHECK_EQ_U32(mp3_player_feed(b.data, b.len), b.len);
  mp3_player_start_fed_output();
  mp3_player_reset_sched_stats();
  core0_idle_begin(s_now_us, s_now_us + 1000000);
  for (int i = 0; i < 20; i++)
    mp3_player_update();
  core0_idle_end(s_now_us);
  CHECK_EQ_U32(sched().idle_frames, 0);
  mp3_player_stop_fed();
  free(b.data);
}

// ID3v2.3 tags of the given body sizes (bodies[0] first, up to 2), whose
// contents look like MP3 frame headers (0xFF 0xF3 ... as in cover art),
// before the file's audio.
static blob_t tagged(blob_t audio, const uint32_t *bodies, int tags) {
  blob_t b;
  b.len = audio.len;
  for (int t = 0; t < tags; t++)
    b.len += 10u + bodies[t];
  b.data = malloc(b.len);
  uint8_t *p = b.data;
  for (int t = 0; t < tags; t++) {
    uint32_t body = bodies[t];
    p[0] = 'I'; p[1] = 'D'; p[2] = '3'; p[3] = 3; p[4] = 0; p[5] = 0;
    p[6] = (uint8_t)((body >> 21) & 0x7f); p[7] = (uint8_t)((body >> 14) & 0x7f);
    p[8] = (uint8_t)((body >> 7) & 0x7f);  p[9] = (uint8_t)(body & 0x7f);
    for (uint32_t i = 0; i < body; i++)
      p[10 + i] = (i % 7 == 0) ? 0xFF : (i % 7 == 1) ? 0xF3 : (uint8_t)(i * 31u);
    p += 10 + body;
  }
  memcpy(p, audio.data, audio.len);
  return b;
}

// Tag sizes around the 8 KB first read (rewind_locked reads 8192 bytes):
// a tag ending just short of it left load()'s probe less than a frame.
#define NEAR_READ(short_by) (8192u - 10u - (short_by))
static const struct { uint32_t bodies[2]; int tags; } TAG_CASES[] = {
  {{4534}, 1},                    // in the first read (Nova Rail's: 4.5 KB)
  {{NEAR_READ(10)}, 1},           // ending 10 bytes before its end
  {{NEAR_READ(50)}, 1},
  {{NEAR_READ(300)}, 1},
  {{20000}, 1},                   // longer than it
  {{20000, 7882}, 2},             // two, the second ending near a read's end
  {{300, 300}, 2},                // two in a row
};
#define TAG_CASE_COUNT (sizeof TAG_CASES / sizeof TAG_CASES[0])

// Plays `path` looping for `renders` mixer renders; the output and the
// decode errors (mp3stats' mad_err).
static uint32_t *s_positions;   // if set: the position after each render
static uint32_t play_looped(const char *path, int32_t *out, int renders) {
  mp3_player_t *p = mp3_player_create();
  mp3_player_reset_diag();
  CHECK(mp3_player_load(p, path));
  mp3_player_set_loop(p, true);
  CHECK(mp3_player_play(p, 0));
  static int32_t l[128], r[128];
  for (int i = 0; i < renders; i++) {
    mp3_player_update();
    memset(l, 0, sizeof l);
    memset(r, 0, sizeof r);
    mp3_player_mix(l, r, 128);
    memcpy(out + i * 128, l, sizeof l);
    if (s_positions)
      s_positions[i] = mp3_player_get_position(p);
  }
  CHECK(mp3_player_is_playing(p));
  mp3_player_stop(p);
  return diag(DIAG_ERRS);
}

static void test_id3v2_tags_are_skipped_at_play_and_at_every_loop(void) {
  blob_t b = load_fixture("chord22m.mp3");   // ~1.07 s: three loops below
  enum { RENDERS = 1100 };
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  put_file("/plain.mp3", b);
  uint32_t errs = play_looped("/plain.mp3", want, RENDERS);
  CHECK(diag(DIAG_FRAMES) > 3 * 39);
  for (size_t c = 0; c < TAG_CASE_COUNT; c++) {
    blob_t t = tagged(b, TAG_CASES[c].bodies, TAG_CASES[c].tags);
    put_file("/tagged.mp3", t);
    uint32_t terrs = play_looped("/tagged.mp3", got, RENDERS);
    CHECK_EQ_U32(terrs, errs);
    CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
    size_t first = SIZE_MAX;
    for (size_t i = 0; i < (size_t)RENDERS * 128; i++)
      if (got[i] != want[i]) { first = i; break; }
    if (first != SIZE_MAX)
      printf("  tags %u,%u: output differs from frame %zu\n",
             (unsigned)TAG_CASES[c].bodies[0], (unsigned)TAG_CASES[c].bodies[1],
             first);
    CHECK(first == SIZE_MAX);
    free(t.data);
  }
  free(b.data);
}

// 44.1 kHz stereo frames are larger (418 B at 128 kbps): the same tags,
// and the file loads and decodes the frames the untagged one does.
static void test_id3v2_tags_before_stereo_frames(void) {
  blob_t b = load_fixture("chord128.mp3");
  enum { RENDERS = 200 };
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  put_file("/plain.mp3", b);
  play_looped("/plain.mp3", want, RENDERS);
  uint32_t frames = diag(DIAG_FRAMES);
  for (size_t c = 0; c < TAG_CASE_COUNT; c++) {
    blob_t t = tagged(b, TAG_CASES[c].bodies, TAG_CASES[c].tags);
    put_file("/tagged.mp3", t);
    play_looped("/tagged.mp3", got, RENDERS);
    CHECK_EQ_U32(diag(DIAG_FRAMES), frames);
    CHECK(memcmp(got, want, sizeof want) == 0);
    free(t.data);
  }
  free(b.data);
}

// ── Tags after the audio and between streams (issue #63) ────────────────────

static void append(blob_t *b, const void *src, uint32_t n) {
  b->data = realloc(b->data, b->len + n);
  memcpy(b->data + b->len, src, n);
  b->len += n;
}

// A tag body as cover art looks to libmad: full of frame headers.
static void add_art(blob_t *b, uint32_t n) {
  b->data = realloc(b->data, b->len + n);
  uint8_t *p = b->data + b->len;
  for (uint32_t i = 0; i < n; i++)
    p[i] = (i % 7 == 0) ? 0xFF : (i % 7 == 1) ? 0xF3 : (uint8_t)(i * 31u);
  b->len += n;
}

// Bytes with no sync word in them: libmad walks them a byte at a time.
static void add_text(blob_t *b, uint32_t n) {
  b->data = realloc(b->data, b->len + n);
  for (uint32_t i = 0; i < n; i++)
    b->data[b->len + i] = (uint8_t)('a' + i % 26);
  b->len += n;
}

// Compressed data, as a JPEG's scan: a sync word every 2 KB or so.
static void add_noise(blob_t *b, uint32_t n) {
  b->data = realloc(b->data, b->len + n);
  uint32_t x = n | 1u;
  for (uint32_t i = 0; i < n; i++) {
    x = x * 1664525u + 1013904223u;
    b->data[b->len + i] = (uint8_t)(x >> 24);
  }
  b->len += n;
}

static void add_id3v2_header(blob_t *b, const char *magic, uint8_t major, uint8_t rev,
                             uint8_t flags, uint32_t size) {
  uint8_t h[10] = {(uint8_t)magic[0], (uint8_t)magic[1], (uint8_t)magic[2], major, rev, flags,
                   (uint8_t)((size >> 21) & 0x7f), (uint8_t)((size >> 14) & 0x7f),
                   (uint8_t)((size >> 7) & 0x7f),  (uint8_t)(size & 0x7f)};
  append(b, h, sizeof h);
}

// An ID3v2 tag: its header, `body` bytes and, for a v2.4 footer flag, the footer.
typedef enum { BODY_ART, BODY_TEXT, BODY_NOISE } body_t;

static void add_id3v2_of(blob_t *b, uint8_t major, uint8_t flags, uint32_t body, body_t kind) {
  add_id3v2_header(b, "ID3", major, 0, flags, body);
  if (kind == BODY_TEXT)
    add_text(b, body);
  else if (kind == BODY_NOISE)
    add_noise(b, body);
  else
    add_art(b, body);
  if (major == 4 && (flags & 0x10))
    add_id3v2_header(b, "3DI", major, 0, flags, body);
}

static void add_id3v2(blob_t *b, uint8_t major, uint8_t flags, uint32_t body) {
  add_id3v2_of(b, major, flags, body, BODY_ART);
}

static void add_id3v1(blob_t *b) {
  uint8_t t[128];
  memset(t, 0, sizeof t);
  memcpy(t, "TAG", 3);
  memcpy(t + 3, "A title", 7);
  t[127] = 17;
  append(b, t, sizeof t);
}

static blob_t copy_of(blob_t b) {
  blob_t c = {0};
  append(&c, b.data, b.len);
  return c;
}

// What can follow a stream.
typedef struct {
  const char *name;
  struct { uint8_t major, flags; uint32_t body; body_t kind; } v2[2];  // major 0: none
  bool v1;                                                // then an ID3v1 tag
  bool lone_footer;                                       // a footer, no header
} tags_t;

#define ART(major, flags, body) {major, flags, body, BODY_ART}
static const tags_t TAGS[] = {
  {.name = "ID3v1", .v1 = true},
  {.name = "v2.3 300", .v2 = {ART(3, 0, 300)}},
  {.name = "v2.3 4534", .v2 = {ART(3, 0, 4534)}},
  {.name = "v2.3 100 KB", .v2 = {ART(3, 0, 100000)}},
  {.name = "v2.3 100 KB of text", .v2 = {{3, 0, 100000, BODY_TEXT}}},
  {.name = "v2.3 100 KB of noise", .v2 = {{3, 0, 100000, BODY_NOISE}}},
  {.name = "v2.2 300", .v2 = {ART(2, 0, 300)}},
  {.name = "v2.4 footer 300", .v2 = {ART(4, 0x10, 300)}},
  {.name = "v2.4 footer 100 KB", .v2 = {ART(4, 0x10, 100000)}},
  {.name = "v2.3 100 KB, ID3v1", .v2 = {ART(3, 0, 100000)}, .v1 = true},
  {.name = "v2.4 footer 4534, ID3v1", .v2 = {ART(4, 0x10, 4534)}, .v1 = true},
  {.name = "two: 20000 and 300", .v2 = {ART(3, 0, 20000), ART(3, 0, 300)}},
  {.name = "a footer alone", .lone_footer = true},
};
#define TAGS_COUNT (sizeof TAGS / sizeof TAGS[0])

static void add_tags(blob_t *b, const tags_t *t) {
  for (int i = 0; i < 2; i++)
    if (t->v2[i].major)
      add_id3v2_of(b, t->v2[i].major, t->v2[i].flags, t->v2[i].body, t->v2[i].kind);
  if (t->lone_footer)
    add_id3v2_header(b, "3DI", 4, 0, 0x10, 300);
  if (t->v1)
    add_id3v1(b);
}

static const char *const STREAMS[] = {"chord22m.mp3", "chord128.mp3"};  // mono, stereo
#define STREAM_COUNT 2

// The file ends with the tag: every pass plays, loops and counts as the file
// without it, in step, and nothing underruns however large the tag.
static void test_a_tag_after_the_audio_costs_nothing(void) {
  enum { RENDERS = 2400 };   // past two loops of the 3 s stream, seven of the 1 s
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  static uint32_t want_pos[RENDERS], got_pos[RENDERS];
  for (int f = 0; f < STREAM_COUNT; f++) {
    blob_t b = load_fixture(STREAMS[f]);
    put_file("/plain.mp3", b);
    s_positions = want_pos;
    uint32_t errs = play_looped("/plain.mp3", want, RENDERS);
    uint32_t frames = diag(DIAG_FRAMES);
    s_positions = got_pos;
    for (size_t c = 0; c < TAGS_COUNT; c++) {
      blob_t t = copy_of(b);
      add_tags(&t, &TAGS[c]);
      put_file("/tagged.mp3", t);
      uint32_t terrs = play_looped("/tagged.mp3", got, RENDERS);
      printf("  %s + %s: %u decode errors (plain %u), %u frames (plain %u), %u underruns\n",
             STREAMS[f], TAGS[c].name, terrs, errs, diag(DIAG_FRAMES), frames,
             mp3_player_staging_underruns());
      CHECK(apart(terrs, errs) <= 1);
      CHECK(apart(diag(DIAG_FRAMES), frames) <= 2);
      CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
      CHECK(memcmp(got, want, sizeof want) == 0);
      // getPosition keeps counting across the loops, in step.
      CHECK(memcmp(got_pos, want_pos, sizeof want_pos) == 0);
      free(t.data);
    }
    s_positions = NULL;
    free(b.data);
  }
}

// A stray byte or three after the last tag (a line end from a script) can hold
// no frame: the tag is still the end of the audio.
static void test_stray_bytes_after_the_last_tag(void) {
  enum { RENDERS = 2400 };
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  blob_t b = load_fixture("chord22m.mp3");
  put_file("/plain.mp3", b);
  uint32_t errs = play_looped("/plain.mp3", want, RENDERS);
  for (uint32_t stray = 1; stray < MP3_RESUME_BYTES; stray++) {
    blob_t t = copy_of(b);
    add_id3v2_of(&t, 3, 0, 100000, BODY_TEXT);
    append(&t, "\r\n\n", stray);
    put_file("/tagged.mp3", t);
    uint32_t terrs = play_looped("/tagged.mp3", got, RENDERS);
    CHECK_EQ_U32(terrs, errs);
    CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
    CHECK(memcmp(got, want, sizeof want) == 0);
    free(t.data);
  }
  free(b.data);
}

// Plays path once: the renders it took, the position at its last render, the
// audio. A finished play can start again from the top.
static int play_once(mp3_player_t *p, int32_t *out, int max_renders, uint32_t *last_pos) {
  static int32_t l[128], r[128];
  int i;
  for (i = 0; i < max_renders && mp3_player_is_playing(p); i++) {
    mp3_player_update();
    memset(l, 0, sizeof l);
    memset(r, 0, sizeof r);
    mp3_player_mix(l, r, 128);
    if (out)
      memcpy(out + i * 128, l, sizeof l);
    if (mp3_player_is_playing(p))
      *last_pos = mp3_player_get_position(p);
  }
  return i;
}

// Not looping, the tag must not hold the end up: isPlaying() goes false on
// the render it does for the plain file. And play() again starts over.
static void test_a_trailing_tag_ends_the_play_on_time(void) {
  enum { RENDERS = 3000 };
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  for (int f = 0; f < STREAM_COUNT; f++) {
    blob_t b = load_fixture(STREAMS[f]);
    mp3_player_t *p = mp3_player_create();
    put_file("/plain.mp3", b);
    CHECK(mp3_player_load(p, "/plain.mp3"));
    mp3_player_set_loop(p, false);
    CHECK(mp3_player_play(p, 1));
    uint32_t pos = 0;
    memset(want, 0, sizeof want);
    int renders = play_once(p, want, RENDERS, &pos);
    CHECK(!mp3_player_is_playing(p));
    mp3_player_stop(p);
    for (size_t c = 0; c < TAGS_COUNT; c++) {
      blob_t t = copy_of(b);
      add_tags(&t, &TAGS[c]);
      put_file("/tagged.mp3", t);
      CHECK(mp3_player_load(p, "/tagged.mp3"));
      for (int again = 0; again < 2; again++) {
        CHECK(mp3_player_play(p, 1));
        uint32_t tpos = 0;
        memset(got, 0, sizeof got);
        int trenders = play_once(p, got, RENDERS, &tpos);
        if (trenders != renders)
          printf("  %s + %s: the end after %d renders, plain %d\n", STREAMS[f],
                 TAGS[c].name, trenders, renders);
        CHECK_EQ_U32((uint32_t)trenders, (uint32_t)renders);
        CHECK_EQ_U32(tpos, pos);
        CHECK(memcmp(got, want, sizeof want) == 0);
      }
      mp3_player_stop(p);
      free(t.data);
    }
    free(b.data);
  }
}

// One stream, a tag, the next (cat a.mp3 b.mp3): the same as the two streams
// with nothing between them. (Each stream's first frame draws on no bit
// reservoir, as any stream's does.)
static void test_a_tag_between_two_streams_costs_nothing(void) {
  enum { RENDERS = 2400 };
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  for (int f = 0; f < STREAM_COUNT; f++) {
    blob_t b = load_fixture(STREAMS[f]);
    blob_t two = copy_of(b);
    append(&two, b.data, b.len);
    put_file("/plain.mp3", two);
    uint32_t errs = play_looped("/plain.mp3", want, RENDERS);
    uint32_t frames = diag(DIAG_FRAMES);
    for (size_t c = 0; c < TAGS_COUNT; c++) {
      blob_t t = copy_of(b);
      add_tags(&t, &TAGS[c]);
      append(&t, b.data, b.len);
      put_file("/tagged.mp3", t);
      uint32_t terrs = play_looped("/tagged.mp3", got, RENDERS);
      printf("  %s, %s, again: %u decode errors (plain %u), %u underruns\n", STREAMS[f],
             TAGS[c].name, terrs, errs, mp3_player_staging_underruns());
      CHECK(apart(terrs, errs) <= 1);
      CHECK(apart(diag(DIAG_FRAMES), frames) <= 2);
      CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
      CHECK(memcmp(got, want, sizeof want) == 0);
      free(t.data);
    }
    free(two.data);
    free(b.data);
  }
}

// Tags in front and behind, between the streams too: a loop goes back to the
// first frame after the tags in front, and loses none of it.
static void test_tags_at_both_ends_loop_from_the_first_frame(void) {
  enum { RENDERS = 2400 };
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  for (int f = 0; f < STREAM_COUNT; f++) {
    blob_t b = load_fixture(STREAMS[f]);
    blob_t two = copy_of(b);
    append(&two, b.data, b.len);
    put_file("/plain.mp3", two);
    uint32_t errs = play_looped("/plain.mp3", want, RENDERS);
    blob_t t = {0};
    add_id3v2(&t, 3, 0, 4534);
    append(&t, b.data, b.len);
    add_id3v2(&t, 4, 0x10, 20000);
    append(&t, b.data, b.len);
    add_id3v2(&t, 3, 0, 100000);
    add_id3v1(&t);
    put_file("/tagged.mp3", t);
    uint32_t terrs = play_looped("/tagged.mp3", got, RENDERS);
    CHECK(apart(terrs, errs) <= 1);
    CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
    CHECK(memcmp(got, want, sizeof want) == 0);
    free(t.data);
    free(two.data);
    free(b.data);
  }
}

// Where each frame starts.
static uint32_t frame_offsets(blob_t b, uint32_t *off, uint32_t cap) {
  uint8_t *buf = calloc(1, b.len + MAD_BUFFER_GUARD);
  memcpy(buf, b.data, b.len);
  struct mad_stream *st = malloc(sizeof *st);
  struct mad_header h;
  mad_stream_init(st);
  mad_stream_buffer(st, buf, b.len + MAD_BUFFER_GUARD);
  uint32_t n = 0;
  while (n < cap) {
    if (mad_header_decode(&h, st) != 0) {
      if (MAD_RECOVERABLE(st->error)) continue;
      break;
    }
    off[n++] = (uint32_t)(st->this_frame - buf);
  }
  free(st);
  free(buf);
  return n;
}

// A tag between two frames of one stream: libmad peeks at what follows a frame
// for the bit reservoir the next frame draws on, and a tag there has none, so
// that next frame or two cannot play (BADDATAPTR), as at any break in a stream.
// The rest does, on time.
static void test_a_tag_inside_a_stream_costs_only_the_reservoir(void) {
  static uint32_t off[256];
  blob_t b = load_fixture("chord128.mp3");
  uint32_t frames = frame_offsets(b, off, 256);
  CHECK_EQ_U32(frames, 116);
  enum { RENDERS = 1100 };
  static int32_t want[RENDERS * 128], got[RENDERS * 128];
  put_file("/plain.mp3", b);
  play_looped("/plain.mp3", want, RENDERS);
  blob_t t = {0};
  append(&t, b.data, off[50]);
  add_id3v2(&t, 3, 0, 100000);
  append(&t, b.data + off[50], b.len - off[50]);
  put_file("/tagged.mp3", t);
  uint32_t errs = play_looped("/tagged.mp3", got, RENDERS);
  uint32_t lost = errs > 1 ? errs - 1 : 0;
  printf("  a tag in one stream: %u frames lost to the reservoir, %u underruns\n", lost,
         mp3_player_staging_underruns());
  CHECK(lost <= 3);
  CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
  // Up to it, the same; from the frames after it on, the same but for the
  // frames lost (each 1152 samples, and the filterbank settling after them).
  uint32_t upto = 50u * 1152u;
  CHECK(memcmp(got, want, upto * sizeof(int32_t)) == 0);
  if (lost <= 3) {
    uint32_t shift = lost * 1152u;
    uint32_t from = upto + (lost + 2) * 1152u;
    uint32_t n = RENDERS * 128u - shift - from;
    CHECK(memcmp(got + from, want + from + shift, n * sizeof(int32_t)) == 0);
  }
  free(t.data);
  free(b.data);
}

// The decode buffer's reads end at 8192 + 4096 k from the audio's start. Tags
// placed so that a read ends inside a tag's header, just after it, or at any
// byte around the tag's end.
static uint32_t read_end_after(uint32_t pos) {
  uint32_t end = 8192;
  while (end < pos + 64)
    end += 4096;
  return end;
}

static void check_plays_as(blob_t t, const int32_t *want, int renders, uint32_t errs,
                           const char *what, int value) {
  static int32_t got[3000 * 128];
  put_file("/tagged.mp3", t);
  uint32_t terrs = play_looped("/tagged.mp3", got, renders);
  bool same = memcmp(got, want, (size_t)renders * 128 * sizeof(int32_t)) == 0;
  if (!same || apart(terrs, errs) > 1 || mp3_player_staging_underruns() != 0)
    printf("  %s %d: %u errors (plain %u), %u underruns, output %s\n", what, value, terrs,
           errs, mp3_player_staging_underruns(), same ? "same" : "differs");
  CHECK(apart(terrs, errs) <= 1);
  CHECK_EQ_U32(mp3_player_staging_underruns(), 0);
  CHECK(same);
}

static void test_tags_cut_by_the_end_of_a_read(void) {
  enum { RENDERS = 2400 };
  static int32_t want_one[RENDERS * 128], want_two[RENDERS * 128];
  for (int f = 0; f < STREAM_COUNT; f++) {
    blob_t b = load_fixture(STREAMS[f]);
    blob_t two = copy_of(b);
    append(&two, b.data, b.len);
    put_file("/plain.mp3", b);
    uint32_t errs_one = play_looped("/plain.mp3", want_one, RENDERS);
    put_file("/plain.mp3", two);
    uint32_t errs_two = play_looped("/plain.mp3", want_two, RENDERS);
    uint32_t end = read_end_after(b.len);

    // A second tag starting cut bytes before the read's end, a first tag
    // padding the file out to it: its header cut, or just inside.
    for (int v1 = 0; v1 < 2; v1++) {
      for (uint32_t cut = 1; cut <= 16; cut++) {
        // ...with the next stream after it, or at the end of the file.
        for (int tail = 0; tail < 2; tail++) {
          blob_t t = copy_of(b);
          add_id3v2(&t, 3, 0, end - cut - b.len - 10);
          if (v1) add_id3v1(&t); else add_id3v2(&t, 3, 0, 300);
          if (!tail) append(&t, b.data, b.len);
          check_plays_as(t, tail ? want_one : want_two, RENDERS,
                         tail ? errs_one : errs_two, v1 ? "ID3v1 cut" : "header cut", (int)cut);
          free(t.data);
        }
      }
    }
    // A tag ending at every byte around the read's end: what follows it is
    // in the buffer, cut by its end, or not read yet.
    for (int d = -16; d <= 16; d++) {
      for (int tail = 0; tail < 2; tail++) {
        blob_t t = copy_of(b);
        add_id3v2(&t, 3, 0, (uint32_t)((int)end + d - (int)b.len - 10));
        if (!tail) append(&t, b.data, b.len);
        check_plays_as(t, tail ? want_one : want_two, RENDERS,
                       tail ? errs_one : errs_two, "tag end from the read's end", d);
        free(t.data);
      }
    }
    free(two.data);
    free(b.data);
  }
}

// Plays path with the mixer pulling a frame at a time: what is heard, the
// gaps left out, so a stall cannot hide what is or is not in the stream.
static uint32_t capture(const char *path, bool loop, int32_t *out, uint32_t cap, int updates) {
  mp3_player_t *p = mp3_player_create();
  mp3_player_reset_diag();
  CHECK(mp3_player_load(p, path));
  mp3_player_set_loop(p, loop);
  CHECK(mp3_player_play(p, loop ? 0 : 1));
  uint32_t n = 0;
  for (int i = 0; i < updates && n < cap && (loop || mp3_player_is_playing(p)); i++) {
    mp3_player_update();
    for (int k = 0; k < 128 && n < cap; k++) {
      int32_t l = 0, r = 0;
      uint32_t under = mp3_player_staging_underruns();
      mp3_player_mix(&l, &r, 1);
      if (mp3_player_staging_underruns() == under)
        out[n++] = l;
    }
  }
  if (!loop)
    CHECK(!mp3_player_is_playing(p));
  mp3_player_stop(p);
  while (n > 0 && out[n - 1] == 0)
    n--;
  return n;
}

// A tag that cannot be believed is bytes to walk, as before the skip: what
// follows it still plays, every frame.
typedef enum {
  LIE_PAST_THE_END,  // a size past the end of the file
  LIE_ONE_PAST,      // ...by one byte
  LIE_INTO_A_FRAME,  // a size ending inside the next stream's first frame
  LIE_INTO_ITS_BODY, // a size ending inside what follows the header
  BAD_MAJOR_5, BAD_MAJOR_1, BAD_REVISION, BAD_FLAGS, BAD_SIZE_BYTE,
  BAD_FOOTER_V3, BAD_FOOTER_NO_FLAG,
  JUNK_ID3,          // "ID3" and a few bytes, into the stream
  JUNK_TAG,          // "TAG", not 128 bytes from anything
  LIE_COUNT
} lie_t;

static void add_lie(blob_t *t, lie_t lie, uint32_t after) {
  enum { JUNK = 400 };
  switch (lie) {
    case LIE_PAST_THE_END:  add_id3v2_header(t, "ID3", 3, 0, 0, 0x0FFFFFFF); break;
    case LIE_ONE_PAST:      add_id3v2_header(t, "ID3", 3, 0, 0, JUNK + after + 1); break;
    case LIE_INTO_A_FRAME:  add_id3v2_header(t, "ID3", 3, 0, 0, JUNK + 50); break;
    case LIE_INTO_ITS_BODY: add_id3v2_header(t, "ID3", 3, 0, 0, JUNK / 2); break;
    case BAD_MAJOR_5:       add_id3v2_header(t, "ID3", 5, 0, 0, JUNK); break;
    case BAD_MAJOR_1:       add_id3v2_header(t, "ID3", 1, 0, 0, JUNK); break;
    case BAD_REVISION:      add_id3v2_header(t, "ID3", 3, 0xFF, 0, JUNK); break;
    case BAD_FLAGS:         add_id3v2_header(t, "ID3", 3, 0, 0x01, JUNK); break;
    case BAD_SIZE_BYTE: {
      add_id3v2_header(t, "ID3", 3, 0, 0, JUNK);
      t->data[t->len - 2] |= 0x80;
      break;
    }
    case BAD_FOOTER_V3:      add_id3v2_header(t, "3DI", 3, 0, 0x10, JUNK); break;
    case BAD_FOOTER_NO_FLAG: add_id3v2_header(t, "3DI", 4, 0, 0, JUNK); break;
    case JUNK_ID3: {
      static const uint8_t j[] = {'I', 'D', '3', 3, 0};
      append(t, j, sizeof j);
      return;   // straight into the next stream's frame header
    }
    case JUNK_TAG: append(t, "TAG", 3); break;
    default: break;
  }
  add_text(t, JUNK);
}

static void test_a_tag_that_lies_loses_no_audio(void) {
  enum { CAP = 600000 };
  static int32_t want[CAP], got[CAP];
  for (int f = 0; f < STREAM_COUNT; f++) {
    blob_t b = load_fixture(STREAMS[f]);
    blob_t two = copy_of(b);
    append(&two, b.data, b.len);
    put_file("/plain.mp3", two);
    uint32_t nwant = capture("/plain.mp3", false, want, CAP, 20000);
    uint32_t frames = diag(DIAG_FRAMES);
    uint32_t errs = diag(DIAG_ERRS);
    for (int lie = 0; lie < LIE_COUNT; lie++) {
      blob_t t = copy_of(b);
      add_lie(&t, (lie_t)lie, b.len);
      append(&t, b.data, b.len);
      put_file("/tagged.mp3", t);
      uint32_t ngot = capture("/tagged.mp3", false, got, CAP, 20000);
      if (ngot != nwant || memcmp(got, want, nwant * sizeof(int32_t)) != 0)
        printf("  %s lie %d: %u frames heard of %u\n", STREAMS[f], lie, ngot, nwant);
      CHECK_EQ_U32(ngot, nwant);
      CHECK(memcmp(got, want, nwant * sizeof(int32_t)) == 0);
      CHECK_EQ_U32(diag(DIAG_FRAMES), frames);
      if (lie != JUNK_ID3)
        CHECK(diag(DIAG_ERRS) >= errs + 400);   // it was walked, not skipped
      free(t.data);
    }
    // At the end of the file: it still ends, and loops, as the plain file.
    put_file("/plain.mp3", b);
    nwant = capture("/plain.mp3", false, want, CAP, 20000);
    for (int lie = 0; lie < LIE_COUNT; lie++) {
      blob_t t = copy_of(b);
      add_lie(&t, (lie_t)lie, 0);
      put_file("/tagged.mp3", t);
      uint32_t ngot = capture("/tagged.mp3", false, got, CAP, 20000);
      CHECK_EQ_U32(ngot, nwant);
      CHECK(memcmp(got, want, nwant * sizeof(int32_t)) == 0);
      free(t.data);
    }
    free(two.data);
    free(b.data);
  }
}

// Tag-shaped bytes inside frames are audio: sizes that would land on a real
// frame, "TAG" where an ID3v1 tag would start, a footer. The decoder never
// looks there; the mixer hears what libmad decodes from the same bytes.
static void test_tag_shaped_audio_is_audio(void) {
  enum { CAP = 300000 };
  static int16_t want[CAP];
  static int32_t got[CAP];
  static uint32_t off[256];
  for (int f = 0; f < STREAM_COUNT; f++) {
    blob_t b = load_fixture(STREAMS[f]);
    uint32_t frames = frame_offsets(b, off, 256);
    CHECK(frames > 30);
    uint32_t in = off[1] - off[0] - 24;   // near a frame's end: past its side information
    blob_t t = copy_of(b);
    // "ID3" + a size landing exactly on frame 15
    uint32_t at = off[10] + in;
    uint32_t size = off[15] - at - 10;
    uint8_t id3[10] = {'I', 'D', '3', 3, 0, 0, 0, 0, (uint8_t)((size >> 7) & 0x7f),
                       (uint8_t)(size & 0x7f)};
    memcpy(t.data + at, id3, sizeof id3);
    // A footer
    static const uint8_t foot[10] = {'3', 'D', 'I', 4, 0, 0x10, 0, 0, 0, 0};
    memcpy(t.data + off[18] + in, foot, sizeof foot);
    // "TAG" with a frame start exactly 128 bytes on
    memcpy(t.data + off[24] - 128, "TAG", 3);
    // "TAG" exactly 128 bytes from the end of the file: inside a frame
    memcpy(t.data + t.len - 128, "TAG", 3);
    uint32_t nwant = reference_decode(t, want, CAP);
    while (nwant > 0 && want[nwant - 1] == 0)
      nwant--;
    put_file("/shaped.mp3", t);
    uint32_t ngot = capture("/shaped.mp3", false, got, CAP, 20000);
    int step = f == 0 ? 2 : 1;   // mono plays each sample twice
    uint32_t bad = 0;
    for (uint32_t i = 64; i < ngot && i / (uint32_t)step < nwant; i++)
      if (got[i] != want[i / (uint32_t)step])
        bad++;
    printf("  %s with tag-shaped audio: %u samples of %u heard, %u differ\n", STREAMS[f],
           ngot / (uint32_t)step, nwant, bad);
    CHECK_EQ_U32(bad, 0);
    CHECK(ngot / (uint32_t)step + 2 >= nwant);
    free(t.data);
    free(b.data);
  }
}

// The skip reads through sdcard_try_fread_at only and waits for nothing: with
// the card taken by Core 0 an update tries once and leaves, and the stream
// comes through whole.
static void test_a_busy_card_holds_the_skip_for_a_tick(void) {
  enum { CAP = 600000 };
  static int32_t want[CAP], got[CAP];
  for (int f = 0; f < STREAM_COUNT; f++) {
    blob_t b = load_fixture(STREAMS[f]);
    blob_t two = copy_of(b);
    append(&two, b.data, b.len);
    put_file("/plain.mp3", two);
    uint32_t nwant = capture("/plain.mp3", false, want, CAP, 20000);

    blob_t t = copy_of(b);
    add_id3v2(&t, 3, 0, 20000);
    append(&t, b.data, b.len);
    add_id3v2(&t, 3, 0, 100000);
    add_id3v1(&t);
    put_file("/tagged.mp3", t);
    mp3_player_t *p = mp3_player_create();
    CHECK(mp3_player_load(p, "/tagged.mp3"));
    mp3_player_set_loop(p, false);
    CHECK(mp3_player_play(p, 1));
    uint32_t n = 0;
    int most_tries = 0;
    for (int i = 0; i < 40000 && mp3_player_is_playing(p); i++) {
      bool busy = (i / 5) % 2 == 0;
      sdfake_set_busy(busy);
      int before = sdfake_try_reads();
      mp3_player_update();
      if (busy && sdfake_try_reads() - before > most_tries)
        most_tries = sdfake_try_reads() - before;
      for (int k = 0; k < 128 && n < CAP; k++) {
        int32_t l = 0, r = 0;
        uint32_t under = mp3_player_staging_underruns();
        mp3_player_mix(&l, &r, 1);
        if (mp3_player_staging_underruns() == under)
          got[n++] = l;
      }
    }
    sdfake_set_busy(false);
    CHECK(!mp3_player_is_playing(p));
    mp3_player_stop(p);
    while (n > 0 && got[n - 1] == 0)
      n--;
    CHECK_EQ_U32((uint32_t)most_tries, 1);
    CHECK_EQ_U32((uint32_t)sdfake_blocking_while_busy(), 0);
    CHECK_EQ_U32(n, nwant);
    CHECK(memcmp(got, want, nwant * sizeof(int32_t)) == 0);
    free(t.data);
    free(two.data);
    free(b.data);
  }
}

// A tag costs a read of what follows it, not of the tag: 1 MB in the middle
// or at the end takes no more reads than the file without it, give or take
// the look past the tag.
static void test_a_big_tag_is_never_read(void) {
  blob_t b = load_fixture("chord128.mp3");
  blob_t two = copy_of(b);
  append(&two, b.data, b.len);
  mp3_player_t *p = mp3_player_create();
  static int32_t l[128], r[128];
  for (int mid = 0; mid < 2; mid++) {
    blob_t plain = mid ? two : b;
    put_file("/plain.mp3", plain);
    CHECK(mp3_player_load(p, "/plain.mp3"));
    mp3_player_set_loop(p, false);
    CHECK(mp3_player_play(p, 1));
    for (int i = 0; i < 40000 && mp3_player_is_playing(p); i++) {
      mp3_player_update();
      mp3_player_mix(l, r, 128);
    }
    int reads = sdfake_try_reads();
    mp3_player_stop(p);

    blob_t t = copy_of(b);
    add_id3v2(&t, 3, 0, 1000000);
    if (mid) append(&t, b.data, b.len);
    put_file("/tagged.mp3", t);
    CHECK(mp3_player_load(p, "/tagged.mp3"));
    mp3_player_set_loop(p, false);
    CHECK(mp3_player_play(p, 1));
    for (int i = 0; i < 40000 && mp3_player_is_playing(p); i++) {
      mp3_player_update();
      mp3_player_mix(l, r, 128);
    }
    printf("  a 1 MB tag %s: %d reads (plain %d)\n", mid ? "mid-file" : "at the end",
           sdfake_try_reads(), reads);
    CHECK(sdfake_try_reads() <= reads + 2);
    CHECK(!mp3_player_is_playing(p));
    mp3_player_stop(p);
    free(t.data);
  }
  free(two.data);
  free(b.data);
}

// Tags one after another count like the errors they replace: an update stops
// after MAX_ERRORS_PER_UPDATE of them, and after one that took a look at the
// card (nothing lets a file of tags hold Core 1 for long).
static void test_tags_in_a_row_cannot_hold_an_update(void) {
  blob_t b = load_fixture("chord128.mp3");
  mp3_player_t *p = mp3_player_create();
  static int32_t l[128], r[128];
  mp3_player_set_loop(p, false);

  // 1000 empty tags in a row: all in the buffer, 32 at most an update. (Idle
  // windows open so that the decoder is free to run.)
  blob_t t = copy_of(b);
  for (int i = 0; i < 1000; i++)
    add_id3v2_header(&t, "ID3", 3, 0, 0, 0);
  append(&t, b.data, b.len);
  put_file("/tagged.mp3", t);
  CHECK(mp3_player_load(p, "/tagged.mp3"));
  mp3_player_reset_diag();
  CHECK(mp3_player_play(p, 1));
  core0_idle_begin(s_now_us, s_now_us + 100000000);
  int stuck = 0;
  uint32_t last = 0;
  for (int i = 0; i < 40000 && mp3_player_is_playing(p); i++) {
    mp3_player_update();
    mp3_player_mix(l, r, 128);
    uint32_t frames = diag(DIAG_FRAMES);
    if (frames == 116u) stuck++;
    last = frames;
  }
  core0_idle_end(s_now_us);
  printf("  1000 empty tags: %d updates between the streams, %u frames\n", stuck, last);
  CHECK(stuck >= 1000 / 32 - 1);
  CHECK_EQ_U32(last, 232);
  mp3_player_stop(p);
  free(t.data);

  // 100 tags of 5 KB in a row, each past the buffer: two reads an update at most.
  t = copy_of(b);
  for (int i = 0; i < 100; i++)
    add_id3v2(&t, 3, 0, 5000);
  append(&t, b.data, b.len);
  put_file("/tagged.mp3", t);
  CHECK(mp3_player_load(p, "/tagged.mp3"));
  mp3_player_reset_diag();
  CHECK(mp3_player_play(p, 1));
  int max_reads = 0;
  for (int i = 0; i < 40000 && mp3_player_is_playing(p); i++) {
    int before = sdfake_try_reads();
    mp3_player_update();
    if (sdfake_try_reads() - before > max_reads)
      max_reads = sdfake_try_reads() - before;
    mp3_player_mix(l, r, 128);
  }
  printf("  100 tags of 5 KB: at most %d reads an update, %u frames\n", max_reads,
         diag(DIAG_FRAMES));
  CHECK(max_reads <= 2);
  CHECK_EQ_U32(diag(DIAG_FRAMES), 232);
  mp3_player_stop(p);
  free(t.data);
  free(b.data);
}

// Fed mode (a video's audio) has no file to seek in and its chunks carry no
// tags: a tag there is bytes to walk, as ever, and what follows it plays.
static void test_fed_mode_leaves_tags_to_libmad(void) {
  blob_t b = load_fixture("chord22m.mp3");
  blob_t t = copy_of(b);
  add_id3v2_header(&t, "ID3", 3, 0, 0, 300);
  add_text(&t, 300);
  append(&t, b.data, b.len);
  sdfake_reset();
  mp3_player_reset_diag();
  CHECK(mp3_player_start_fed(22050, 1));
  CHECK_EQ_U32(mp3_player_feed(t.data, t.len), t.len);
  mp3_player_fed_end();
  mp3_player_start_fed_output();
  static int32_t l[128], r[128];
  for (int i = 0; i < 3000 && mp3_player_is_playing(mp3_player_create()); i++) {
    mp3_player_update();
    mp3_player_mix(l, r, 128);
  }
  CHECK(diag(DIAG_ERRS) >= 300);
  static uint32_t off[64];
  CHECK_EQ_U32(diag(DIAG_FRAMES), 2u * frame_offsets(b, off, 64));
  CHECK_EQ_U32((uint32_t)sdfake_try_reads(), 0);
  mp3_player_stop_fed();
  free(t.data);
  free(b.data);
}

// Files stitched at random from frames, junk full of false sync words and
// tag-shaped headers with sizes at random, some cut short: nothing crashes
// (ASan), nothing blocks, and every play ends.
static uint32_t s_rng = 0x63u;
static uint32_t rnd(uint32_t n) {
  s_rng = s_rng * 1664525u + 1013904223u;
  return (s_rng >> 8) % n;
}

static void test_random_files_finish(void) {
  blob_t b = load_fixture("chord22m.mp3");
  mp3_player_t *p = mp3_player_create();
  static int32_t l[128], r[128];
  for (int run = 0; run < 60; run++) {
    blob_t t = {0};
    if (rnd(4) == 0)
      add_id3v2(&t, 3, 0, rnd(3000));
    append(&t, b.data, b.len);        // (load() wants a frame first)
    uint32_t parts = rnd(8);
    for (uint32_t i = 0; i < parts; i++) {
      switch (rnd(6)) {
        case 0: append(&t, b.data, b.len); break;
        case 1: {   // part of the stream, from anywhere
          uint32_t from = rnd(b.len);
          append(&t, b.data + from, rnd(b.len - from) + 1);
          break;
        }
        case 2: add_art(&t, rnd(6000)); break;
        case 3: add_id3v2(&t, (uint8_t)(2 + rnd(3)), rnd(2) ? 0x10 : 0, rnd(12000)); break;
        case 4:
          add_id3v2_header(&t, rnd(2) ? "ID3" : "3DI", (uint8_t)rnd(6), (uint8_t)rnd(2),
                           (uint8_t)(rnd(2) ? 0x10 : rnd(256)), rnd(1u << 24));
          break;
        default:
          if (rnd(2)) add_id3v1(&t);
          else append(&t, "TAG", 3);
          break;
      }
    }
    put_file("/random.mp3", t);
    CHECK(mp3_player_load(p, "/random.mp3"));
    mp3_player_set_loop(p, rnd(2) != 0);
    CHECK(mp3_player_play(p, 1));
    int i;
    for (i = 0; i < 6000 && mp3_player_is_playing(p); i++) {
      sdfake_set_busy(rnd(4) == 0);
      mp3_player_update();
      mp3_player_mix(l, r, 128);
    }
    sdfake_set_busy(false);
    mp3_player_set_loop(p, false);
    for (i = 0; i < 40000 && mp3_player_is_playing(p); i++) {
      mp3_player_update();
      mp3_player_mix(l, r, 128);
    }
    CHECK(!mp3_player_is_playing(p));
    CHECK_EQ_U32((uint32_t)sdfake_blocking_while_busy(), 0);
    mp3_player_stop(p);
    free(t.data);
  }
  free(b.data);
}

// A player whose init could not allocate the decoder (umm full): create()
// still hands it out, and load() and play() refuse instead of using the
// missing buffers.
void umm_fake_fail(bool fail);
static void test_a_player_without_its_decoder_refuses_to_load(void) {
  blob_t b = load_fixture("chord22m.mp3");
  put_file("/plain.mp3", b);
  mp3_player_deinit();
  umm_fake_fail(true);
  mp3_player_t *p = mp3_player_create();
  CHECK(p != NULL);
  CHECK(!mp3_player_load(p, "/plain.mp3"));
  CHECK(!mp3_player_play(p, 1));
  mp3_player_update();
  umm_fake_fail(false);
  CHECK(mp3_player_init());
  CHECK(mp3_player_load(p, "/plain.mp3"));
  mp3_player_stop(p);
  free(b.data);
}

int main(void) {
  CHECK(mp3_player_init());
  test_mono_plays_every_sample_in_order();
  test_decoding_ahead_needs_core0_idle();
  test_decoding_ahead_loses_no_frame();
  test_fed_mode_never_decodes_ahead();
  test_id3v2_tags_are_skipped_at_play_and_at_every_loop();
  test_id3v2_tags_before_stereo_frames();
  test_a_tag_after_the_audio_costs_nothing();
  test_stray_bytes_after_the_last_tag();
  test_a_trailing_tag_ends_the_play_on_time();
  test_a_tag_between_two_streams_costs_nothing();
  test_tags_at_both_ends_loop_from_the_first_frame();
  test_a_tag_inside_a_stream_costs_only_the_reservoir();
  test_tags_cut_by_the_end_of_a_read();
  test_a_tag_that_lies_loses_no_audio();
  test_tag_shaped_audio_is_audio();
  test_a_busy_card_holds_the_skip_for_a_tick();
  test_a_big_tag_is_never_read();
  test_tags_in_a_row_cannot_hold_an_update();
  test_fed_mode_leaves_tags_to_libmad();
  test_random_files_finish();
  test_a_player_without_its_decoder_refuses_to_load();
  mp3_player_deinit();
  return check_report("test_mp3_decode");
}
