#include "mp3_player.h"
#include "audio.h"
#include "pcm_stage.h"
#include "mp3_sched.h"
#include "../os/core0_idle.h"
#include "sdcard.h"
#include "pio_psram.h"
#include "pico/platform.h"
#include "pico/critical_section.h"
#include "pico/mutex.h"
#include "pico/time.h"
#include "hardware/clocks.h"

#define FPM_DEFAULT
#include "mad.h"

#include "umm_malloc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MP3_DECODE_BUFFER_SIZE (8192 + MAD_BUFFER_GUARD)
#define PCM_RING_SIZE          32768
// The SRAM staging buffer the mixer reads (pcm_stage.h): ~46 ms of 44.1 kHz
// stereo. The mixer runs in Core 1's DMA refill ISR and never touches PIO
// PSRAM, so Core 1's update copies the PCM ring into it between decodes.
#define STAGING_BUF_SIZE       8192

static struct mad_stream *s_mad_stream = NULL;
static struct mad_frame  *s_mad_frame  = NULL;
// The frame's arrays in QMI PSRAM (umm, from init): all of them, or on the
// device those left out of SRAM (s_mp3_sram).
#if defined(PICODECK_MP3_SRAM)
typedef struct {
    mad_fixed_t overlap[2][32][18];
    mad_fixed_t tmp[576];
} frame_psram_t;
#else
typedef struct mad_frame_mem frame_psram_t;
#endif
static frame_psram_t *s_mad_frame_mem = NULL;
static struct mad_synth  *s_mad_synth  = NULL;
#if defined(PICODECK_MP3_SRAM)
// The decoder's hottest state in SRAM on the device (issue #28), 17.6 KB;
// the rest (the stream and its bit reservoir, the IMDCT overlap, the
// short-block reorder buffer, the input) stays in QMI PSRAM. In QMI PSRAM
// every access went through the 16 KB XIP cache Core 0 shares: a 44.1 kHz
// stereo frame missed it ~14 k times (13 ms a frame at 200 MHz, Core 0
// idle; mp3bench). Measured one at a time, what each saves a frame:
//  - the synthesis state (the polyphase filterbank, read whole for every
//    32 samples of each channel, and a granule's PCM, which the ring
//    write's DMA then reads straight from SRAM instead of a byte at a time
//    through the uncached alias): 2.9 ms;
//  - the subband samples (one granule: written column by column by the
//    IMDCT, read row by row by the synthesis): ~3 ms;
//  - the requantised spectrum (xr, both channels: Huffman decode, stereo,
//    alias reduction, IMDCT each sweep it): 2 ms;
//  - the window D[] (libmad's synth.c, MAD_D_IN_RAM): 0.6 ms more.
// Next in line, with no SRAM left: the overlap (4.5 KB, 1.4 ms) and
// layer3.c's code (~11 KB, 1-2 ms).
static struct {
    struct mad_synth synth;
    mad_fixed_t sbsample[2][18][32];
    mad_fixed_t xr[576 * 2];
} s_mp3_sram;
#endif
static sdfile_t    s_file = NULL;
static uint32_t    s_file_pos = 0;  // next byte of s_file to decode (Core 1
                                    // reads at it: no blocking fseek there)
// The compressed input, in QMI PSRAM (umm, from init): libmad reads it a
// byte at a time once per frame (~0.4 KB a frame at 128 kbps), so the
// cache serves it with a few dozen line fills; SRAM goes to the decoder's
// hot state instead (issue #28).
static uint8_t    *s_decode_buffer = NULL;
static int         s_bytes_in_buffer = 0;
static int         s_buffer_pos = 0;
static bool        s_eof = false;   // decoding reached the end (not looping)
// The input has ended (the file, or fed data after mp3_player_fed_end):
// libmad now gets MAD_BUFFER_GUARD zero bytes after the data, so it decodes
// the last frame. Until then it gets only the data: it decodes a frame only
// with the next one's header in sight, which it reads to keep the bit
// reservoir the next frame draws on. A frame ending at the very end of the
// buffer, decoded against zeros, kept none, and the next was skipped
// (BADDATAPTR): with 4 KB refills a constant bitrate comes back to that
// alignment, every 1.28 s at 128 kbps.
static bool        s_input_end = false;
// A frame in progress (decode_fill_ring decodes a granule at a time):
// mad_frame_decode_begin() succeeded and granules s_gr_next ..
// s_gr_count - 1 are still to decode. Until it ends the decode buffer must
// not move (libmad reads the frame's main_data from it), so nothing
// refills it meanwhile: refills happen only between frames. Every decoder
// reset (mad_frame_init) abandons it.
#define MP3_GRANULE_SAMPLES 576u
static bool         s_in_frame = false;
static unsigned int s_gr_next = 0, s_gr_count = 0;
static uint64_t     s_frame_t0 = 0;  // when it began (a held restart's timing)

static mp3_player_t s_player;
static bool         s_initialized = false;
static mutex_t      s_mp3_mutex;
#define MAX_ERRORS_PER_UPDATE  32

static uint8_t *s_pcm_ring = NULL;
static _Atomic size_t s_ring_rd = 0;
static _Atomic size_t s_ring_wr = 0;
static bool s_use_pio_psram = false;
static uint32_t s_pio_psram_base = 0;
static int  s_pcm_channels = 2;

// ── The mixer's side ────────────────────────────────────────────────────────
// The mixer (audio_mix_render: Core 1's DMA refill ISR on the device) pulls
// the stage through mp3_player_mix. s_stage_cs guards the stage and
// s_mixing (whether the mixer pulls it at all): the mixer holds it for one
// 32-frame chunk, the refill for its compaction and commit, Core 0 for field
// updates. A striped spin lock with interrupts off: never held across
// anything slow (SD, PIO PSRAM copies, printf, umm), never nested with
// another lock.
static uint8_t s_staging_buf[STAGING_BUF_SIZE] __attribute__((aligned(4)));
static pcm_stage_t s_stage;
static critical_section_t s_stage_cs;
static atomic_bool s_stage_ready;  // set once: the mixer may run before the first init
static volatile bool s_mixing = false;

// Forced inline: the mixer (RAM-resident, in the refill ISR) must not
// reach them through a flash veneer.
static __force_inline void stage_lock(void) {
    critical_section_enter_blocking(&s_stage_cs);
}

static __force_inline void stage_unlock(void) {
    critical_section_exit(&s_stage_cs);
}

// Diagnostics (declared early: used by the ring/refill/decode paths below).
static volatile uint32_t s_diag_updates = 0;       // mp3_player_update entered
static volatile uint32_t s_diag_skip_mutex = 0;    // update skipped: mutex held (Core 0 API call)
static volatile uint32_t s_diag_refill_calls = 0;  // refill attempted (staging below half)
static volatile uint32_t s_diag_refill_empty = 0;  // refill found PCM ring empty
static volatile uint32_t s_diag_decode_runs = 0;   // decode_fill_ring past 50% trigger
static volatile uint32_t s_diag_decode_frames = 0; // mad frames decoded OK
static volatile uint32_t s_diag_decode_errs = 0;   // mad errors
static volatile uint32_t s_diag_sd_fail = 0;       // SD refill: mutex busy or read failed
static volatile uint32_t s_diag_max_us = 0;        // longest single update
static volatile uint64_t s_diag_total_us = 0;

// When to decode (mp3_sched.h, issue #28): the decode-time estimate (Core 1,
// under s_mp3_mutex), the switch (xipstat mp3idle on|off) and the counts.
static mp3_sched_t s_sched = {MP3_SCHED_SEED_US};
static atomic_bool s_decode_ahead = true;
static volatile uint32_t s_sched_low_frames = 0;   // decoded: the ring was low
static volatile uint32_t s_sched_idle_frames = 0;  // decoded ahead, Core 0 idle
static volatile uint32_t s_sched_overran = 0;      // ...that ran past the window
// Where a granule's time goes (xipstat): its decode, mad_synth_granule,
// and the PCM ring write, summed over the granules decoded (us).
static volatile uint32_t s_prof_dec_us = 0, s_prof_syn_us = 0, s_prof_out_us = 0;

// ── Fed mode: compressed MP3 ring in QMI PSRAM, written by Core 0 (video) ───
// Uses umm_malloc (not PIO PSRAM) because both cores access this ring
// concurrently and PIO1 SPI is not thread-safe across cores.
#define FED_RING_SIZE  (64 * 1024)
static bool     s_fed_mode = false;
static uint8_t *s_fed_ring_buf = NULL; // umm_malloc'd in QMI PSRAM
static _Atomic uint32_t s_fed_wr = 0; // written by Core 0
static _Atomic uint32_t s_fed_rd = 0; // read by Core 1
static atomic_bool      s_fed_end;    // Core 0: nothing more will be fed

// The loop mark (mp3_player_fed_mark). Stream positions count from
// start_fed: s_fed_fed is the bytes fed (Core 0); s_fed_taken the bytes the
// decoder has taken from the ring and s_fed_frames the content frames it
// has decoded (the decoder's side, under s_mp3_mutex). Core 0 sets
// s_mark_at, then arms the mark; the decoder sets s_mark_frame (the frames
// decoded before the first frame at or after s_mark_at), then marks it
// reached; Core 0 clears it once it has read it.
enum { MARK_NONE, MARK_ARMED, MARK_REACHED };
static uint32_t    s_fed_fed = 0;
static uint32_t    s_fed_taken = 0;
static uint32_t    s_fed_frames = 0;
static uint32_t    s_mark_at = 0;
static uint32_t    s_mark_frame = 0;
static _Atomic int s_mark_state = MARK_NONE;

// A restart in place (mp3_player_restart_fed) decodes the new audio's
// first frame while the stage still plays the old: nothing may move into
// the stage meanwhile (refill_staging_buf), and the decode stops at that
// one frame, or before a frame that might not be done before the old audio
// runs out: s_hold_deadline (time_us_64), less s_hold_frame_us (the
// longest frame decoded behind it so far, seeded with RESTART_FRAME_US).
// Set and cleared by Core 0 inside one hold of s_mp3_mutex.
static bool s_stage_held = false;
static uint64_t s_hold_deadline = 0;
static uint32_t s_hold_frame_us = 0;
static bool s_hold_late = false;    // the decode stopped at the deadline
// One frame's decode on Core 0 behind the old audio, before one is
// measured: 44.1 kHz stereo at 96 kbps took 8.4-10.4 ms (decode, synthesis
// and the PCM ring write) at the video's 300 MHz.
#define RESTART_FRAME_US  12000u
// The old audio the switch itself needs: its fade-out renders in the next
// refill (every 2.9 ms), then Core 0 resets the stage.
#define RESTART_GUARD_US  6000u
// Frames to decode and not play: a restart mid-stream primes the decoder
// with one, since from nothing (the synthesis filterbank and the IMDCT
// overlap empty) its first frame plays as a ramp up from silence, ~5 ms of
// zeros then ~7 ms quiet. Under s_mp3_mutex.
static int s_prime_frames = 0;

// Restart diagnostics (mp3_player_fed_restart_stats), Core 0 only.
// s_fallback_silent_at: when start_fed faded out a playing session (0:
// it did not), for start_fed_output to measure the gap at its attach.
static mp3_fed_restart_stats_t s_restart_stats;
static uint64_t s_fallback_silent_at = 0;

static inline uint32_t fed_ring_available(void) {
    uint32_t wr = atomic_load_explicit(&s_fed_wr, memory_order_acquire);
    uint32_t rd = atomic_load_explicit(&s_fed_rd, memory_order_acquire);
    return (wr >= rd) ? (wr - rd) : (FED_RING_SIZE - rd + wr);
}

static inline uint32_t fed_ring_free(void) {
    return FED_RING_SIZE - 1 - fed_ring_available();
}

static void fed_ring_read(uint8_t *dst, uint32_t len) {
    uint32_t rd = atomic_load_explicit(&s_fed_rd, memory_order_acquire);
    uint32_t to_end = FED_RING_SIZE - rd;
    if (len <= to_end) {
        memcpy(dst, s_fed_ring_buf + rd, len);
    } else {
        memcpy(dst, s_fed_ring_buf + rd, to_end);
        memcpy(dst + to_end, s_fed_ring_buf, len - to_end);
    }
    atomic_store_explicit(&s_fed_rd, (rd + len) % FED_RING_SIZE, memory_order_release);
}

// ── PCM ring ────────────────────────────────────────────────────────────────
static inline size_t ring_available(void) {
    size_t wr = atomic_load_explicit(&s_ring_wr, memory_order_acquire);
    size_t rd = atomic_load_explicit(&s_ring_rd, memory_order_acquire);
    return (wr >= rd) ? (wr - rd) : (PCM_RING_SIZE - rd + wr);
}

static inline size_t ring_free(void) {
    return PCM_RING_SIZE - 1 - ring_available();
}

static void ring_write(const uint8_t *data, size_t len) {
    while (len > 0) {
        size_t wr = atomic_load_explicit(&s_ring_wr, memory_order_relaxed);
        size_t free_space = ring_free();
        if (free_space == 0) break;

        size_t chunk = (len < free_space) ? len : free_space;
        size_t to_end = PCM_RING_SIZE - wr;

        if (s_use_pio_psram) {
            if (chunk <= to_end) {
                pio_psram_write(s_pio_psram_base + wr, data, chunk);
            } else {
                pio_psram_write(s_pio_psram_base + wr, data, to_end);
                pio_psram_write(s_pio_psram_base, data + to_end, chunk - to_end);
            }
        } else {
            if (chunk <= to_end) {
                memcpy(s_pcm_ring + wr, data, chunk);
            } else {
                memcpy(s_pcm_ring + wr, data, to_end);
                memcpy(s_pcm_ring, data + to_end, chunk - to_end);
            }
        }
        atomic_store_explicit(&s_ring_wr, (wr + chunk) % PCM_RING_SIZE, memory_order_release);
        data += chunk;
        len -= chunk;
    }
}

// ── Refill the staging buffer from the PCM ring ─────────────────────────────
// Core 1's update (and Core 0's pre-fill in play, with the stage detached)
// keeps the stage topped up between decodes.
static void stage_fill(void);

static void refill_staging_buf(void) {
    if (s_stage_held || s_stage.avail >= STAGING_BUF_SIZE / 2)
        return;
    stage_fill();
}

// Copies as much of the PCM ring as fits into the stage.
static void stage_fill(void) {
    s_diag_refill_calls++;
    // Compact under the lock: the mixer reads [pos, pos + avail) and moves
    // both (a memmove it could preempt used to double or drop whole blocks).
    uint32_t space;
    stage_lock();
    pcm_stage_compact(&s_stage);
    uint8_t *dst = pcm_stage_tail(&s_stage, &space);
    uint32_t fb = pcm_stage_frame_bytes(&s_stage);
    stage_unlock();
    // Outside it the mixer only consumes: the tail stays where it is.
    size_t avail = ring_available();
    size_t to_read = space < avail ? space : avail;
    to_read -= to_read % fb;
    if (to_read == 0) { s_diag_refill_empty++; return; }

    size_t rd = atomic_load_explicit(&s_ring_rd, memory_order_relaxed);
    size_t to_end = PCM_RING_SIZE - rd;
    if (s_use_pio_psram) {
        if (to_read <= to_end) {
            pio_psram_read(s_pio_psram_base + rd, dst, to_read);
        } else {
            pio_psram_read(s_pio_psram_base + rd, dst, to_end);
            pio_psram_read(s_pio_psram_base, dst + to_end, to_read - to_end);
        }
    } else {
        if (to_read <= to_end) {
            memcpy(dst, s_pcm_ring + rd, to_read);
        } else {
            memcpy(dst, s_pcm_ring + rd, to_end);
            memcpy(dst + to_end, s_pcm_ring, to_read - to_end);
        }
    }
    atomic_store_explicit(&s_ring_rd, (rd + to_read) % PCM_RING_SIZE, memory_order_release);
    stage_lock();
    pcm_stage_commit(&s_stage, (uint32_t)to_read);
    stage_unlock();
}

uint32_t mp3_player_staging_underruns(void) { return s_stage.underruns; }

// Under the lock: the mixer read-modify-writes the count (a dev command's
// reset could land mid-chunk and be lost). Before the first init there is
// no lock yet, and nothing has counted.
void mp3_player_reset_staging_underruns(void) {
    if (!atomic_load(&s_stage_ready)) return;
    stage_lock();
    s_stage.underruns = 0;
    stage_unlock();
}

void mp3_player_get_diag(uint32_t out[11]) {
    out[0] = s_stage.underruns;
    out[1] = s_diag_updates;
    out[2] = s_diag_skip_mutex;
    out[3] = s_diag_refill_calls;
    out[4] = s_diag_refill_empty;
    out[5] = s_diag_decode_runs;
    out[6] = s_diag_decode_frames;
    out[7] = s_diag_decode_errs;
    out[8] = s_diag_sd_fail;
    out[9] = s_diag_max_us;
    out[10] = (uint32_t)(s_diag_total_us & 0xFFFFFFFFu);
}

void mp3_player_get_sched_stats(mp3_sched_stats_t *out) {
    out->low_frames = s_sched_low_frames;
    out->idle_frames = s_sched_idle_frames;
    out->overran = s_sched_overran;
    out->frame_us = s_sched.frame_us;
    out->decode_ahead = atomic_load(&s_decode_ahead);
    out->dec_us = s_prof_dec_us;
    out->syn_us = s_prof_syn_us;
    out->out_us = s_prof_out_us;
}

void mp3_player_reset_sched_stats(void) {
    s_sched_low_frames = s_sched_idle_frames = s_sched_overran = 0;
    s_prof_dec_us = s_prof_syn_us = s_prof_out_us = 0;
}

void mp3_player_set_decode_ahead(bool on) {
    atomic_store(&s_decode_ahead, on);
}

void mp3_player_reset_diag(void) {
    mp3_player_reset_staging_underruns();
    s_diag_updates = s_diag_skip_mutex = 0;
    s_diag_refill_calls = s_diag_refill_empty = 0;
    s_diag_decode_runs = s_diag_decode_frames = s_diag_decode_errs = 0;
    s_diag_sd_fail = 0;
    s_diag_max_us = 0;
    s_diag_total_us = 0;
}

// ── Refill compressed-data buffer from SD card or fed ring ──────────────────
// What a refill achieved. The decode loop must stop on anything but
// REFILL_GOT_DATA: retrying at once, as it used to, busy-spun Core 1 (no
// WiFi, no other audio) for as long as Core 0 held the SD card or the video
// player had not fed more data, whenever a partial frame was buffered.
typedef enum {
    REFILL_GOT_DATA,  // new bytes were appended
    REFILL_WAIT,      // none now (SD busy, fed ring empty, buffer full): next tick
    REFILL_EOF,       // the file is exhausted (or unreadable)
} refill_t;

static refill_t refill_decode_buffer(void) {
    // Shift leftover data to front
    if (s_buffer_pos > 0 && s_bytes_in_buffer > 0) {
        memmove(s_decode_buffer, s_decode_buffer + s_buffer_pos, s_bytes_in_buffer);
    }
    s_buffer_pos = 0;

    refill_t result = REFILL_WAIT;
    int space = (int)MP3_DECODE_BUFFER_SIZE - s_bytes_in_buffer - MAD_BUFFER_GUARD;
    if (space > 0) {
        if (s_fed_mode) {
            // Fed mode: read from the compressed audio ring. The end only
            // once the video player has said so and the ring is empty.
            bool ended = atomic_load_explicit(&s_fed_end, memory_order_acquire);
            uint32_t avail = fed_ring_available();
            uint32_t to_read = ((uint32_t)space < avail) ? (uint32_t)space : avail;
            if (to_read > 4096) to_read = 4096;
            if (to_read > 0) {
                fed_ring_read(s_decode_buffer + s_bytes_in_buffer, to_read);
                s_bytes_in_buffer += (int)to_read;
                s_fed_taken += to_read;
                result = REFILL_GOT_DATA;
            } else if (ended && avail == 0) {
                result = REFILL_EOF;
            }
        } else if (!s_file) {
            result = REFILL_EOF;
        } else {
            // SD mode: non-blocking positioned read
            int to_read = (space > 4096) ? 4096 : space;
            int br = sdcard_try_fread_at(s_file, s_file_pos,
                                         s_decode_buffer + s_bytes_in_buffer, to_read);
            if (br > 0) {
                s_bytes_in_buffer += br;
                s_file_pos += (uint32_t)br;
                result = REFILL_GOT_DATA;
            } else if (br == SDCARD_BUSY) {
                s_diag_sd_fail++;
            } else {
                if (br < 0) s_diag_sd_fail++;
                result = REFILL_EOF;
            }
        }
    }

    // Zero-pad guard bytes for libmad
    memset(s_decode_buffer + s_bytes_in_buffer, 0, MAD_BUFFER_GUARD);
    return result;
}

// ── The mixer's side: attach, detach, fade ──────────────────────────────────

// Stop the mixer pulling the stage and empty it (the next start fades in).
static void detach(void) {
    stage_lock();
    s_mixing = false;
    pcm_stage_reset(&s_stage);
    stage_unlock();
}

// The content's format, before a pre-fill (the refill copies whole frames).
static void stage_format(void) {
    stage_lock();
    pcm_stage_set_format(&s_stage, s_player.sample_rate, (uint8_t)s_pcm_channels);
    stage_unlock();
}

// Start the mixer pulling the stage, fading in.
static void attach(void) {
    stage_format();
    stage_lock();
    pcm_stage_fade_in(&s_stage);
    s_mixing = true;
    stage_unlock();
}

// The decoder reached the end of a non-looping stream: what is buffered is
// the tail, and running out of it is the end, not an underrun.
static void mark_eof(void) {
    s_eof = true;
    stage_lock();
    s_stage.eof = true;
    stage_unlock();
}

// A stream that reached its end finishes once the mixer has played the
// last staged frame, so isPlaying() stays true until the audio really ends.
static void finish_if_drained(void) {
    // pcm_stage_frames unlocked: benign, the mixer only shrinks avail, and
    // it runs on Core 1 like this update.
    if (!s_eof || ring_available() > 0 || pcm_stage_frames(&s_stage) > 0)
        return;
    detach();
    s_player.playing = false;
}

// Ramps the MP3 down to silence and waits until the mixer has rendered the
// whole ramp (PCM_STAGE_FADE_FRAMES frames: the next one or two refills),
// so detaching it afterwards cannot click. Returns at once when nothing
// would render the ramp (not mixing, or the output off), and after 50 ms
// at worst. The mixer renders it in the refill interrupt, whoever holds
// s_mp3_mutex: callers wait without it so that Core 1's update keeps the
// stage topped up, except a restart in place, which holds it so that
// nothing new reaches the stage before the old audio has faded out.
static void fade_out_and_wait(void) {
    if (!atomic_load(&s_stage_ready) || !audio_output_running())
        return;
    stage_lock();
    bool mixing = s_mixing;
    if (mixing)
        pcm_stage_fade_out(&s_stage);
    stage_unlock();
    for (int i = 0; mixing && i < 200 && s_stage.fade != PCM_FADE_SILENT; i++)
        sleep_us(250);
}

// ── End of stream / decode ──────────────────────────────────────────────────

// End of the file with no whole frame left: loop back to the start (at
// most once per update, so an empty or unreadable file cannot spin) or
// finish. Returns true when decoding should go on.
static bool end_of_stream(bool *rewound) {
    if (s_player.loop && !*rewound) {
        *rewound = true;
        s_file_pos = 0;
        s_bytes_in_buffer = 0;
        s_buffer_pos = 0;
        s_input_end = false;
        mad_stream_init(s_mad_stream);
        mad_frame_init(s_mad_frame);
        s_in_frame = false;
        mad_synth_init(s_mad_synth);
        return refill_decode_buffer() == REFILL_GOT_DATA;
    }
    if (!s_player.loop)
        mark_eof();
    return false;
}

// The decoder's side of the loop mark: the frame just decoded starts `at`
// bytes into the `valid` bytes the decode buffer held, the last the decoder
// took from the fed ring. The first frame at or after the mark is where
// the next pass begins (its first frame uses no bit reservoir, so the
// decoder needs nothing from before it).
static void fed_note_frame(uint32_t at, uint32_t valid) {
    if (atomic_load_explicit(&s_mark_state, memory_order_acquire) != MARK_ARMED)
        return;
    uint32_t pos = s_fed_taken - valid + at;
    if ((int32_t)(pos - s_mark_at) < 0)
        return;
    s_mark_frame = s_fed_frames;
    atomic_store_explicit(&s_mark_state, MARK_REACHED, memory_order_release);
}

// ── Decode: fill PCM ring buffer (called from main loop, NOT ISR) ───────────

// Writes the granule just synthesised (576 samples a channel) to the PCM ring.
static void write_granule(void) {
    struct mad_pcm *pcm = &s_mad_synth->pcm;
    s_pcm_channels = pcm->channels;
    unsigned int nsamples = pcm->length;
    if (s_fed_mode)
        s_fed_frames += nsamples;

    if (pcm->channels == 2) {
        // Stereo: samplesX is already interleaved [sample][2] int16_t
        ring_write((const uint8_t *)pcm->samplesX, nsamples * 2 * sizeof(int16_t));
    } else {
        // Mono: the left channel (samplesX[n][0]) packed in place to the
        // front of samplesX (sample n moves from byte 4n to 2n, so the
        // forward copy never overwrites one it has yet to read), then
        // one ring write. Writing each 2-byte sample on its own was 576
        // PIO PSRAM transfers per frame, each with its own lock, XIP
        // cache clean and DMA setup.
        int16_t *packed = &pcm->samplesX[0][0];
        for (unsigned int i = 1; i < nsamples; i++)
            packed[i] = pcm->samplesX[i][0];
        ring_write((const uint8_t *)packed, nsamples * sizeof(int16_t));
    }
}

// Decodes a granule at a time (issue #28): an MPEG-1 frame (44.1 kHz) is
// two, 1152 samples a channel; an MPEG-2 frame (22.05 kHz) one, 576. Each
// granule is decoded, synthesised and written to the ring before the next
// one starts, so the unit of work is half a stereo frame and a frame may
// stay in progress across updates (s_in_frame).
static void decode_fill_ring(void) {
    if (!s_player.playing || s_player.paused || s_eof || !s_mad_stream)
        return;
    if (!s_fed_mode && !s_file)
        return;

    // When to decode (mp3_sched.h): a burst of up to MP3_SCHED_BURST
    // granules when the ring is at or below half (whatever Core 0 does:
    // nothing starves), or ahead while a paced app's Core 0 waits for its
    // frame's deadline (os/core0_idle.h), so the decoder's sweep of the
    // shared XIP cache and QMI lands on Core 0's idle time. Fed mode (the
    // video player, which does not pace through perf) decodes on the low
    // ring only, as before. A call from Core 0 (play's pre-fill) never sees
    // a window.
    bool low = mp3_sched_low((uint32_t)ring_available(), PCM_RING_SIZE);
    bool ahead = !s_fed_mode &&
                 atomic_load_explicit(&s_decode_ahead, memory_order_relaxed);
    if (!low && !(ahead && core0_idle_left_us(time_us_64()) >= s_sched.frame_us))
        return;  // the ring is over half full and Core 0 is working

    s_diag_decode_runs++;
    // A restart in place needs one frame to start the new audio: more
    // would only keep the old audio playing longer (and risk its stage
    // running dry). Core 1 decodes on as soon as the restart ends.
    uint32_t max_frames = s_stage_held ? 1u : MP3_SCHED_BURST;
    uint32_t frames_decoded = 0;   // frames heard, finished in this update
    uint32_t granules = 0;         // granules decoded in this update
    int errors_this_update = 0;
    bool rewound = false;
    // Every pass either decodes a granule, gets new input, counts an error
    // (capped), or leaves: the loop is bounded within one update.
    while (frames_decoded < max_frames &&
           ring_free() >= MP3_GRANULE_SAMPLES * 2 * 2) {
        uint64_t t0 = time_us_64();
        if (!s_in_frame && s_stage_held && t0 + s_hold_frame_us > s_hold_deadline) {
            s_hold_late = true;  // the old audio could run out first
            break;
        }
        mp3_sched_why_t why = mp3_sched_next(&s_sched, low, granules,
                                             ahead ? core0_idle_left_us(t0) : 0);
        // A held restart finishes the frame it began (its deadline was
        // judged at the frame's start, against whole frames).
        if (why == MP3_SCHED_NONE && !(s_stage_held && s_in_frame))
            break;

        if (!s_in_frame) {
            // A new frame: its header and side information. Until it ends
            // the decode buffer stays put (libmad reads main_data from it).
            const uint8_t *base = s_decode_buffer + s_buffer_pos;
            uint32_t valid = (uint32_t)s_bytes_in_buffer;
            // (The guard bytes after the data are always zeroed by the refill.)
            mad_stream_buffer(s_mad_stream, base,
                              valid + (s_input_end ? MAD_BUFFER_GUARD : 0));

            int rc = mad_frame_decode_begin(s_mad_frame, s_mad_stream);
            // Track consumed bytes (on an error too)
            if (s_mad_stream->next_frame) {
                int consumed = (int)(s_mad_stream->next_frame - (s_decode_buffer + s_buffer_pos));
                if (consumed > 0 && consumed <= s_bytes_in_buffer) {
                    s_buffer_pos += consumed;
                    s_bytes_in_buffer -= consumed;
                }
            }
            if (rc != 0) {
                bool need_data = s_mad_stream->error == MAD_ERROR_BUFLEN ||
                                 (s_mad_stream->error == MAD_ERROR_LOSTSYNC &&
                                  s_bytes_in_buffer < 256);
                if (need_data) {
                    if (s_mad_stream->error != MAD_ERROR_BUFLEN)
                        s_diag_decode_errs++;
                    if (s_input_end) {  // the last frame is decoded
                        if (end_of_stream(&rewound))
                            continue;
                        break;
                    }
                    refill_t r = refill_decode_buffer();
                    if (r == REFILL_GOT_DATA)
                        continue;
                    if (r == REFILL_WAIT)
                        break;  // no new data yet: next tick, don't spin
                    s_input_end = true;  // decode what is left, then end
                    continue;
                }

                if (MAD_RECOVERABLE(s_mad_stream->error)) {
                    // libmad skips past the bad data: try the next frame, but a
                    // resync storm yields the core after MAX_ERRORS_PER_UPDATE.
                    s_diag_decode_errs++;
                    if (++errors_this_update >= MAX_ERRORS_PER_UPDATE)
                        break;
                    continue;
                }

                // Non-recoverable error: end the stream (what was decoded still plays)
                printf("[MP3] libmad error: 0x%04x\n", s_mad_stream->error);
                mark_eof();
                break;
            }

            if (s_fed_mode)
                fed_note_frame((uint32_t)(s_mad_stream->this_frame - base), valid);
            s_in_frame = true;
            s_gr_next = 0;
            s_gr_count = mad_frame_granules(s_mad_frame);
            s_frame_t0 = t0;
        }

        // One granule: decode, synthesise, and (unless it primes the
        // decoder) into the ring.
        unsigned int gr = s_gr_next++;
        bool ok = mad_frame_decode_granule(s_mad_frame, s_mad_stream, gr) == 0;
        uint64_t t_dec = time_us_64(), t_syn = t_dec;
        if (ok) {
            mad_synth_granule(s_mad_synth, s_mad_frame, gr);
            t_syn = time_us_64();
            if (s_prime_frames == 0)
                write_granule();
        }
        granules++;

        if (!ok || s_gr_next >= s_gr_count) {
            // The frame's end: keep the main_data the next frame draws on.
            s_in_frame = false;
            if (mad_frame_decode_end(s_mad_frame, s_mad_stream) != 0) {
                // A granule failed (its frame's rest is skipped).
                if (!MAD_RECOVERABLE(s_mad_stream->error)) {
                    printf("[MP3] libmad error: 0x%04x\n", s_mad_stream->error);
                    mark_eof();
                    break;
                }
                s_diag_decode_errs++;
                if (++errors_this_update >= MAX_ERRORS_PER_UPDATE)
                    break;
                continue;
            }
            s_diag_decode_frames++;
            if (s_prime_frames > 0) {   // it primed the decoder: not heard
                s_prime_frames--;
                if (s_stage_held) {
                    // The frame heard next takes as long, and its ring write.
                    uint32_t us = (uint32_t)(time_us_64() - s_frame_t0);
                    if (us > s_hold_frame_us) s_hold_frame_us = us;
                }
                continue;
            }
            frames_decoded++;
        }
        uint64_t t_out = time_us_64();
        s_prof_dec_us += (uint32_t)(t_dec - t0);
        s_prof_syn_us += (uint32_t)(t_syn - t_dec);
        s_prof_out_us += (uint32_t)(t_out - t_syn);

        // Top up the staging buffer between granules: a decode burst can
        // run 10-90ms on Core 1 (libmad resync storms, flash-cold synth), far
        // longer than the staging cushion, so refilling only once per update
        // lets the mixer drain the stage dry and crackles.
        refill_staging_buf();

        if (why == MP3_SCHED_IDLE) {
            // A granule that finished inside Core 0's window ran
            // uncontended: its time refines the estimate the next window
            // is judged by.
            uint64_t t1 = time_us_64();
            s_sched_idle_frames++;
            if (core0_idle_left_us(t1) > 0)
                mp3_sched_note(&s_sched, (uint32_t)(t1 - t0));
            else
                s_sched_overran++;
        } else {
            s_sched_low_frames++;
        }
    }
}

// ── Helper to skip ID3v2 tags ───────────────────────────────────────────────
static int skip_id3v2tag(struct mad_stream *stream) {
    const unsigned char *ptr = stream->buffer;
    size_t len = stream->bufend - stream->buffer;

    if (len < 10) return 0;

    // ID3v2 header: "ID3" (3 bytes), version (2 bytes), flags (1 byte), size (4 bytes)
    if (ptr[0] == 'I' && ptr[1] == 'D' && ptr[2] == '3') {
        // Size is 4 syncsafe bytes (msb is always 0)
        unsigned long size = 
            ((unsigned long)(ptr[6] & 0x7f) << 21) |
            ((unsigned long)(ptr[7] & 0x7f) << 14) |
            ((unsigned long)(ptr[8] & 0x7f) << 7) |
            ((unsigned long)(ptr[9] & 0x7f));

        size += 10; // header size

        // If footer flag is set (bit 4 of flags byte 5), there's a 10-byte footer
        if (ptr[5] & 0x10) size += 10;

        if (size > len) size = len; // sanity check

        mad_stream_skip(stream, size);
        return (int)size;
    }

    return 0;
}

// Reads the start of the file into the decode buffer and resets the
// decoder and the PCM ring: where every play starts. A blocking read, on
// Core 0 with s_mp3_mutex held (Core 1's update is skipping meanwhile).
static bool rewind_locked(void) {
    if (!s_file || !sdcard_fseek(s_file, 0))
        return false;
    int rd = sdcard_fread(s_file, s_decode_buffer,
                          MP3_DECODE_BUFFER_SIZE - MAD_BUFFER_GUARD);
    if (rd <= 0)
        return false;
    s_bytes_in_buffer = rd;
    s_buffer_pos = 0;
    s_file_pos = (uint32_t)rd;
    memset(s_decode_buffer + rd, 0, MAD_BUFFER_GUARD);
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    s_in_frame = false;
    mad_synth_init(s_mad_synth);
    s_ring_rd = s_ring_wr = 0;
    s_eof = false;
    s_input_end = false;
    mp3_sched_init(&s_sched);  // a new file: its own decode time
    return true;
}

// Points the frame's arrays at their homes: the PSRAM block, and on the
// device the hottest of them at SRAM (s_mp3_sram).
static void bind_frame(void) {
#if defined(PICODECK_MP3_SRAM)
    s_mad_frame->sbsample = s_mp3_sram.sbsample;
    s_mad_frame->xr_raw = s_mp3_sram.xr;
    s_mad_frame->overlap = s_mad_frame_mem->overlap;
    s_mad_frame->tmp = s_mad_frame_mem->tmp;
#else
    mad_frame_bind(s_mad_frame, s_mad_frame_mem);
#endif
}

// ── Public API ──────────────────────────────────────────────────────────────

void mp3_player_reset(void) {
    if (!s_initialized) return;
    if (s_fed_mode) mp3_player_stop_fed();
    mutex_enter_blocking(&s_mp3_mutex);
    detach();
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }
    memset(&s_player, 0, sizeof(s_player));
    s_player.volume = 100;
    stage_lock();
    s_stage.vol_scale = 256;
    stage_unlock();
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_pcm_channels = 2;
    s_eof = false;
    s_input_end = false;
    s_ring_rd = s_ring_wr = 0;
    if (s_mad_stream) mad_stream_init(s_mad_stream);
    if (s_mad_frame)  mad_frame_init(s_mad_frame);
    s_in_frame = false;
    if (s_mad_synth)  mad_synth_init(s_mad_synth);
    mutex_exit(&s_mp3_mutex);
}

void mp3_player_deinit(void) {
    if (!s_initialized) return;
    mutex_enter_blocking(&s_mp3_mutex);
    detach();
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }
    if (s_mad_stream) { umm_free(s_mad_stream); s_mad_stream = NULL; }
    if (s_mad_frame)  { umm_free(s_mad_frame); s_mad_frame = NULL; }
    if (s_mad_frame_mem) { umm_free(s_mad_frame_mem); s_mad_frame_mem = NULL; }
#if !defined(PICODECK_MP3_SRAM)
    if (s_mad_synth)  umm_free(s_mad_synth);
#endif
    s_mad_synth = NULL;
    if (s_decode_buffer) { umm_free(s_decode_buffer); s_decode_buffer = NULL; }
    s_in_frame = false;
    if (s_pcm_ring)   { umm_free(s_pcm_ring); s_pcm_ring = NULL; }
    s_use_pio_psram = false;
    s_initialized = false;
    mutex_exit(&s_mp3_mutex);
}

bool mp3_player_init(void) {
    if (s_initialized) return true;

    mutex_init(&s_mp3_mutex);
    if (!atomic_load(&s_stage_ready)) {
        critical_section_init_with_lock_num(&s_stage_cs, next_striped_spin_lock_num());
        pcm_stage_init(&s_stage, s_staging_buf, STAGING_BUF_SIZE, AUDIO_OUT_RATE);
        atomic_store_explicit(&s_stage_ready, true, memory_order_release);
    }
    printf("[MP3] Initializing libmad decoder...\n");

    if (!s_mad_stream) {
        s_mad_stream = umm_malloc(sizeof(struct mad_stream));
        if (!s_mad_stream) { printf("[MP3] FAILED to alloc mad_stream\n"); return false; }
    }
    if (!s_mad_frame) {
        s_mad_frame = umm_malloc(sizeof(struct mad_frame));
        if (!s_mad_frame) { printf("[MP3] FAILED to alloc mad_frame\n"); return false; }
    }
    if (!s_mad_frame_mem) {
        s_mad_frame_mem = umm_malloc(sizeof(*s_mad_frame_mem));
        if (!s_mad_frame_mem) { printf("[MP3] FAILED to alloc mad_frame_mem\n"); return false; }
    }
    bind_frame();
    if (!s_mad_synth) {
#if defined(PICODECK_MP3_SRAM)
        s_mad_synth = &s_mp3_sram.synth;
#else
        s_mad_synth = umm_malloc(sizeof(struct mad_synth));
#endif
        if (!s_mad_synth) { printf("[MP3] FAILED to alloc mad_synth\n"); return false; }
    }
    if (!s_decode_buffer) {
        s_decode_buffer = umm_malloc(MP3_DECODE_BUFFER_SIZE);
        if (!s_decode_buffer) { printf("[MP3] FAILED to alloc the input buffer\n"); return false; }
    }

    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    s_in_frame = false;
    mad_synth_init(s_mad_synth);

    printf("[MP3] libmad structs: stream=%u frame=%u+%u synth=%u bytes\n",
           (unsigned)sizeof(struct mad_stream),
           (unsigned)sizeof(struct mad_frame),
           (unsigned)sizeof(*s_mad_frame_mem),
           (unsigned)sizeof(struct mad_synth));

    if (!s_pcm_ring && !s_use_pio_psram) {
        if (pio_psram_available()) {
            s_use_pio_psram = true;
            s_pio_psram_base = PIO_PSRAM_MP3_RING_BASE;
            printf("[MP3] PCM ring buffer → PIO PSRAM @ 0x%lx (%d bytes, 8KB bulk xfers)\n",
                   (unsigned long)s_pio_psram_base, PCM_RING_SIZE);
        } else {
            s_pcm_ring = umm_malloc(PCM_RING_SIZE);
            if (!s_pcm_ring) {
                printf("[MP3] FAILED to alloc ring buffer\n");
                return false;
            }
            s_use_pio_psram = false;
            printf("[MP3] PCM ring buffer → QMI PSRAM (umm_malloc, %d bytes)\n", PCM_RING_SIZE);
        }
    }

    memset(&s_player, 0, sizeof(s_player));
    s_player.volume = 100;
    stage_lock();
    s_stage.vol_scale = 256;
    stage_unlock();
    s_initialized = true;

    return true;
}

mp3_player_t *mp3_player_create(void) {
    if (!s_initialized) {
        mp3_player_init();
    }
    return &s_player;
}

void mp3_player_destroy(mp3_player_t *player) {
    (void)player;
    mp3_player_stop(player);
}

bool mp3_player_load(mp3_player_t *player, const char *path) {
    if (!player || !path) return false;

    // Video audio and file playback share the decoder: stop fed mode first.
    if (s_fed_mode) mp3_player_stop_fed();
    fade_out_and_wait();

    mutex_enter_blocking(&s_mp3_mutex);
    detach();
    player->playing = false;
    player->paused = false;
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }

    s_file = sdcard_fopen(path, "rb");
    if (!s_file) {
        printf("mp3_player: failed to open %s\n", path);
        mutex_exit(&s_mp3_mutex);
        return false;
    }
    if (!rewind_locked()) {
        sdcard_fclose(s_file); s_file = NULL;
        mutex_exit(&s_mp3_mutex);
        return false;
    }

    // Probe the first frame header (past an ID3v2 tag) for the format, then
    // put the decoder back at the start of the buffer.
    int have = s_bytes_in_buffer;
    mad_stream_buffer(s_mad_stream, s_decode_buffer, have + MAD_BUFFER_GUARD);
    skip_id3v2tag(s_mad_stream);
    bool ok = mad_header_decode(&s_mad_frame->header, s_mad_stream) == 0;
    if (ok) {
        player->sample_rate = s_mad_frame->header.samplerate;
        player->channels    = MAD_NCHANNELS(&s_mad_frame->header);
        player->length      = 0;
        s_pcm_channels      = player->channels;
    }
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    s_in_frame = false;
    mad_synth_init(s_mad_synth);
    s_buffer_pos = 0;
    s_bytes_in_buffer = have;
    if (!ok) {
        printf("mp3_player: not an MP3 file (%s)\n", path);
        sdcard_fclose(s_file); s_file = NULL;
        mutex_exit(&s_mp3_mutex);
        return false;
    }

    printf("mp3_player: loaded %s (%lu Hz, %lu ch)\n", path,
           (unsigned long)player->sample_rate, (unsigned long)player->channels);
    mutex_exit(&s_mp3_mutex);
    return true;
}

// Plays from the start of the file, including after it finished. A playing
// stream fades out first, so the restart cannot click.
bool mp3_player_play(mp3_player_t *player, uint8_t repeat_count) {
    (void)repeat_count;
    if (!player || !s_file) return false;
    fade_out_and_wait();

    mutex_enter_blocking(&s_mp3_mutex);
    detach();
    bool ok = rewind_locked();
    player->playing = ok;
    player->paused  = false;
    if (ok) {
        // Pre-fill before the mixer sees it, so it starts with staged audio.
        stage_format();
        decode_fill_ring();
        refill_staging_buf();
        attach();
    }
    mutex_exit(&s_mp3_mutex);
    if (ok)
        audio_output_ensure_running();
    return ok;
}

void mp3_player_stop(mp3_player_t *player) {
    if (!player) return;

    // If in fed mode, delegate to stop_fed
    if (s_fed_mode) {
        mp3_player_stop_fed();
        return;
    }

    fade_out_and_wait();
    mutex_enter_blocking(&s_mp3_mutex);
    detach();
    player->playing  = false;
    player->paused   = false;
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;
    s_eof = false;
    s_input_end = false;
    s_in_frame = false;
    mutex_exit(&s_mp3_mutex);
}

void mp3_player_pause(mp3_player_t *player) {
    if (!player || !player->playing || player->paused) return;
    fade_out_and_wait();
    mutex_enter_blocking(&s_mp3_mutex);
    player->paused = true;
    stage_lock();
    s_mixing = false;      // the staged audio stays for the resume
    stage_unlock();
    mutex_exit(&s_mp3_mutex);
}

void mp3_player_resume(mp3_player_t *player) {
    if (!player || !player->paused || !player->playing) return;
    mutex_enter_blocking(&s_mp3_mutex);
    player->paused = false;
    attach();              // fades in from the silence the pause left
    mutex_exit(&s_mp3_mutex);
    audio_output_ensure_running();
}

bool mp3_player_is_playing(const mp3_player_t *player) {
    return player && player->playing && !player->paused;
}

uint32_t mp3_player_get_position(const mp3_player_t *player) {
    if (!player) return 0;
    return s_stage.frames_played;
}

uint32_t mp3_player_get_length(const mp3_player_t *player) {
    if (!player) return 0;
    return player->length;
}

void mp3_player_set_volume(mp3_player_t *player, uint8_t volume) {
    if (!player) return;
    if (volume > 100) volume = 100;
    player->volume = volume;
    if (!atomic_load(&s_stage_ready)) return;
    stage_lock();
    s_stage.vol_scale = (uint32_t)volume * 256 / 100;
    stage_unlock();
}

uint8_t mp3_player_get_volume(const mp3_player_t *player) {
    if (!player) return 0;
    return player->volume;
}

uint32_t mp3_player_get_sample_rate(const mp3_player_t *player) {
    if (!player) return 0;
    return player->sample_rate;
}

void mp3_player_set_loop(mp3_player_t *player, bool loop) {
    if (!player) return;
    player->loop = loop;
}

// ── Fed mode API (video player audio) ─────────────────────────────────────────

// Records a restart of a playing session: its gap, and for one in place
// the decode behind the old audio and the old audio it had left. Core 0.
static void restart_note(bool in_place, bool late, uint64_t gap_us,
                         uint64_t preroll_us, uint64_t margin_us) {
    mp3_fed_restart_stats_t *st = &s_restart_stats;
    uint32_t gap = gap_us > UINT32_MAX ? UINT32_MAX : (uint32_t)gap_us;
    st->gap_us = gap;
    if (gap > st->gap_max_us) st->gap_max_us = gap;
    if (!in_place) {
        st->fallbacks++;
        return;
    }
    uint32_t pre = preroll_us > UINT32_MAX ? UINT32_MAX : (uint32_t)preroll_us;
    uint32_t margin = margin_us > UINT32_MAX ? UINT32_MAX : (uint32_t)margin_us;
    if (pre > st->preroll_max_us) st->preroll_max_us = pre;
    if (st->restarts == 0 || margin < st->margin_min_us) st->margin_min_us = margin;
    st->restarts++;
    if (late) st->late++;
}

void mp3_player_fed_restart_stats(mp3_fed_restart_stats_t *out, bool reset) {
    *out = s_restart_stats;
    if (reset)
        memset(&s_restart_stats, 0, sizeof s_restart_stats);
}

bool mp3_player_start_fed(uint32_t sample_rate, uint16_t channels) {
    if (!s_initialized) {
        if (!mp3_player_init()) return false;
    }
    // A playing session restarted here (a seek's fallback) is silent from
    // its fade-out until start_fed_output starts the new audio.
    bool audible = s_fed_mode && s_mixing && audio_output_running();
    fade_out_and_wait();
    uint64_t silent_at = time_us_64();

    mutex_enter_blocking(&s_mp3_mutex);
    detach();
    s_fallback_silent_at = audible ? silent_at : 0;
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }

    // Init fed ring in QMI PSRAM
    if (!s_fed_ring_buf) {
        s_fed_ring_buf = (uint8_t *)umm_malloc(FED_RING_SIZE);
        if (!s_fed_ring_buf) {
            printf("[MP3] FAILED to alloc fed ring (%d bytes)\n", FED_RING_SIZE);
            mutex_exit(&s_mp3_mutex);
            return false;
        }
    }
    s_fed_mode = true;
    s_fed_wr = 0;
    s_fed_rd = 0;
    s_prime_frames = 0;
    s_fed_fed = s_fed_taken = s_fed_frames = 0;
    atomic_store(&s_mark_state, MARK_NONE);
    atomic_store(&s_fed_end, false);

    // Reset decode state
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;
    s_eof = false;
    s_input_end = false;
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    s_in_frame = false;
    mad_synth_init(s_mad_synth);

    // Configure player state
    memset(&s_player, 0, sizeof(s_player));
    s_player.sample_rate = sample_rate;
    s_player.channels = channels;
    s_player.playing = true;
    s_player.volume = 100;
    s_pcm_channels = (int)channels;
    stage_lock();
    s_stage.vol_scale = 256;
    stage_unlock();
    // From here Core 1's update decodes what the video player feeds and
    // stages it (playing, not yet mixed): in the content's frame size.
    stage_format();

    printf("[MP3] Fed mode started: %u Hz, %u ch\n",
           (unsigned)sample_rate, (unsigned)channels);
    mutex_exit(&s_mp3_mutex);
    return true;
}

// Keeps the stage: since start_fed, Core 1's update has been decoding the
// fed data and moving it into the stage (advancing the PCM ring), so
// emptying it here would drop that audio (26-100 ms: the sound would lead
// the picture). Nothing mixes it until attach().
void mp3_player_start_fed_output(void) {
    if (!s_fed_mode || !s_initialized) return;
    mutex_enter_blocking(&s_mp3_mutex);
    if (s_mixing) {        // already started
        mutex_exit(&s_mp3_mutex);
        return;
    }
    stage_format();
    decode_fill_ring();    // compressed data from the fed ring into the PCM ring
    refill_staging_buf();
    attach();
    if (s_fallback_silent_at) {
        restart_note(false, false, time_us_64() - s_fallback_silent_at, 0, 0);
        s_fallback_silent_at = 0;
    }
    mutex_exit(&s_mp3_mutex);
    audio_output_ensure_running();
}

bool mp3_player_restart_fed(const uint8_t *data, uint32_t len, bool mid_stream) {
    if (!s_initialized || !data || len == 0)
        return false;
    mutex_enter_blocking(&s_mp3_mutex);
    // The old audio has to be playing to play on; otherwise the caller
    // restarts through start_fed. (finish_if_drained, which ends a
    // finished stream, runs under this mutex too.)
    if (!s_fed_mode || !s_fed_ring_buf || !s_player.playing ||
        s_player.paused || !s_mixing || !audio_output_running()) {
        mutex_exit(&s_mp3_mutex);
        return false;
    }
    // The old audio left to play on is what the stage holds: as much as
    // it takes (the refill keeps it only over half, ~23 ms at 44.1 kHz
    // stereo, and less while Core 1 is mid-burst; the decode behind it
    // takes ~22 ms).
    stage_fill();
    uint64_t t0 = time_us_64();
    stage_lock();
    uint64_t margin_us = (uint64_t)pcm_stage_frames(&s_stage) * 1000000u / s_stage.rate;
    stage_unlock();

    // Everything behind the stage starts again at the new data, while the
    // stage plays the old audio on (Core 1's update is locked out: nothing
    // refills it). The decode stops at the new audio's first frame, or
    // before a frame that might outlast the old audio (s_hold_deadline).
    s_stage_held = true;
    s_hold_late = false;
    s_hold_frame_us = RESTART_FRAME_US;
    s_hold_deadline = t0 + (margin_us > RESTART_GUARD_US ? margin_us - RESTART_GUARD_US : 0);
    s_fed_wr = 0;
    s_fed_rd = 0;
    s_fed_fed = s_fed_taken = s_fed_frames = 0;
    atomic_store(&s_mark_state, MARK_NONE);
    atomic_store(&s_fed_end, false);
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;
    s_eof = false;
    s_input_end = false;
    s_prime_frames = mid_stream ? 1 : 0;
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    s_in_frame = false;
    mad_synth_init(s_mad_synth);
    mp3_player_feed(data, len);
    decode_fill_ring();    // (primed,) the new audio's first frame, into the PCM ring
    uint64_t t1 = time_us_64();

    // The switch: the next render ramps the old audio down; then the stage
    // empties (the position restarts) and takes the new audio from its
    // first frame, and the render after fades it in.
    fade_out_and_wait();
    uint64_t t2 = time_us_64();
    bool late = s_hold_late;
    if (late) {
        // The old audio has faded out before it ran dry: decode the rest of
        // the way to the new audio's first frame, as a restart from silence
        // would (start_fed then three frames' pre-roll).
        s_hold_deadline = UINT64_MAX;
        decode_fill_ring();
    }
    stage_lock();
    pcm_stage_reset(&s_stage);
    stage_unlock();
    s_stage_held = false;
    refill_staging_buf();
    attach();
    uint64_t t3 = time_us_64();
    mutex_exit(&s_mp3_mutex);

    // The old audio stopped when its fade-out ended, or earlier if its
    // stage ran dry during the decode.
    uint64_t stopped = t0 + margin_us < t2 ? t0 + margin_us : t2;
    restart_note(true, late, t3 - stopped, t1 - t0, margin_us);
    printf("[MP3] Fed restart: gap %lu us, decode %lu us behind %lu us of old audio%s\n",
           (unsigned long)(t3 - stopped), (unsigned long)(t1 - t0),
           (unsigned long)margin_us, late ? " LATE" : "");
    return true;
}

uint32_t mp3_player_feed(const uint8_t *data, uint32_t len) {
    if (!s_fed_ring_buf) return 0;
    uint32_t free_space = fed_ring_free();
    uint32_t to_write = (len < free_space) ? len : free_space;
    if (to_write == 0) return 0;

    uint32_t wr = atomic_load_explicit(&s_fed_wr, memory_order_relaxed);
    uint32_t to_end = FED_RING_SIZE - wr;
    if (to_write <= to_end) {
        memcpy(s_fed_ring_buf + wr, data, to_write);
    } else {
        memcpy(s_fed_ring_buf + wr, data, to_end);
        memcpy(s_fed_ring_buf, data + to_end, to_write - to_end);
    }
    atomic_store_explicit(&s_fed_wr, (wr + to_write) % FED_RING_SIZE, memory_order_release);
    s_fed_fed += to_write;
    // More data after an end: not the end (a video's loop switched on late).
    atomic_store_explicit(&s_fed_end, false, memory_order_release);
    return to_write;
}

void mp3_player_fed_end(void) {
    if (s_fed_mode)
        atomic_store_explicit(&s_fed_end, true, memory_order_release);
}

void mp3_player_fed_mark(void) {
    if (!s_fed_mode) return;
    s_mark_at = s_fed_fed;
    atomic_store_explicit(&s_mark_state, MARK_ARMED, memory_order_release);
}

bool mp3_player_fed_mark_reached(int32_t *frames) {
    if (atomic_load_explicit(&s_mark_state, memory_order_acquire) != MARK_REACHED)
        return false;
    // frames_played: the mixer's count, one aligned word, from the same
    // start_fed as s_fed_frames (ring and stage pass every frame on).
    *frames = (int32_t)(s_stage.frames_played - s_mark_frame);
    atomic_store_explicit(&s_mark_state, MARK_NONE, memory_order_relaxed);
    return true;
}

void mp3_player_stop_fed(void) {
    if (!s_fed_mode) return;
    fade_out_and_wait();
    mutex_enter_blocking(&s_mp3_mutex);
    detach();
    s_player.playing = false;
    s_fed_mode = false;
    s_fed_wr = 0;
    s_fed_rd = 0;
    s_prime_frames = 0;
    s_fallback_silent_at = 0;
    atomic_store(&s_mark_state, MARK_NONE);
    atomic_store(&s_fed_end, false);
    if (s_fed_ring_buf) { umm_free(s_fed_ring_buf); s_fed_ring_buf = NULL; }
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;
    s_eof = false;
    s_input_end = false;
    s_in_frame = false;
    printf("[MP3] Fed mode stopped\n");
    mutex_exit(&s_mp3_mutex);
}

uint32_t mp3_player_feed_space(void) {
    return fed_ring_free();
}

bool mp3_player_is_fed_mode(void) {
    return s_fed_mode;
}

void mp3_player_update(void) {
    if (!s_initialized) return;
    if (!mutex_try_enter(&s_mp3_mutex, NULL)) { s_diag_skip_mutex++; return; }

    uint64_t upd_t0 = time_us_64();
    s_diag_updates++;

    if (s_player.playing && !s_player.paused) {
        refill_staging_buf();
        decode_fill_ring();
        finish_if_drained();
    }

    uint32_t upd_us = (uint32_t)(time_us_64() - upd_t0);
    s_diag_total_us += upd_us;
    if (upd_us > s_diag_max_us) s_diag_max_us = upd_us;
    mutex_exit(&s_mp3_mutex);
}

void __time_critical_func(mp3_player_mix)(int32_t *l, int32_t *r, int frames) {
    if (!atomic_load_explicit(&s_stage_ready, memory_order_acquire))
        return;
    if (!s_mixing)         // the common case: no lock taken for nothing
        return;
    stage_lock();
    if (s_mixing)
        pcm_stage_mix(&s_stage, l, r, frames);
    stage_unlock();
}

#if !defined(PICODECK_SIMULATOR) && !defined(PICODECK_HOST_TEST)
// ── mp3bench (dev command): a frame's decode cost, by phase (issue #28) ────
// Decodes a file's frames on the calling core (a dev command: Core 0, its
// PSRAM stack) the way the player does, a granule at a time, and prints per
// frame the time and the XIP cache accesses and misses of each phase: the
// decode (header, side information, Huffman, IMDCT), the synthesis, and the
// PCM ring write. Run it at the launcher with nothing playing: the cache
// counters count both cores, and `flags` moves state into SRAM borrowed
// from the display's back buffer (the launcher redraws it), to see what
// placing it there would save:
#include "hardware/structs/xip.h"
#include "display.h"
#include "../os/xip_stats.h"

#define BENCH_SRAM_STREAM   1u  // struct mad_stream (the bit reservoir)
#define BENCH_SRAM_FRAME    2u  // all of the frame's arrays
#define BENCH_SRAM_SYNTH    4u  // struct mad_synth (filterbank, PCM)
#define BENCH_NO_OUT       16u  // no PCM ring write
#define BENCH_SRAM_XR      64u  // the frame's xr_raw
#define BENCH_SRAM_OVL    128u  // its overlap
#define BENCH_SRAM_SBS    256u  // its sbsample

typedef struct { uint64_t us, acc, miss; } bench_ph_t;

static inline void bench_ctr_clear(void) {
    xip_ctrl_hw->ctr_acc = 0;   // write to clear
    xip_ctrl_hw->ctr_hit = 0;
}

// Adds the phase that began at t0 and restarts the counters.
static inline uint64_t bench_ph_add(bench_ph_t *p, uint64_t t0) {
    uint32_t hit = xip_ctrl_hw->ctr_hit;
    uint32_t acc = xip_ctrl_hw->ctr_acc;
    uint64_t t1 = time_us_64();
    p->us += t1 - t0;
    p->acc += acc;
    p->miss += acc > hit ? acc - hit : 0;
    bench_ctr_clear();
    return t1;
}

void mp3_player_bench(const char *path, uint32_t frames, uint32_t flags) {
    if (!s_initialized && !mp3_player_init()) {
        printf("[DEV] mp3bench: error=init\n");
        return;
    }
    if (s_player.playing || s_fed_mode) {
        printf("[DEV] mp3bench: error=busy\n");
        return;
    }
    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) {
        printf("[DEV] mp3bench: error=open\n");
        return;
    }
    int size = sdcard_fsize_handle(f);
    if (size <= 0) size = 0;
    if (size > 512 * 1024) size = 512 * 1024;
    uint8_t *data = umm_malloc((size_t)size + MAD_BUFFER_GUARD);
    int got = data ? sdcard_fread(f, data, size) : -1;
    sdcard_fclose(f);
    if (got <= 0) {
        if (data) umm_free(data);
        printf("[DEV] mp3bench: error=read\n");
        return;
    }
    memset(data + got, 0, MAD_BUFFER_GUARD);

    mutex_enter_blocking(&s_mp3_mutex);
    xip_stats_stop();   // its harvest timer would clear the counters
    // The SRAM: the back buffer, aligned (the framebuffers are only 2-byte
    // aligned; libmad's state needs 8): the stream, the frame's arrays,
    // the synthesis state.
    uint8_t *sram = (uint8_t *)(((uintptr_t)display_get_back_buffer() + 31u) & ~(uintptr_t)31u);
    _Static_assert(sizeof(struct mad_stream) <= 4096, "bench layout");
    _Static_assert(sizeof(struct mad_frame_mem) <= 20480, "bench layout");
    _Static_assert(sizeof(struct mad_synth) <= 8192, "bench layout");
    struct mad_stream *st = (flags & BENCH_SRAM_STREAM)
        ? (struct mad_stream *)sram : s_mad_stream;
    struct mad_frame *fr = s_mad_frame;
    struct mad_frame_mem *fm = (struct mad_frame_mem *)(sram + 4096);
    if (flags & BENCH_SRAM_FRAME)
        mad_frame_bind(fr, fm);
    if (flags & BENCH_SRAM_XR)  fr->xr_raw = fm->xr_raw;
    if (flags & BENCH_SRAM_OVL) fr->overlap = fm->overlap;
    if (flags & BENCH_SRAM_SBS) fr->sbsample = fm->sbsample;
    struct mad_synth *sy = (flags & BENCH_SRAM_SYNTH)
        ? (struct mad_synth *)(sram + 4096 + 20480) : s_mad_synth;
    mad_stream_init(st);
    mad_frame_init(fr);
    mad_synth_init(sy);
    mad_stream_buffer(st, data, (unsigned long)got + MAD_BUFFER_GUARD);

    bench_ph_t dec = {0}, syn = {0}, out = {0};
    uint32_t done = 0, errs = 0, loops = 0, nch = 0, rate = 0;
    uint64_t t_start = time_us_64();
    while (done < frames && loops < 1000 && errs < 1000) {
        bench_ctr_clear();
        uint64_t t = time_us_64();
        int rc = mad_frame_decode_begin(fr, st);
        if (rc != 0) {
            if (st->error == MAD_ERROR_BUFLEN) {   // the end: loop the file
                loops++;
                mad_stream_buffer(st, data, (unsigned long)got + MAD_BUFFER_GUARD);
                continue;
            }
            errs++;
            if (!MAD_RECOVERABLE(st->error)) break;
            continue;
        }
        t = bench_ph_add(&dec, t);
        unsigned ngr = mad_frame_granules(fr);
        for (unsigned gr = 0; gr < ngr && rc == 0; gr++) {
            rc = mad_frame_decode_granule(fr, st, gr);
            t = bench_ph_add(&dec, t);
            if (rc != 0) break;
            mad_synth_granule(sy, fr, gr);
            t = bench_ph_add(&syn, t);
            if (!(flags & BENCH_NO_OUT) && s_use_pio_psram) {
                pio_psram_write(s_pio_psram_base, (const uint8_t *)sy->pcm.samplesX,
                                sy->pcm.length * 2u * sizeof(int16_t));
                t = bench_ph_add(&out, t);
            }
        }
        rc = mad_frame_decode_end(fr, st);
        bench_ph_add(&dec, t);
        nch = sy->pcm.channels;
        rate = sy->pcm.samplerate;
        if (rc != 0) { errs++; continue; }
        done++;
    }
    uint64_t total = time_us_64() - t_start;
    bind_frame();
    // The live decoder starts over at its next play().
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    s_in_frame = false;
    mad_synth_init(s_mad_synth);
    mutex_exit(&s_mp3_mutex);
    umm_free(data);

    uint32_t n = done ? done : 1;
    printf("[DEV] mp3bench: file=%s frames=%lu errs=%lu flags=%lu rate=%lu ch=%lu "
           "total_us=%llu dec_us=%llu syn_us=%llu out_us=%llu "
           "dec_acc=%llu dec_miss=%llu syn_acc=%llu syn_miss=%llu "
           "out_acc=%llu out_miss=%llu sys_khz=%lu\n", path,
           (unsigned long)done, (unsigned long)errs, (unsigned long)flags,
           (unsigned long)rate, (unsigned long)nch,
           (unsigned long long)(total / n),
           (unsigned long long)(dec.us / n), (unsigned long long)(syn.us / n),
           (unsigned long long)(out.us / n),
           (unsigned long long)(dec.acc / n), (unsigned long long)(dec.miss / n),
           (unsigned long long)(syn.acc / n), (unsigned long long)(syn.miss / n),
           (unsigned long long)(out.acc / n), (unsigned long long)(out.miss / n),
           (unsigned long)(clock_get_hz(clk_sys) / 1000u));
}
#endif
