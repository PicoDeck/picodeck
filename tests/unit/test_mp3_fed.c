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
// A seek restarts the session at the new position. Through start_fed the
// old audio fades out first, then the decoder starts on the new while the
// mixer has nothing: the pre-roll decode (and the s_mp3_mutex waits) are
// silence. mp3_player_restart_fed decodes the new audio's first frame
// while the old plays on, then fades the old out and the new in. Mid-
// stream, a decoder started from nothing also plays its first frame as a
// ramp up from silence (~240 zero samples here: the synthesis filterbank
// and the IMDCT overlap start empty), so the restart decodes one frame more
// first and does not play it. The restart tests model the device's time:
// the output renders 128 frames whenever Core 0 waits in a fade (sleep_us)
// and after every frame the decoder synthesises (a wrapped
// mad_synth_frame), and the longest run of silent output frames around
// the restart is the gap.
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
#include "pcm_stage.h"
#include "pio_psram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX PICODECK_ROOT "/tests/unit/fixtures/mp3/"
#define FIXTURE_FRAMES 40u
#define SAMPLES_PER_FRAME 1152u

// ── The output and the clock, modelled ──────────────────────────────────────
// Off (the default), the output renders only when a test plays
// (play_until), and fades complete at once (nothing would render them). On,
// it also renders while Core 0 waits in a fade, and while the decoder
// synthesises a frame (s_renders_per_decode renders each: the decode's
// time). Every render is one refill interrupt: 128 frames of the MP3
// source, 2902 us of the clock, and, while capturing, the frames kept.
#define RENDER_FRAMES 128
#define RENDER_US 2902u
static bool s_output_on;
static int s_renders_per_decode;
static uint64_t s_now_us;
static int32_t s_l[RENDER_FRAMES], s_r[RENDER_FRAMES];
static bool s_capturing;
static int32_t *s_cap;            // captured frames, L and R interleaved
static uint32_t s_cap_frames, s_cap_size;

static void render(void) {
  memset(s_l, 0, sizeof s_l);
  memset(s_r, 0, sizeof s_r);
  mp3_player_mix(s_l, s_r, RENDER_FRAMES);
  s_now_us += RENDER_US;
  if (!s_capturing) return;
  if (s_cap_frames + RENDER_FRAMES > s_cap_size) {
    s_cap_size = s_cap_size ? s_cap_size * 2 : 65536;
    s_cap = realloc(s_cap, (size_t)s_cap_size * 2 * sizeof *s_cap);
    if (!s_cap) abort();
  }
  for (int i = 0; i < RENDER_FRAMES; i++) {
    s_cap[2 * (s_cap_frames + i)] = s_l[i];
    s_cap[2 * (s_cap_frames + i) + 1] = s_r[i];
  }
  s_cap_frames += RENDER_FRAMES;
}

// Linked with --wrap=mad_synth_frame: mp3_player.c's decoder lands here.
struct mad_synth;
struct mad_frame;
void __real_mad_synth_frame(struct mad_synth *synth, struct mad_frame const *frame);
void __wrap_mad_synth_frame(struct mad_synth *synth, struct mad_frame const *frame) {
  __real_mad_synth_frame(synth, frame);
  for (int i = 0; s_output_on && i < s_renders_per_decode; i++)
    render();
}

// ── What mp3_player.c needs from the rest of the firmware ───────────────────
void hal_sleep_ms(uint32_t ms) { (void)ms; }
void hal_sleep_us(uint64_t us) {
  (void)us;
  if (s_output_on) render();
}
uint64_t hal_get_time_us(void) { return s_now_us; }
bool audio_output_running(void) { return s_output_on; }
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
// frames have played or the output stops moving (the fed data ran out, or
// the stream ended). Returns the furthest position: a stream that ends
// detaches, and its position goes back to 0.
static uint32_t play_until(uint32_t frames) {
  int idle = 0;
  uint32_t furthest = position();
  while (furthest < frames && idle < 50) {
    uint32_t before = position();
    mp3_player_update();
    render();
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

// The fixture on its own, its end marked: every frame decodes, and plays.
static void test_one_pass(void) {
  start();
  feed_pass();
  mp3_player_fed_end();
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
  mp3_player_fed_end();
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
  mp3_player_fed_end();
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
  mp3_player_fed_end();
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

// The byte offset of the fixture's frame n (as an AVI's audio chunk n).
static uint32_t frame_offset(int n) {
  static const int kbps[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  uint32_t off = 0;
  for (int f = 0; f < n; f++) {
    uint32_t h = (uint32_t)s_mp3[off] << 24 | (uint32_t)s_mp3[off + 1] << 16 |
                 (uint32_t)s_mp3[off + 2] << 8 | s_mp3[off + 3];
    off += 144u * 1000u * (uint32_t)kbps[(h >> 12) & 15] / 44100u + ((h >> 9) & 1);
  }
  return off;
}

// The mark counts the loop point from the session's start, whatever was
// fed before it in that session (a later loop, or a start mid-file).
static void test_mark_after_a_partial_pass(void) {
  int32_t late;
  start();
  // From frame 10 of the fixture on, then the whole of it (a video started
  // a quarter of the way in, then looping).
  uint32_t off = frame_offset(10);
  CHECK(off < s_mp3_len);
  CHECK_EQ_U32(mp3_player_feed(s_mp3 + off, s_mp3_len - off), s_mp3_len - off);
  mp3_player_fed_mark();
  feed_pass();
  mp3_player_fed_end();
  mp3_player_start_fed_output();
  // Past the loop point (the partial pass is at most 30 frames), take the
  // mark; then play out to learn how long the partial pass was. Frame 10
  // leans on the reservoir the skipped frames filled: libmad drops it (and
  // maybe the next).
  uint32_t at = play_until((FIXTURE_FRAMES - 10) * SAMPLES_PER_FRAME + 5000);
  CHECK(mp3_player_fed_mark_reached(&late));
  uint32_t partial = play_until(UINT32_MAX) - ONE_PASS;
  CHECK(partial <= (FIXTURE_FRAMES - 10) * SAMPLES_PER_FRAME);
  CHECK(partial >= (FIXTURE_FRAMES - 12) * SAMPLES_PER_FRAME);
  CHECK_EQ_INT(late, (int32_t)(at - partial));
  mp3_player_stop_fed();
}

// ── Restarts: a seek ────────────────────────────────────────────────────────

static void capture_from_scratch(void) {
  s_cap_frames = 0;
  s_capturing = true;
}

// The longest run of silent output frames in the capture.
static uint32_t longest_silence(void) {
  uint32_t run = 0, longest = 0;
  for (uint32_t i = 0; i < s_cap_frames; i++) {
    if (s_cap[2 * i] == 0 && s_cap[2 * i + 1] == 0) {
      if (++run > longest) longest = run;
    } else {
      run = 0;
    }
  }
  return longest;
}

// Reads and resets the restart diagnostics.
static mp3_fed_restart_stats_t restart_stats(void) {
  mp3_fed_restart_stats_t st;
  mp3_player_fed_restart_stats(&st, true);
  return st;
}

// A video's audio playing, 0.5 s in, two passes of the fixture fed, the
// output on, and each frame's decode as long as a render (~2.9 ms: less
// than the device takes). A seek lands here.
static void playing_session(void) {
  s_output_on = true;
  s_renders_per_decode = 1;
  start();
  feed_pass();
  feed_pass();
  mp3_player_start_fed_output();
  play_until(22050);
  restart_stats();
}

// A seek to frame 10 (a chunk mid-stream: libmad drops its first frame,
// whose bit reservoir it never saw, and plays the next as a ramp up from
// silence unless the restart primes it). In place, the old audio plays on
// while the new audio's first frames decode: the output is silent only
// between the old audio's fade-out and the new audio's fade-in, the tail
// of the render that ends the fade-out, and nothing underruns. Unprimed,
// the decoder's ramp adds more than a render of silence. Through
// start_fed (the fallback), the output is silent from the fade-out through
// the whole pre-roll decode as well (three frames, three renders here)
// and the decoder's ramp.
static void test_restart_in_place_leaves_no_gap(void) {
  uint32_t off = frame_offset(10);
  CHECK(off < s_mp3_len);

  playing_session();
  uint32_t underruns = mp3_player_staging_underruns();
  capture_from_scratch();
  render();
  CHECK(mp3_player_restart_fed(s_mp3 + off, s_mp3_len - off, true));
  CHECK_EQ_U32(position(), 0);                  // counts from the new audio
  play_until(10000);
  uint32_t in_place = longest_silence();
  CHECK(in_place >= PCM_STAGE_FADE_FRAMES);
  CHECK(in_place < RENDER_FRAMES);
  CHECK_EQ_U32(mp3_player_staging_underruns(), underruns);
  mp3_fed_restart_stats_t st = restart_stats();
  CHECK_EQ_U32(st.restarts, 1);
  CHECK_EQ_U32(st.fallbacks, 0);
  CHECK_EQ_U32(st.gap_us, 0);                   // the fade-in's render is the next
  CHECK_EQ_U32(st.gap_max_us, 0);
  // Two frames decoded (the primer and the first heard), the old audio
  // playing, which had more than that left.
  CHECK_EQ_U32(st.preroll_max_us, 2 * RENDER_US);
  CHECK(st.margin_min_us > st.preroll_max_us);
  mp3_player_stop_fed();

  playing_session();                            // unprimed
  capture_from_scratch();
  render();
  CHECK(mp3_player_restart_fed(s_mp3 + off, s_mp3_len - off, false));
  play_until(10000);
  uint32_t unprimed = longest_silence();
  CHECK(unprimed >= PCM_STAGE_FADE_FRAMES + RENDER_FRAMES);
  CHECK_EQ_U32(restart_stats().preroll_max_us, RENDER_US);
  mp3_player_stop_fed();

  playing_session();
  capture_from_scratch();
  render();
  CHECK(mp3_player_start_fed(44100, 2));
  CHECK_EQ_U32(mp3_player_feed(s_mp3 + off, s_mp3_len - off), s_mp3_len - off);
  mp3_player_start_fed_output();
  play_until(10000);
  uint32_t fallback = longest_silence();
  CHECK(fallback >= PCM_STAGE_FADE_FRAMES + 3 * RENDER_FRAMES);
  printf("seek gaps, silent output frames: in place %u, unprimed %u, "
         "through start_fed %u\n", (unsigned)in_place, (unsigned)unprimed,
         (unsigned)fallback);
  st = restart_stats();
  CHECK_EQ_U32(st.restarts, 0);
  CHECK_EQ_U32(st.fallbacks, 1);
  CHECK_EQ_U32(st.gap_us, 3 * RENDER_US);
  CHECK_EQ_U32(st.gap_max_us, 3 * RENDER_US);
  mp3_player_stop_fed();
  s_capturing = false;
  s_output_on = false;
}

// A restart in place plays exactly what a session started on the same
// data plays after its first frame: the new audio, primed, from the frame
// after the one the decoder starts on, nothing of the old audio in it,
// nothing dropped or repeated. (Compared past the fade-in's ramp.) And
// unprimed, from its first frame: the session's start.
static void test_restart_in_place_plays_the_new_audio_whole(void) {
  const uint32_t n = 40 * RENDER_FRAMES;
  const uint32_t ramp = PCM_STAGE_FADE_FRAMES;
  const uint32_t offs[2] = {frame_offset(10), 0};

  for (int primed = 1; primed >= 0; primed--) {
    uint32_t off = offs[primed ? 0 : 1];
    uint32_t skip = primed ? SAMPLES_PER_FRAME : 0;

    s_output_on = true;
    s_renders_per_decode = 1;
    start();
    capture_from_scratch();
    CHECK_EQ_U32(mp3_player_feed(s_mp3 + off, s_mp3_len - off), s_mp3_len - off);
    mp3_player_start_fed_output();
    uint32_t fresh_at = s_cap_frames;
    play_until(skip + n);
    mp3_player_stop_fed();
    CHECK(s_cap_frames - fresh_at >= skip + n);
    int32_t *fresh = malloc((size_t)n * 2 * sizeof *fresh);
    if (!fresh) abort();
    memcpy(fresh, s_cap + 2 * (fresh_at + skip), (size_t)n * 2 * sizeof *fresh);

    playing_session();
    capture_from_scratch();
    CHECK(mp3_player_restart_fed(s_mp3 + off, s_mp3_len - off, primed));
    uint32_t restarted_at = s_cap_frames;       // the old audio and its fade-out
    CHECK(restarted_at > 0);
    play_until(n);
    CHECK(s_cap_frames - restarted_at >= n);
    CHECK(memcmp(fresh + 2 * ramp, s_cap + 2 * (restarted_at + ramp),
                 (size_t)(n - ramp) * 2 * sizeof *fresh) == 0);
    mp3_player_stop_fed();
    free(fresh);
  }
  s_capturing = false;
  s_output_on = false;
}

// The output of a restart in place at `off`, from its fade-in on: n frames.
static int32_t *restart_output(uint32_t off, bool primed, uint32_t n) {
  playing_session();
  capture_from_scratch();
  CHECK(mp3_player_restart_fed(s_mp3 + off, s_mp3_len - off, primed));
  uint32_t at = s_cap_frames;
  play_until(n);
  CHECK(s_cap_frames - at >= n);
  int32_t *out = malloc((size_t)n * 2 * sizeof *out);
  if (!out) abort();
  memcpy(out, s_cap + 2 * at, (size_t)n * 2 * sizeof *out);
  mp3_player_stop_fed();
  s_capturing = false;
  s_output_on = false;
  return out;
}

// The video player starts a primed restart's chunks one before the
// cursor's: the first frame heard is the one an unprimed restart from the
// cursor's chunk played first (libmad dropped the cursor's own frame), now
// without the ramp up from silence; from the next frame on the two are the
// same.
static void test_primed_restart_keeps_the_position(void) {
  const uint32_t n = 20 * RENDER_FRAMES;
  const uint32_t ramp = PCM_STAGE_FADE_FRAMES;
  int32_t *primed = restart_output(frame_offset(9), true, n);
  int32_t *unprimed = restart_output(frame_offset(10), false, n);
  CHECK(memcmp(primed + 2 * ramp, unprimed + 2 * ramp,
               (SAMPLES_PER_FRAME - ramp) * 2 * sizeof *primed) != 0);
  CHECK(memcmp(primed + 2 * SAMPLES_PER_FRAME, unprimed + 2 * SAMPLES_PER_FRAME,
               (n - SAMPLES_PER_FRAME) * 2 * sizeof *primed) == 0);
  free(primed);
  free(unprimed);
}

// A restart in place clears the loop mark (here one the decoder had
// reached, not yet taken), and the session plays on as new: a mark set
// after it counts from the restart, and everything fed since plays, the
// frame decoded behind the old audio included.
static void test_mark_after_a_restart_in_place(void) {
  int32_t late;
  s_output_on = true;
  s_renders_per_decode = 1;
  start();
  feed_pass();
  mp3_player_fed_mark();
  feed_pass();
  mp3_player_start_fed_output();
  play_until(ONE_PASS - 1000);                  // the decoder is past the mark
  CHECK(mp3_player_restart_fed(s_mp3, s_mp3_len, false));
  CHECK(!mp3_player_fed_mark_reached(&late));
  mp3_player_fed_mark();
  feed_pass();
  mp3_player_fed_end();
  uint32_t pos = play_until(ONE_PASS - 1000);
  CHECK(mp3_player_fed_mark_reached(&late));
  CHECK_EQ_INT(late, (int32_t)(pos - ONE_PASS));
  CHECK_EQ_U32(play_until(UINT32_MAX), 2 * ONE_PASS);
  mp3_player_stop_fed();
  s_output_on = false;
}

// Nothing audible to play on (no session, its output not started, paused,
// or the output off): restart_fed refuses and changes nothing, and the
// video player restarts through start_fed.
static void test_restart_refused_when_nothing_plays(void) {
  restart_stats();
  s_output_on = true;
  s_renders_per_decode = 0;
  CHECK(!mp3_player_restart_fed(s_mp3, s_mp3_len, false));    // no fed session

  start();                                              // output not started
  feed_pass();
  CHECK(!mp3_player_restart_fed(s_mp3, s_mp3_len, false));
  mp3_player_fed_end();
  mp3_player_start_fed_output();
  CHECK_EQ_U32(play_until(UINT32_MAX), ONE_PASS);       // nothing more was fed
  mp3_player_stop_fed();

  start();                                              // paused
  feed_pass();
  feed_pass();
  mp3_player_fed_end();
  mp3_player_start_fed_output();
  play_until(10000);
  mp3_player_pause(mp3_player_create());
  uint32_t at = position();
  CHECK(!mp3_player_restart_fed(s_mp3, s_mp3_len, false));
  CHECK_EQ_U32(position(), at);
  mp3_player_resume(mp3_player_create());
  CHECK_EQ_U32(play_until(UINT32_MAX), 2 * ONE_PASS);   // it kept its place
  mp3_player_stop_fed();

  s_output_on = false;                                  // the output off
  start();
  feed_pass();
  mp3_player_start_fed_output();
  play_until(5000);
  CHECK(!mp3_player_restart_fed(s_mp3, s_mp3_len, false));
  mp3_player_stop_fed();

  mp3_fed_restart_stats_t st = restart_stats();
  CHECK_EQ_U32(st.restarts, 0);
  CHECK_EQ_U32(st.fallbacks, 0);
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
  test_restart_in_place_leaves_no_gap();
  test_restart_in_place_plays_the_new_audio_whole();
  test_primed_restart_keeps_the_position();
  test_mark_after_a_restart_in_place();
  test_restart_refused_when_nothing_plays();
  mp3_player_deinit();
  free(s_mp3);
  free(s_cap);
  return check_report("test_mp3_fed");
}
