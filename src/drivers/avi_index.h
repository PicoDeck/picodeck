#pragma once

// The video player's audio chunk index (video_player.cpp): where each audio
// chunk of an AVI's movi list is, in file order. Sized before the movi scan
// from the audio stream header, and grown during the scan when the file
// holds more chunks than that: a low frame rate interleaves several MP3
// chunks per video frame, and every chunk the index cannot hold is audio
// that never plays. Header-only (C and C++); host-tested in
// tests/unit/test_avi_index.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t file_offset;   // the chunk header
    uint32_t chunk_size;    // payload bytes
} avi_index_entry_t;

typedef struct {
    avi_index_entry_t *entries;
    uint32_t count;
    uint32_t capacity;
} avi_index_t;

// The most entries the index grows to (2 MB): past the video frame index's
// cap the scan stops anyway, and 65536 frames at 10 fps hold ~250,000
// chunks of 44.1 kHz MP3.
#define AVI_AUDIO_INDEX_MAX (256u * 1024u)

// Every chunk has an 8-byte header, so a movi list of `movi_size` bytes
// holds at most movi_size / 8 of them (0: size unknown, no cap). Never
// below a few entries: the scan grows the index past it anyway.
static inline uint32_t avi_index_cap_to_movi(uint64_t cap, uint32_t movi_size) {
    if (movi_size > 0 && cap > movi_size / 8)
        cap = movi_size / 8;
    if (cap < 16)
        cap = 16;
    return cap > AVI_AUDIO_INDEX_MAX ? AVI_AUDIO_INDEX_MAX : (uint32_t)cap;
}

// The estimate with nothing better to go on: 1.5 chunks a video frame.
static inline uint32_t avi_audio_index_fallback(uint32_t video_frames,
                                                uint32_t movi_size) {
    return avi_index_cap_to_movi((uint64_t)video_frames + video_frames / 2 + 64,
                                 movi_size);
}

// The first capacity, from the audio stream header's dwLength and
// dwSampleSize. A sample size of 0 is variable-bitrate audio (as ffmpeg
// writes MP3): every chunk is one block and dwLength counts the chunks.
// Otherwise dwLength counts bytes, or is implausible for `video_frames`
// frames (no audio has 64 chunks a frame); the estimate is then
// avi_audio_index_fallback, and the scan grows it. Either way no more than
// the movi list can hold, so a header that lies costs no more than the
// file could need.
static inline uint32_t avi_audio_index_capacity(uint32_t video_frames,
                                                uint32_t strh_length,
                                                uint32_t strh_sample_size,
                                                uint32_t movi_size) {
    if (strh_sample_size == 0 && strh_length > 0 &&
        strh_length <= (uint64_t)video_frames * 64 + 1024)
        return avi_index_cap_to_movi((uint64_t)strh_length + 16, movi_size);
    return avi_audio_index_fallback(video_frames, movi_size);
}

// Allocates an empty index of `cap` entries through `grow` (a realloc, from
// NULL), or of `fallback` entries when that fails and `fallback` is
// smaller: an over-large first guess must not cost the audio. False, the
// index empty, when neither fits.
static inline bool avi_index_alloc(avi_index_t *ix, uint32_t cap,
                                   uint32_t fallback,
                                   void *(*grow)(void *, size_t)) {
    ix->count = 0;
    ix->capacity = 0;
    ix->entries = (avi_index_entry_t *)grow(NULL, (size_t)cap * sizeof(avi_index_entry_t));
    if (!ix->entries && fallback < cap) {
        cap = fallback;
        ix->entries = (avi_index_entry_t *)grow(NULL, (size_t)cap * sizeof(avi_index_entry_t));
    }
    if (ix->entries)
        ix->capacity = cap;
    return ix->entries != NULL;
}

// Appends an entry. When the index is full it grows by half (at least 64
// entries, at most to AVI_AUDIO_INDEX_MAX) through `grow`, a realloc. False,
// the index unchanged, when it cannot grow.
static inline bool avi_index_push(avi_index_t *ix, uint32_t file_offset,
                                  uint32_t chunk_size,
                                  void *(*grow)(void *, size_t)) {
    if (ix->count >= ix->capacity) {
        if (ix->capacity >= AVI_AUDIO_INDEX_MAX)
            return false;
        uint32_t add = ix->capacity / 2 < 64 ? 64 : ix->capacity / 2;
        uint32_t cap = AVI_AUDIO_INDEX_MAX - ix->capacity < add
                           ? AVI_AUDIO_INDEX_MAX
                           : ix->capacity + add;
        void *p = grow(ix->entries, (size_t)cap * sizeof(avi_index_entry_t));
        if (!p)
            return false;
        ix->entries = (avi_index_entry_t *)p;
        ix->capacity = cap;
    }
    ix->entries[ix->count].file_offset = file_offset;
    ix->entries[ix->count].chunk_size = chunk_size;
    ix->count++;
    return true;
}
