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

// The first capacity, from the audio stream header's dwLength and
// dwSampleSize. A sample size of 0 is variable-bitrate audio (as ffmpeg
// writes MP3): every chunk is one block and dwLength counts the chunks.
// Otherwise dwLength counts bytes, or is implausible for `video_frames`
// frames (no audio has 64 chunks a frame); the estimate is then 1.5 chunks
// a video frame, and the scan grows it.
static inline uint32_t avi_audio_index_capacity(uint32_t video_frames,
                                                uint32_t strh_length,
                                                uint32_t strh_sample_size) {
    uint64_t cap;
    if (strh_sample_size == 0 && strh_length > 0 &&
        strh_length <= (uint64_t)video_frames * 64 + 1024)
        cap = (uint64_t)strh_length + 16;
    else
        cap = (uint64_t)video_frames + video_frames / 2 + 64;
    return cap > AVI_AUDIO_INDEX_MAX ? AVI_AUDIO_INDEX_MAX : (uint32_t)cap;
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
