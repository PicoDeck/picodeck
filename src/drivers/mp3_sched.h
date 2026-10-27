#pragma once

// When Core 1's MP3 decoder decodes a granule (mp3_player.c's
// decode_fill_ring; issue #28): 576 samples a channel, half an MPEG-1
// frame (44.1 kHz) or all of an MPEG-2 one. Pure, header-only; host-tested
// in tests/unit/test_mp3_sched.c against a model of the ring, the stage,
// the mixer and a paced or unpaced Core 0.
//
// Decoding contends with Core 0 for the XIP cache and the QMI: a granule
// runs ~11 KB of flash code over a few KB of tables and the decoder state
// left in QMI PSRAM, through the shared 16 KB cache (src/drivers/CLAUDE.md,
// MP3). Two reasons to decode:
//  - LOW: the PCM ring is at or below half at the update's start. Today's
//    rule, and the floor: a burst whatever Core 0 does, so nothing starves.
//  - IDLE: Core 0 waits in perf.endFrame's pacing (os/core0_idle.h) and at
//    least one granule's decode time is left in its window: decode ahead,
//    so the contention lands on Core 0's idle time. An app that never paces
//    opens no window and decodes on the low ring only.
// Either way an update decodes at most MP3_SCHED_BURST granules.

#include <stdbool.h>
#include <stdint.h>

#define MP3_SCHED_BURST    6u     // granules per update, either reason
#define MP3_SCHED_SEED_US  3000u  // first estimate of an idle granule's decode

typedef enum {
    MP3_SCHED_NONE,   // decode nothing more in this update
    MP3_SCHED_LOW,    // the ring was low: decode now
    MP3_SCHED_IDLE,   // Core 0 is idle long enough: decode ahead
} mp3_sched_why_t;

typedef struct {
    uint32_t frame_us;  // one granule's decode with Core 0 idle (estimate;
                        // the name is xipstat's mp3_frame_us)
} mp3_sched_t;

static inline void mp3_sched_init(mp3_sched_t *s) {
    s->frame_us = MP3_SCHED_SEED_US;
}

// At an update's start: whether the ring is low (bytes queued, its size).
static inline bool mp3_sched_low(uint32_t ring_used, uint32_t ring_size) {
    return ring_used <= ring_size / 2;
}

// Before each granule of an update: whether to decode it, and why.
// `decoded`: granules this update has decoded; `idle_left_us`: what is left
// of Core 0's idle window (0: Core 0 is working, or decoding ahead is off).
static inline mp3_sched_why_t mp3_sched_next(const mp3_sched_t *s, bool low,
                                             uint32_t decoded,
                                             uint32_t idle_left_us) {
    if (decoded >= MP3_SCHED_BURST)
        return MP3_SCHED_NONE;
    if (low)
        return MP3_SCHED_LOW;
    if (idle_left_us > 0 && idle_left_us >= s->frame_us)
        return MP3_SCHED_IDLE;
    return MP3_SCHED_NONE;
}

// An IDLE granule that finished inside its window took `us`: a quarter of
// it goes into the estimate. A granule that ran past the window's end is
// not noted (Core 0's work slowed it), so the estimate is of uncontended
// granules; when no window is long enough to finish one, the estimate stays
// put and each window still starts one granule (its overlap with Core 0 is
// shorter than decoding the same granule when the ring runs low).
static inline void mp3_sched_note(mp3_sched_t *s, uint32_t us) {
    s->frame_us = s->frame_us - s->frame_us / 4u + us / 4u;
}
