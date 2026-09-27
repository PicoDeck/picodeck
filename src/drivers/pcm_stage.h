#pragma once

// The MP3 player's staging buffer and its read side, the mixer's MP3
// source (mp3_player.c): 16-bit PCM at the content's own rate (mono, or
// interleaved stereo), read at the mixer's rate with nearest-neighbour
// resampling, scaled by the player volume and a PCM_STAGE_FADE_FRAMES fade
// envelope, and added into the mixer's int32 accumulators.
//
// Header-only and always inlined, like audio_ring.h: pcm_stage_mix runs
// per output frame inside the __time_critical_func mixer, which must not
// call out to flash. Host-tested in tests/unit/test_pcm_stage.c.
//
// One writer (the refill, on Core 1 in task context) and one reader (the
// mixer, in Core 1's DMA refill ISR). The unread bytes are
// buf[pos, pos + avail): the reader moves pos and avail forward and never
// touches the tail, buf[pos + avail, size), so the writer copies into the
// tail without the lock. compact() and commit() move pos or avail from the
// writer's side and run under the owner's lock (mp3_player.c's s_stage_cs),
// as does every other call that changes what the reader uses.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PCM_STAGE_FADE_FRAMES 64u

typedef enum {
    PCM_FADE_NONE,    // full gain
    PCM_FADE_IN,      // ramping up
    PCM_FADE_OUT,     // ramping down
    PCM_FADE_SILENT,  // adds nothing, keeps its data: fresh, or faded out
} pcm_fade_t;

typedef struct {
    uint8_t *buf;
    uint32_t size;                    // bytes, a multiple of 4
    volatile uint32_t pos;            // the first unread byte
    volatile uint32_t avail;          // unread bytes
    uint32_t rate;                    // content frames per second
    uint32_t out_rate;                // mixer frames per second
    uint32_t phase;                   // the resample accumulator
    uint8_t channels;                 // 1 or 2
    uint32_t vol_scale;               // 0..256, 256 = full volume
    volatile pcm_fade_t fade;
    uint32_t fade_pos;                // frames into the current ramp
    volatile bool eof;                // no more data will come: running dry
                                      // is the end, not an underrun
    volatile uint32_t frames_played;  // content frames read since the reset
    volatile uint32_t underruns;      // output frames that found no data
} pcm_stage_t;

#define PCM_STAGE_INLINE static inline __attribute__((always_inline))

// Empties the stage and silences it: the next start fades in.
PCM_STAGE_INLINE void pcm_stage_reset(pcm_stage_t *s) {
    s->pos = 0;
    s->avail = 0;
    s->phase = 0;
    s->fade = PCM_FADE_SILENT;
    s->fade_pos = 0;
    s->eof = false;
    s->frames_played = 0;
}

PCM_STAGE_INLINE void pcm_stage_init(pcm_stage_t *s, uint8_t *buf,
                                     uint32_t size, uint32_t out_rate) {
    s->buf = buf;
    s->size = size;
    s->out_rate = out_rate;
    s->rate = out_rate;
    s->channels = 2;
    s->vol_scale = 256;
    s->underruns = 0;
    pcm_stage_reset(s);
}

// The content's format. A zero rate plays at the mixer's rate.
PCM_STAGE_INLINE void pcm_stage_set_format(pcm_stage_t *s, uint32_t rate,
                                           uint8_t channels) {
    s->rate = rate ? rate : s->out_rate;
    s->channels = channels == 1 ? 1 : 2;
}

PCM_STAGE_INLINE uint32_t pcm_stage_frame_bytes(const pcm_stage_t *s) {
    return 2u * s->channels;
}

// Whole frames not yet read.
PCM_STAGE_INLINE uint32_t pcm_stage_frames(const pcm_stage_t *s) {
    return s->avail / pcm_stage_frame_bytes(s);
}

// Moves the unread bytes to the front of the buffer (writer, locked).
PCM_STAGE_INLINE void pcm_stage_compact(pcm_stage_t *s) {
    uint32_t pos = s->pos, avail = s->avail;
    if (pos > 0 && avail > 0)
        memmove(s->buf, s->buf + pos, avail);
    s->pos = 0;
}

// Where the writer copies to, and how many bytes fit there (whole frames).
// The tail stays put while the reader consumes.
PCM_STAGE_INLINE uint8_t *pcm_stage_tail(const pcm_stage_t *s,
                                         uint32_t *space) {
    uint32_t end = s->pos + s->avail;
    uint32_t fb = pcm_stage_frame_bytes(s);
    *space = (s->size - end) / fb * fb;
    return s->buf + end;
}

// Publishes n bytes the writer copied to the tail (writer, locked).
PCM_STAGE_INLINE void pcm_stage_commit(pcm_stage_t *s, uint32_t n) {
    s->avail += n;
}

// Ramps up from the current gain: from silence, or back up from partway
// through a fade-out, so the gain never jumps. At full gain, or already
// ramping up: no change.
PCM_STAGE_INLINE void pcm_stage_fade_in(pcm_stage_t *s) {
    if (s->fade == PCM_FADE_SILENT) {
        s->fade_pos = 0;
        s->fade = PCM_FADE_IN;
    } else if (s->fade == PCM_FADE_OUT) {
        s->fade_pos = PCM_STAGE_FADE_FRAMES - s->fade_pos;
        s->fade = PCM_FADE_IN;
    }
}

// Ramps down to silence from the current gain. Silent, or already ramping
// down: no change.
PCM_STAGE_INLINE void pcm_stage_fade_out(pcm_stage_t *s) {
    if (s->fade == PCM_FADE_NONE) {
        s->fade_pos = 0;
        s->fade = PCM_FADE_OUT;
    } else if (s->fade == PCM_FADE_IN) {
        s->fade_pos = PCM_STAGE_FADE_FRAMES - s->fade_pos;
        s->fade = PCM_FADE_OUT;
    }
}

// Adds `frames` output frames of the stage into l and r (reader, locked).
// A fade-out that ends here stops the call: the rest of the frames get
// nothing, and the unread data stays for a fade back in.
PCM_STAGE_INLINE void pcm_stage_mix(pcm_stage_t *s, int32_t *l, int32_t *r,
                                    int frames) {
    pcm_fade_t fade = s->fade;
    if (fade == PCM_FADE_SILENT)
        return;
    const uint32_t fb = pcm_stage_frame_bytes(s);
    const bool stereo = s->channels > 1;
    const int32_t vol = (int32_t)s->vol_scale;
    uint32_t pos = s->pos, avail = s->avail, phase = s->phase;
    uint32_t fade_pos = s->fade_pos, played = s->frames_played;
    uint32_t under = s->underruns;
    for (int i = 0; i < frames; i++) {
        int32_t lv = 0, rv = 0;
        if (avail < fb) {
            if (!s->eof)
                under++;
        } else {
            int16_t a, b;
            memcpy(&a, s->buf + pos, 2);
            b = a;
            if (stereo)
                memcpy(&b, s->buf + pos + 2, 2);
            lv = a;
            rv = b;
            // Advance at the content's own rate (22050 Hz content is heard
            // twice per frame at 44100).
            phase += s->rate;
            while (phase >= s->out_rate) {
                phase -= s->out_rate;
                if (avail >= fb) {
                    pos += fb;
                    avail -= fb;
                    played++;
                }
            }
        }
        lv = lv * vol / 256;
        rv = rv * vol / 256;
        if (fade != PCM_FADE_NONE) {
            uint32_t g = fade == PCM_FADE_IN ? fade_pos
                                             : PCM_STAGE_FADE_FRAMES - fade_pos;
            int32_t gain = (int32_t)(g * 256u / PCM_STAGE_FADE_FRAMES);
            lv = lv * gain / 256;
            rv = rv * gain / 256;
            if (++fade_pos >= PCM_STAGE_FADE_FRAMES) {
                fade = fade == PCM_FADE_IN ? PCM_FADE_NONE : PCM_FADE_SILENT;
                fade_pos = 0;
            }
        }
        l[i] += lv;
        r[i] += rv;
        if (fade == PCM_FADE_SILENT)
            break;
    }
    s->pos = pos;
    s->avail = avail;
    s->phase = phase;
    s->fade = fade;
    s->fade_pos = fade_pos;
    s->frames_played = played;
    s->underruns = under;
}
