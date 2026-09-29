// Host unit tests for src/drivers/avi_index.h: the video player's audio
// chunk index (issue #19). It was sized 1.5x the video frame count + 64, so
// a low frame-rate AVI, which interleaves more MP3 chunks than that, lost
// the audio past the cap: a 10 fps clip was silent for a third of a loop.
#include "check.h"
#include "avi_index.h"

#include <stdlib.h>
#include <string.h>

// One MP3 frame per chunk, as ffmpeg muxes it: 44.1 kHz MPEG-1 (1152
// samples a frame) is 38.28 chunks a second.
static uint32_t mp3_chunks(uint32_t seconds) { return seconds * 44100u / 1152u + 1; }

// ffmpeg writes MP3 as variable-bitrate AVI audio: dwSampleSize 0, and
// dwLength counts the chunks. The first capacity holds them all, whatever
// the frame rate.
static void test_vbr_header_counts_the_chunks(void) {
  uint32_t chunks = mp3_chunks(5);                   // 192
  uint32_t frames_10fps = 5 * 10;
  CHECK(frames_10fps + frames_10fps / 2 + 64 < chunks);  // the old sizing fell short
  CHECK(avi_audio_index_capacity(frames_10fps, chunks, 0) >= chunks);
  CHECK(avi_audio_index_capacity(5 * 25, chunks, 0) >= chunks);
  // ...without holding much more than that.
  CHECK(avi_audio_index_capacity(5 * 60, chunks, 0) < 5 * 60);
}

// dwSampleSize != 0 (constant-bitrate audio) makes dwLength a byte count:
// the estimate falls back to the video frame count, and the scan grows it.
static void test_cbr_header_falls_back_to_the_frame_count(void) {
  CHECK_EQ_U32(avi_audio_index_capacity(1000, 12000 * 60, 1), 1000 + 500 + 64);
  CHECK_EQ_U32(avi_audio_index_capacity(1000, 0, 0), 1000 + 500 + 64);
}

// A dwLength no real file has (a non-seekable ffmpeg output writes ~2^31)
// must not become a huge first allocation that fails and costs the audio.
static void test_bogus_length_is_not_trusted(void) {
  uint32_t cap = avi_audio_index_capacity(1000, 0x7FFFFFFFu, 0);
  CHECK_EQ_U32(cap, 1000 + 500 + 64);
  CHECK(avi_audio_index_capacity(0xFFFFFFFFu, 0xFFFFFFFFu, 1) <= AVI_AUDIO_INDEX_MAX);
}

// Pushing past the capacity grows the index and keeps every entry in order.
static void test_push_grows_and_keeps_entries(void) {
  avi_index_t ix = {0};
  ix.capacity = avi_audio_index_capacity(50, 0, 1);  // 139: the old 10 fps sizing
  ix.entries = malloc(ix.capacity * sizeof(avi_index_entry_t));
  uint32_t chunks = mp3_chunks(5);
  for (uint32_t i = 0; i < chunks; i++)
    CHECK(avi_index_push(&ix, 1000 + i * 700, 300 + i % 7, realloc));
  CHECK_EQ_U32(ix.count, chunks);
  CHECK(ix.capacity >= chunks);
  int bad = 0;
  for (uint32_t i = 0; i < chunks; i++)
    bad += ix.entries[i].file_offset != 1000 + i * 700 ||
           ix.entries[i].chunk_size != 300 + i % 7;
  CHECK_EQ_INT(bad, 0);
  free(ix.entries);
}

// Starting from nothing works too (the first push allocates).
static void test_push_from_empty(void) {
  avi_index_t ix = {0};
  CHECK(avi_index_push(&ix, 8, 16, realloc));
  CHECK_EQ_U32(ix.count, 1);
  CHECK(ix.capacity >= 1);
  CHECK_EQ_U32(ix.entries[0].file_offset, 8);
  free(ix.entries);
}

static int s_refuse;
static void *refusing_realloc(void *p, size_t n) { return s_refuse ? NULL : realloc(p, n); }

// Out of memory: the push fails and the index keeps what it had.
static void test_failed_growth_keeps_the_index(void) {
  avi_index_t ix = {0};
  ix.capacity = 4;
  ix.entries = malloc(4 * sizeof(avi_index_entry_t));
  for (uint32_t i = 0; i < 4; i++) CHECK(avi_index_push(&ix, i, i, refusing_realloc));
  s_refuse = 1;
  CHECK(!avi_index_push(&ix, 99, 99, refusing_realloc));
  CHECK_EQ_U32(ix.count, 4);
  CHECK_EQ_U32(ix.capacity, 4);
  CHECK_EQ_U32(ix.entries[3].file_offset, 3);
  s_refuse = 0;
  CHECK(avi_index_push(&ix, 4, 4, refusing_realloc));  // and grows once it can
  CHECK_EQ_U32(ix.count, 5);
  free(ix.entries);
}

// The index never grows past AVI_AUDIO_INDEX_MAX entries.
static void test_growth_stops_at_the_limit(void) {
  avi_index_t ix = {0};
  ix.capacity = AVI_AUDIO_INDEX_MAX;
  ix.count = AVI_AUDIO_INDEX_MAX;
  ix.entries = NULL;  // never touched: the push refuses before growing
  CHECK(!avi_index_push(&ix, 1, 1, realloc));
  CHECK_EQ_U32(ix.count, AVI_AUDIO_INDEX_MAX);

  avi_index_t near = {0};
  near.capacity = AVI_AUDIO_INDEX_MAX - 10;
  near.entries = malloc(near.capacity * sizeof(avi_index_entry_t));
  near.count = near.capacity;
  CHECK(avi_index_push(&near, 1, 1, realloc));
  CHECK_EQ_U32(near.capacity, AVI_AUDIO_INDEX_MAX);  // clamped, not doubled
  free(near.entries);
}

int main(void) {
  test_vbr_header_counts_the_chunks();
  test_cbr_header_falls_back_to_the_frame_count();
  test_bogus_length_is_not_trusted();
  test_push_grows_and_keeps_entries();
  test_push_from_empty();
  test_failed_growth_keeps_the_index();
  test_growth_stops_at_the_limit();
  return check_report("test_avi_index");
}
