#include "mp3_player.h"
#include "audio.h"
#include "pcm_stage.h"
#include "sdcard.h"
#include "pio_psram.h"
#include "pico/platform.h"
#include "pico/critical_section.h"
#include "pico/mutex.h"
#include "pico/time.h"

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
static struct mad_synth  *s_mad_synth  = NULL;
static sdfile_t    s_file = NULL;
static uint32_t    s_file_pos = 0;  // next byte of s_file to decode (Core 1
                                    // reads at it: no blocking fseek there)
static uint8_t     s_decode_buffer[MP3_DECODE_BUFFER_SIZE] __attribute__((aligned(4)));
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
static void refill_staging_buf(void) {
    if (s_stage.avail >= STAGING_BUF_SIZE / 2)
        return;

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
// at worst. Call WITHOUT s_mp3_mutex: Core 1's update keeps running.
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
static void decode_fill_ring(void) {
    if (!s_player.playing || s_player.paused || s_eof || !s_mad_stream)
        return;
    if (!s_fed_mode && !s_file)
        return;

    // Batch decode: only decode when ring buffer is below 50% capacity,
    // then decode up to 3 frames to refill quickly.  This creates bursty
    // PSRAM access (~10ms decode burst, ~30-40ms idle) instead of constant
    // pressure every 5ms, reducing QMI contention with Core 0's XIP cache.
    size_t avail = ring_available();
    if (avail > PCM_RING_SIZE / 2)
        return;  // ring buffer is >50% full, skip this cycle

    s_diag_decode_runs++;
    int max_frames = 3;
    int frames_decoded = 0;
    int errors_this_update = 0;
    bool rewound = false;
    // Every pass either decodes a frame, gets new input, counts an error
    // (capped), or leaves: the loop is bounded within one update.
    while (frames_decoded < max_frames && ring_free() >= 1152 * 2 * 2) {
        const uint8_t *base = s_decode_buffer + s_buffer_pos;
        uint32_t valid = (uint32_t)s_bytes_in_buffer;
        // (The guard bytes after the data are always zeroed by the refill.)
        mad_stream_buffer(s_mad_stream, base,
                          valid + (s_input_end ? MAD_BUFFER_GUARD : 0));

        if (mad_frame_decode(s_mad_frame, s_mad_stream) != 0) {
            // Track consumed bytes
            if (s_mad_stream->next_frame) {
                int consumed = (int)(s_mad_stream->next_frame - (s_decode_buffer + s_buffer_pos));
                if (consumed > 0 && consumed <= s_bytes_in_buffer) {
                    s_buffer_pos += consumed;
                    s_bytes_in_buffer -= consumed;
                }
            }

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

        // Track consumed bytes on success
        if (s_mad_stream->next_frame) {
            int consumed = (int)(s_mad_stream->next_frame - (s_decode_buffer + s_buffer_pos));
            if (consumed > 0 && consumed <= s_bytes_in_buffer) {
                s_buffer_pos += consumed;
                s_bytes_in_buffer -= consumed;
            }
        }

        if (s_fed_mode)
            fed_note_frame((uint32_t)(s_mad_stream->this_frame - base), valid);

        mad_synth_frame(s_mad_synth, s_mad_frame);
        frames_decoded++;
        s_diag_decode_frames++;

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

        // Top up the staging buffer between frames: a 3-frame decode burst can
        // run 10-90ms on Core 1 (libmad resync storms, flash-cold synth), far
        // longer than the staging cushion, so refilling only once per update
        // lets the mixer drain the stage dry and crackles.
        refill_staging_buf();
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
    mad_synth_init(s_mad_synth);
    s_ring_rd = s_ring_wr = 0;
    s_eof = false;
    s_input_end = false;
    return true;
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
    if (s_mad_synth)  { umm_free(s_mad_synth); s_mad_synth = NULL; }
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
    if (!s_mad_synth) {
        s_mad_synth = umm_malloc(sizeof(struct mad_synth));
        if (!s_mad_synth) { printf("[MP3] FAILED to alloc mad_synth\n"); return false; }
    }

    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);

    printf("[MP3] libmad structs: stream=%u frame=%u synth=%u bytes\n",
           (unsigned)sizeof(struct mad_stream),
           (unsigned)sizeof(struct mad_frame),
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

bool mp3_player_start_fed(uint32_t sample_rate, uint16_t channels) {
    if (!s_initialized) {
        if (!mp3_player_init()) return false;
    }
    fade_out_and_wait();

    mutex_enter_blocking(&s_mp3_mutex);
    detach();
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
    mutex_exit(&s_mp3_mutex);
    audio_output_ensure_running();
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
    atomic_store(&s_mark_state, MARK_NONE);
    atomic_store(&s_fed_end, false);
    if (s_fed_ring_buf) { umm_free(s_fed_ring_buf); s_fed_ring_buf = NULL; }
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;
    s_eof = false;
    s_input_end = false;
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
