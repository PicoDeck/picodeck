// Host unit tests for src/drivers/mod_player.c (firmware and simulator).
//
// Review row Audio/storage High "Cross-core use-after-free ...
// mod_player_update doesn't take s_mod_mutex, but mod_player_load frees
// mod_data". Core 1's update runs on a second thread against Core 0's
// load/play/stop/destroy on this one; built with ASan, rendering freed MOD
// data is a heap-use-after-free.
#include "check.h"
#include "mod_player.h"
#include "audio.h"
#include "fakes/sdcard_fake.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// ── Stream ring model (audio.c) ─────────────────────────────────────────────
static atomic_uint s_ring_used;
static atomic_ulong s_pushed;
void audio_start_stream(uint32_t sample_rate) { (void)sample_rate; atomic_store(&s_ring_used, 0); }
void audio_stop_stream(void) {}
uint32_t audio_ring_free(void) { return 4096u - atomic_load(&s_ring_used); }
void audio_push_samples(const int16_t *samples, int count) {
  (void)samples;
  atomic_fetch_add(&s_ring_used, (unsigned)count);
  atomic_fetch_add(&s_pushed, (unsigned long)count);
}
void audio_stream_debug(uint32_t *isr, uint32_t *under, uint32_t *used) {
  if (isr) *isr = 0;
  if (under) *under = 0;
  if (used) *used = atomic_load(&s_ring_used);
}
static void drain(unsigned frames) {
  unsigned u = atomic_load(&s_ring_used);
  atomic_store(&s_ring_used, u > frames ? u - frames : 0);
}

// A minimal valid ProTracker MOD ("M.K.", 4 channels, one silent pattern)
// as tests/e2e/helpers.write_mod builds it.
static void put_mod(const char *path) {
  size_t len = 20 + 31 * 30 + 2 + 128 + 4 + 64 * 4 * 4;
  char *m = calloc(1, len);
  memcpy(m, "picodeck test", 13);  // title field is 20 bytes
  for (int i = 0; i < 31; i++) {
    char *s = m + 20 + i * 30;
    s[25] = 0x40;   // volume
    s[29] = 0x01;   // loop length 1 word = no loop
  }
  char *t = m + 20 + 31 * 30;
  t[0] = 1; t[1] = 127;
  memcpy(t + 2 + 128, "M.K.", 4);
  sdfake_put(path, m, len);
  free(m);
}

static void test_load_and_play(void) {
  sdfake_reset();
  put_mod("/a.mod");
  mod_player_t *p = mod_player_create();
  CHECK(p != NULL);
  CHECK(mod_player_load(p, "/a.mod"));
  mod_player_play(p, true);
  CHECK(mod_player_is_playing(p));
  // Core 1's tick gate (main.c) skips mod_player_update while this reads idle.
  CHECK(!mod_player_core1_idle());
  atomic_store(&s_pushed, 0);
  for (int i = 0; i < 50; i++) { mod_player_update(); drain(4096); }
  CHECK(atomic_load(&s_pushed) > 0);
  mod_player_destroy(p);
  CHECK(!mod_player_is_playing(p));
}

static atomic_bool s_run_core1;
static void *core1(void *arg) {
  (void)arg;
  while (atomic_load(&s_run_core1)) {
    mod_player_update();
    drain(256);
  }
  return NULL;
}

static void test_cross_core_load_is_safe(void) {
  sdfake_reset();
  put_mod("/a.mod");
  put_mod("/b.mod");
  mod_player_t *p = mod_player_create();
  atomic_store(&s_run_core1, true);
  pthread_t t;
  pthread_create(&t, NULL, core1, NULL);
  for (int i = 0; i < 20000; i++) {
    mod_player_load(p, (i & 1) ? "/a.mod" : "/b.mod");
    mod_player_play(p, true);
    if (i % 5 == 0) mod_player_destroy(p);
    if (i % 7 == 0) mod_player_stop(p);
  }
  atomic_store(&s_run_core1, false);
  pthread_join(t, NULL);
  mod_player_destroy(p);
  CHECK(1);
}

int main(void) {
  mod_player_init();
  test_load_and_play();
  test_cross_core_load_is_safe();
  mod_player_deinit();
  sdfake_reset();
  return check_report("test_mod_player");
}
