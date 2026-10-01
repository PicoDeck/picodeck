#include "fileplayer.h"
#include "audio.h"
#include "sdcard.h"
#include "pico/mutex.h"
#include "umm_malloc.h"
#include "wav.h"

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WAV_BUFFER_SIZE FILEPLAYER_BUFFER_SIZE
// Most bytes one update reads (~23 ms of 44.1 kHz stereo).
#define FILEPLAYER_READ_MAX 4096u
// Below this a read is not worth an SD transaction (unless it finishes the
// data chunk): wait for the ring to drain further.
#define FILEPLAYER_READ_MIN 512u
// The most QOA frames one Core 1 tick decodes (~0.2 ms of stereo from RAM
// at 200 MHz); a quarter of the stream ring.
#define QOA_TICK_FRAMES 1024u

/* Locking. Core 1 streams the active player in fileplayer_update(); Core 0
 * loads, plays, seeks, stops and frees players. s_lock guards everything
 * update reads: the players' files and data-chunk fields, their state and
 * position, and s_active_player. Core 0 takes it blocking (held for field
 * updates only: files are opened and closed outside it); Core 1 only ever
 * try-locks it and skips the tick when Core 0 holds it, so Core 1 never
 * waits on Core 0. Core 1's SD reads are try-reads too (sdcard_try_fread_at),
 * so Core 1 cannot deadlock with a Core 0 that holds the SD card and then
 * calls in here. Finish/loop callbacks run after the lock is released. */
static mutex_t s_lock;
static atomic_bool s_initialized;

static fileplayer_t s_players[FILEPLAYER_MAX_INSTANCES];
static fileplayer_t *s_active_player = NULL;
static uint8_t *s_wav_buffer = NULL;

// Parse the header window at the start of f (RIFF chunk walk in wav.c).
// The window buffer comes from PSRAM, not the 4 KB main stack.
static bool parse_wav_header(sdfile_t f, int file_size, wav_info_t *info,
                             const char **why) {
    uint8_t *hdr = (uint8_t *)umm_malloc(WAV_HEADER_WINDOW);
    if (!hdr) {
        *why = "out of memory";
        return false;
    }
    int n = sdcard_fread(f, hdr, WAV_HEADER_WINDOW);
    wav_err_t err = n > 0 ? wav_parse(hdr, (size_t)n, info) : WAV_ERR_NOT_WAV;
    umm_free(hdr);
    if (err != WAV_OK) {
        printf("fileplayer: %s\n", wav_strerror(err));
        *why = wav_strerror(err);
        return false;
    }
    // The streaming path converts 16-bit PCM only.
    if (info->bits_per_sample != 16) {
        printf("fileplayer: %u-bit WAV not supported (16-bit only)\n",
               info->bits_per_sample);
        *why = "only 16-bit WAV can be streamed";
        return false;
    }
    // Clamp a data chunk that claims more than the file holds.
    uint32_t avail = file_size > (int)info->data_offset
                         ? (uint32_t)file_size - info->data_offset : 0;
    if (info->data_size > avail)
        info->data_size = avail - avail % info->block_align;
    return true;
}

static fileplayer_type_t detect_file_type(sdfile_t f) {
    uint8_t header[16];
    memset(header, 0, sizeof(header));

    if (sdcard_fread(f, header, sizeof(header)) < (int)sizeof(header)) {
        return FILEPLAYER_TYPE_UNKNOWN;
    }

    sdcard_fseek(f, 0);

    if (memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0) {
        return FILEPLAYER_TYPE_WAV;
    }

    if (memcmp(header, "qoaf", 4) == 0) {
        return FILEPLAYER_TYPE_QOA;
    }

    if (memcmp(header, "ID3", 3) == 0) {
        return FILEPLAYER_TYPE_MP3;
    }

    if ((header[0] == 0xFF && (header[1] & 0xE0) == 0xE0) ||
        (header[0] == 0xFE) || (header[0] == 0xFA) ||
        (header[0] == 0xFB) || (header[0] == 0xFC)) {
        return FILEPLAYER_TYPE_MP3;
    }

    return FILEPLAYER_TYPE_UNKNOWN;
}

// Parse the QOA header window at the start of f.
static bool parse_qoa_header(sdfile_t f, qoa_info_t *info, const char **why) {
    uint8_t hdr[QOA_HEADER_WINDOW];
    int n = sdcard_fread(f, hdr, sizeof(hdr));
    qoa_err_t err = n > 0 ? qoa_parse(hdr, (size_t)n, info) : QOA_ERR_NOT_QOA;
    if (err != QOA_OK) {
        printf("fileplayer: %s\n", qoa_strerror(err));
        *why = qoa_strerror(err);
        return false;
    }
    return true;
}

// Point a QOA player's compressed cursor at its (virtual PCM) position: the
// frame holding it is read next, and its frames before the position are
// decoded and dropped.  Call with s_lock held.
static void qoa_reposition_locked(fileplayer_t *player) {
    uint32_t index = player->position / player->block_align;
    player->qoa_file_pos = qoa_frame_offset(&player->qoa, index);
    player->qoa_skip = index % player->qoa.frame_samples;
    player->qoa_loaded = false;
}

static bool any_playing_locked(void) {
    for (int i = 0; i < FILEPLAYER_MAX_INSTANCES; i++)
        if (s_players[i].in_use && s_players[i].state == FILEPLAYER_STATE_PLAYING)
            return true;
    return false;
}

// Stops player; returns true when it had the stream and nothing else plays
// (the caller stops the stream once the lock is released).
static bool stop_locked(fileplayer_t *player) {
    bool had_stream = player->state == FILEPLAYER_STATE_PLAYING ||
                      s_active_player == player;
    player->state = FILEPLAYER_STATE_STOPPED;
    player->position = 0;
    if (s_active_player == player)
        s_active_player = NULL;
    return had_stream && !any_playing_locked();
}

void fileplayer_reset(void) {
    if (!atomic_load(&s_initialized)) return;
    sdfile_t files[FILEPLAYER_MAX_INSTANCES];
    mutex_enter_blocking(&s_lock);
    bool stream = s_active_player &&
                  s_active_player->state == FILEPLAYER_STATE_PLAYING;
    for (int i = 0; i < FILEPLAYER_MAX_INSTANCES; i++)
        files[i] = s_players[i].file;
    memset(s_players, 0, sizeof(s_players));
    s_active_player = NULL;
    mutex_exit(&s_lock);
    if (stream)
        audio_stop_stream();
    for (int i = 0; i < FILEPLAYER_MAX_INSTANCES; i++)
        if (files[i])
            sdcard_fclose(files[i]);
}

void fileplayer_init(void) {
    if (atomic_load(&s_initialized)) return;

    printf("[FILEPLAYER] Allocating WAV buffer (%d bytes)...\n", WAV_BUFFER_SIZE);
    s_wav_buffer = umm_malloc(WAV_BUFFER_SIZE);
    printf("[FILEPLAYER] WAV buffer allocated: %s\n", s_wav_buffer ? "OK" : "FAILED");
    if (!s_wav_buffer) return;

    memset(s_players, 0, sizeof(s_players));
    mutex_init(&s_lock);
    // Published last: Core 1's update checks it before touching the lock.
    atomic_store(&s_initialized, true);
}

fileplayer_t *fileplayer_create(void) {
    fileplayer_init();  // native apps reach here without the Lua bridge
    if (!atomic_load(&s_initialized)) return NULL;
    fileplayer_t *found = NULL;
    mutex_enter_blocking(&s_lock);
    for (int i = 0; i < FILEPLAYER_MAX_INSTANCES; i++) {
        if (!s_players[i].in_use) {
            found = &s_players[i];
            memset(found, 0, sizeof(*found));
            found->in_use = true;
            found->volume = 100;
            found->volume_r = 100;
            found->channels = 2;
            found->rate = 1.0f;
            found->block_align = 4;
            found->sample_rate = 44100;
            break;
        }
    }
    mutex_exit(&s_lock);
    return found;
}

void fileplayer_destroy(fileplayer_t *player) {
    if (!player || !atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_lock);
    bool stop_stream = stop_locked(player);
    sdfile_t file = player->file;
    memset(player, 0, sizeof(fileplayer_t));  // in_use = false: slot free
    mutex_exit(&s_lock);
    if (stop_stream)
        audio_stop_stream();
    if (file)
        sdcard_fclose(file);
}

bool fileplayer_load(fileplayer_t *player, const char *path) {
    return fileplayer_load_err(player, path, NULL);
}

bool fileplayer_load_err(fileplayer_t *player, const char *path,
                         const char **reason) {
    const char *why = "cannot play file";
    if (reason) *reason = why;
    if (!player || !path || !atomic_load(&s_initialized)) {
        if (reason) *reason = "fileplayer unavailable";
        return false;
    }

    // Open and parse outside the lock (SD work can take milliseconds; Core
    // 1 keeps streaming another player meanwhile).
    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) {
        printf("fileplayer: failed to open %s\n", path);
        if (reason) *reason = "cannot open file";
        return false;
    }

    fileplayer_type_t type = detect_file_type(f);
    wav_info_t info;
    qoa_info_t qinfo;
    bool ok = false;
    if (type == FILEPLAYER_TYPE_MP3) {
        printf("fileplayer: MP3 file detected, use sound.mp3player() instead\n");
        why = "MP3 is not supported here (use mp3player)";
    }
    else if (type == FILEPLAYER_TYPE_QOA) {
        // A QOA player reports virtual 16-bit PCM: data_size/position are
        // counted as if the decoded PCM were the file, so the position,
        // offset and flow-control code is shared with WAV.
        ok = parse_qoa_header(f, &qinfo, &why);
        if (ok) {
            // A copy cut short plays its whole frames (and loops, like a
            // short WAV); the cut frame is not playable.
            int size = sdcard_fsize_handle(f);
            qinfo.samples = qoa_samples_in(&qinfo, size > 0 ? (uint32_t)size : 0);
            if (qinfo.samples == 0) {
                printf("fileplayer: %s\n", qoa_strerror(QOA_ERR_TRUNCATED));
                why = qoa_strerror(QOA_ERR_TRUNCATED);
                ok = false;
            }
        }
        if (ok) {
            info.sample_rate = qinfo.sample_rate;
            info.channels = qinfo.channels;
            info.block_align = (uint16_t)(2 * qinfo.channels);
            info.data_offset = qinfo.first_frame_offset;
            info.data_size = qinfo.samples * info.block_align;
        }
    } else if (type != FILEPLAYER_TYPE_WAV) {
        printf("fileplayer: unknown file format\n");
        why = "unknown file format (not WAV or QOA)";
    } else if (!parse_wav_header(f, sdcard_fsize_handle(f), &info, &why))
        printf("fileplayer: failed to parse WAV\n");
    else
        ok = true;

    // Swap the new file in (or none, on failure: a failed load leaves the
    // player empty, as before) and stop this player.
    mutex_enter_blocking(&s_lock);
    bool stop_stream = stop_locked(player);
    sdfile_t old = player->file;
    player->file = ok ? f : NULL;
    player->type = type;
    strncpy(player->path, path, sizeof(player->path) - 1);
    player->path[sizeof(player->path) - 1] = '\0';
    if (ok) {
        player->sample_rate = info.sample_rate;
        player->data_offset = info.data_offset;
        player->data_size = info.data_size;
        player->block_align = info.block_align;
        player->channels = (uint8_t)info.channels;
        player->length = info.data_size / info.block_align;
        if (type == FILEPLAYER_TYPE_QOA)
            player->qoa = qinfo;
    } else {
        player->data_size = 0;
        player->length = 0;
    }
    // A new file does not inherit the old file's loop range.
    player->loop = false;
    player->loop_start = 0;
    player->loop_end = 0;
    player->position = 0;
    if (ok && type == FILEPLAYER_TYPE_QOA)
        qoa_reposition_locked(player);
    mutex_exit(&s_lock);

    if (stop_stream)
        audio_stop_stream();
    if (old)
        sdcard_fclose(old);
    if (!ok) {
        sdcard_fclose(f);
        if (reason) *reason = why;
        return false;
    }
    if (reason) *reason = NULL;

    printf("fileplayer: loaded %s (%lu Hz, %s, %u ch, %lu samples)\n",
           path, (unsigned long)info.sample_rate,
           type == FILEPLAYER_TYPE_QOA ? "QOA" : "16-bit",
           info.channels, (unsigned long)player->length);
    return true;
}

bool fileplayer_play(fileplayer_t *player, uint8_t repeat_count) {
    if (!player || !atomic_load(&s_initialized)) return false;

    mutex_enter_blocking(&s_lock);
    if (!player->file) {
        mutex_exit(&s_lock);
        return false;
    }
    // One stream: starting this player stops whichever other one was on it.
    if (s_active_player && s_active_player != player)
        s_active_player->state = FILEPLAYER_STATE_STOPPED;
    player->state = FILEPLAYER_STATE_PLAYING;
    player->position = 0;  // update() reads at data_offset + position
    if (player->type == FILEPLAYER_TYPE_QOA)
        qoa_reposition_locked(player);
    player->repeats = repeat_count;  // 0 plays until stopped
    player->plays = 0;
    player->pass_pushed = false;
    player->under_armed = false;
    player->underran = false;
    s_active_player = player;
    uint32_t rate = player->sample_rate;
    // Start the stream (clears the ring) before Core 1 can push into it,
    // held: update() releases it once the ring is full (#34). Started on
    // an empty ring, it ran dry whenever Core 0 took the SD card right
    // after play() (loading a sample, say), before the first read.
    audio_start_stream_held(rate);
    mutex_exit(&s_lock);
    return true;
}

void fileplayer_stop(fileplayer_t *player) {
    if (!player || !atomic_load(&s_initialized)) return;
    // The file stays loaded: play() starts it again from the beginning.
    mutex_enter_blocking(&s_lock);
    bool stop_stream = stop_locked(player);
    mutex_exit(&s_lock);
    if (stop_stream)
        audio_stop_stream();
}

// Pause holds the stream: the sound stops at once and the ring keeps what
// it has (it used to play out, then count as underruns until resume).
void fileplayer_pause(fileplayer_t *player) {
    if (!player || !atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_lock);
    if (player->state == FILEPLAYER_STATE_PLAYING) {
        player->state = FILEPLAYER_STATE_PAUSED;
        if (s_active_player == player)
            audio_stream_hold();
    }
    mutex_exit(&s_lock);
}

// Resume plays on from the paused frame: the next update() tops the ring
// up and releases the stream, as after play().
void fileplayer_resume(fileplayer_t *player) {
    if (!player || !atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_lock);
    if (player->state == FILEPLAYER_STATE_PAUSED && s_active_player == player) {
        player->state = FILEPLAYER_STATE_PLAYING;
        player->under_armed = false;  // re-armed by the next push
    }
    mutex_exit(&s_lock);
}

bool fileplayer_is_playing(const fileplayer_t *player) {
    return player && player->state == FILEPLAYER_STATE_PLAYING;
}

// Frames of the data chunk consumed so far.
uint32_t fileplayer_get_position(const fileplayer_t *player) {
    if (!player || player->block_align == 0) return 0;
    return player->position / player->block_align;
}

uint32_t fileplayer_get_length(const fileplayer_t *player) {
    if (!player) return 0;
    return player->length;
}

uint32_t fileplayer_get_sample_rate(const fileplayer_t *player) {
    if (!player || !player->file) return 0;
    return player->sample_rate;
}

void fileplayer_set_volume(fileplayer_t *player, uint8_t left, uint8_t right) {
    if (!player) return;
    // 0-100 like every other volume: the mixer scales by vol/100, so more
    // would overdrive (and clip) the stream.
    if (left > 100) left = 100;
    if (right > 100) right = 100;
    player->volume = left;
    player->volume_r = right > 0 ? right : left;
}

void fileplayer_get_volume(const fileplayer_t *player, uint8_t *left, uint8_t *right) {
    if (!player) return;
    if (left) *left = player->volume;
    if (right) *right = player->volume_r;
}

void fileplayer_set_loop_range(fileplayer_t *player, uint32_t start, uint32_t end) {
    if (!player || !atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_lock);
    player->loop = true;
    player->loop_start = start;
    player->loop_end = end;
    mutex_exit(&s_lock);
}

void fileplayer_set_finish_callback(fileplayer_t *player, int (*cb)(void *), void *arg) {
    if (!player || !atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_lock);
    player->finish_callback = cb;
    player->finish_callback_arg = arg;
    mutex_exit(&s_lock);
}

void fileplayer_set_loop_callback(fileplayer_t *player, int (*cb)(void *), void *arg) {
    if (!player || !atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_lock);
    player->loop_callback = cb;
    player->loop_callback_arg = arg;
    mutex_exit(&s_lock);
}

void fileplayer_set_offset(fileplayer_t *player, uint32_t seconds) {
    if (!player || !atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_lock);
    if (player->file) {
        // Whole frames from the start of the data chunk.
        uint64_t offset = (uint64_t)seconds * player->sample_rate * player->block_align;
        if (offset > player->data_size)
            offset = player->data_size;
        player->position = (uint32_t)offset;
        if (player->type == FILEPLAYER_TYPE_QOA)
            qoa_reposition_locked(player);
        // Paused (or still filling after play()), the stream is held and
        // its ring holds audio from before the seek: drop it, so resume
        // (or the start) plays the new position. Playing, the ring plays
        // out first, as it always has.
        if (s_active_player == player)
            audio_stream_flush_held();
    }
    mutex_exit(&s_lock);
}

uint32_t fileplayer_get_offset(const fileplayer_t *player) {
    if (!player || player->block_align == 0 || player->sample_rate == 0) return 0;
    return player->position / player->block_align / player->sample_rate;
}

void fileplayer_set_stop_on_underrun(fileplayer_t *player, bool flag) {
    if (!player || !atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_lock);
    player->stop_on_underrun = flag;
    mutex_exit(&s_lock);
}

void fileplayer_set_rate(fileplayer_t *player, float rate) {
    if (!player) return;
    if (!(rate >= 0.1f)) rate = 0.1f;  // NaN too: it compares false
    if (rate > 4.0f) rate = 4.0f;
    player->rate = rate;
}

float fileplayer_get_rate(const fileplayer_t *player) {
    return player ? player->rate : 1.0f;
}

/* Flow control. The stream ring holds frames at the content rate (audio.c's
 * refill ISR resamples them to AUDIO_OUT_RATE); a player at speed `rate`
 * turns `rate` input frames into one ring frame (nearest neighbour), so
 * with F ring frames free it may read floor(F * rate) input frames, i.e.
 * floor(F * rate) * block_align bytes. Reading more (as the old code did,
 * 4 KB every tick at SD speed) only made audio_push_samples drop what did
 * not fit while position raced to EOF: a long WAV "finished" in seconds. */
static uint32_t bytes_that_fit(const fileplayer_t *p, uint32_t limit) {
    float rate = p->rate < 0.1f ? 0.1f : p->rate;
    uint32_t in_frames = (uint32_t)((float)audio_ring_free() * rate);
    uint32_t remaining = p->position < limit ? limit - p->position : 0;
    uint32_t n = FILEPLAYER_READ_MAX;
    if (n / p->block_align > in_frames) n = in_frames * p->block_align;
    if (n > remaining) n = remaining;
    return n - n % p->block_align;  // whole frames: an odd step swaps L/R
}

// Convert br bytes of 16-bit PCM at pcm to stereo and push them.
static void push_pcm(const fileplayer_t *p, const int16_t *pcm, uint32_t br) {
    uint32_t in_frames = br / p->block_align;
    float rate = p->rate < 0.1f ? 0.1f : p->rate;
    uint32_t out_frames = (uint32_t)((float)in_frames / rate);
    if (out_frames == 0) out_frames = 1;
    bool mono = p->channels == 1;
    int32_t vol_l = p->volume, vol_r = p->volume_r;
    int16_t stereo_buf[512];  // 256 stereo frames at a time
    uint32_t pos = 0;
    while (pos < out_frames) {
        uint32_t chunk = out_frames - pos;
        if (chunk > 256) chunk = 256;
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t src = (uint32_t)((float)(pos + i) * rate);
            if (src >= in_frames) src = in_frames - 1;
            int32_t l = mono ? pcm[src] : pcm[src * 2];
            int32_t r = mono ? pcm[src] : pcm[src * 2 + 1];
            // 0-100 volumes never grow a sample: no clipping needed.
            stereo_buf[i * 2] = (int16_t)((l * vol_l) / 100);
            stereo_buf[i * 2 + 1] = (int16_t)((r * (mono ? vol_l : vol_r)) / 100);
        }
        audio_push_samples(stereo_buf, (int)chunk);
        pos += chunk;
    }
}

typedef struct {
    int (*fn)(void *);
    void *arg;
} fp_callback_t;

/* Loop range. setLoopRange(start, end) is in seconds; the pass runs to
 * `end`, then continues from `start` (the first pass starts wherever the
 * player is, normally 0). Both become frame-aligned byte positions in the
 * (virtual, for QOA) PCM the position counts, clamped to the data; an end of
 * 0 is the end of the data, and an empty or reversed range loops the whole
 * file. Without a range a pass runs to the end of the data and wraps to 0.
 * QOA wraps through qoa_reposition_locked(), the seek's machinery: sample
 * exact, but the decoder drops the frames before `start` inside its QOA
 * frame (up to 5119) at every wrap, so a start on a frame boundary
 * (multiples of 5120 samples) wraps cheapest. */
static void loop_bounds(const fileplayer_t *p, uint32_t *start, uint32_t *end) {
    uint32_t size = p->data_size - p->data_size % p->block_align;
    *start = 0;
    *end = size;
    if (!p->loop)
        return;
    uint64_t unit = (uint64_t)p->sample_rate * p->block_align;
    uint64_t s = (uint64_t)p->loop_start * unit;
    uint64_t e = p->loop_end ? (uint64_t)p->loop_end * unit : size;
    if (e > size) e = size;
    if (s < e) {
        *start = (uint32_t)s;
        *end = (uint32_t)e;
    }
}

// Where this pass stops: the loop end, or the end of the data for a player
// that is already past it (a seek beyond the range plays out the file).
static uint32_t pass_limit(const fileplayer_t *p) {
    uint32_t start, end;
    loop_bounds(p, &start, &end);
    return p->position <= end ? end : p->data_size;
}

// Where the next pass starts.
static uint32_t wrap_position(const fileplayer_t *p) {
    uint32_t start, end;
    loop_bounds(p, &start, &end);
    return start;
}

/* Underruns. The stream ring's count (audio_mix.c) is global; a player
 * compares it tick to tick. The ring is legitimately empty from play() or
 * resume() until the first push, so the check is armed by a push
 * (arm_underrun_locked, which re-reads the base after the push's read). Returns
 * true when the stream starved since the previous tick (and latches
 * p->underran). A count that went down was reset (audiostat reset). */
static bool underrun_check_locked(fileplayer_t *p) {
    uint32_t u = 0;
    audio_stream_debug(NULL, &u, NULL);
    bool hit = p->under_armed && u > p->under_base;
    p->under_base = u;
    if (hit)
        p->underran = true;
    return hit;
}

// Arms the check after a push, with the count as of now: the refill ISR
// runs during the SD read that preceded the push (several renders on the
// device), and an empty ring's pops in that time are not an underrun.
static void arm_underrun_locked(fileplayer_t *p) {
    if (p->under_armed)
        return;
    audio_stream_debug(NULL, &p->under_base, NULL);
    p->under_armed = true;
}

// The active player p finished (s_lock held): the stream plays out what
// the ring holds, and its running dry then is the end, not an underrun.
static fp_callback_t finish_locked(fileplayer_t *p) {
    p->state = FILEPLAYER_STATE_STOPPED;
    s_active_player = NULL;
    audio_stream_drain();
    return (fp_callback_t){p->finish_callback, p->finish_callback_arg};
}

// p's pass ended and the next starts at the loop start (s_lock held). The
// stream notes the loop point for `audiostat`.
static fp_callback_t wrap_locked(fileplayer_t *p) {
    p->position = wrap_position(p);
    if (p->type == FILEPLAYER_TYPE_QOA)
        qoa_reposition_locked(p);
    p->pass_pushed = false;
    audio_stream_mark_loop();
    return (fp_callback_t){p->loop_callback, p->loop_callback_arg};
}

// Whether another pass follows the one that just ended (counts the pass).
static bool plays_again(fileplayer_t *p) {
    return p->loop || p->repeats == 0 || ++p->plays < p->repeats;
}

// One streaming step for the active player p (s_lock held).
static fp_callback_t update_locked(fileplayer_t *p) {
    fp_callback_t cb = {NULL, NULL};

    // Stop on underrun if configured
    if (underrun_check_locked(p) && p->stop_on_underrun) {
        if (stop_locked(p))
            audio_stop_stream();
        return cb;
    }

    uint32_t limit = pass_limit(p);
    uint32_t remaining = p->position < limit ? limit - p->position : 0;
    uint32_t to_read = bytes_that_fit(p, limit);
    if (remaining >= p->block_align &&
        (to_read == 0 || (to_read < FILEPLAYER_READ_MIN && to_read < remaining))) {
        // The ring is (nearly) full: wait for the DMA to drain it. The
        // stream plays from here (after play() or resume(), it starts now).
        audio_stream_release();
        return cb;
    }

    // Read at our own offset (to_read == 0 at the end of the data chunk
    // takes the end-of-data path below). Skip the tick if Core 0 owns the
    // SD card.
    int n = to_read > 0
                ? sdcard_try_fread_at(p->file, p->data_offset + p->position,
                                      s_wav_buffer, (int)to_read)
                : 0;
    if (n == SDCARD_BUSY)
        return cb;
    uint32_t br = n > 0 ? (uint32_t)n : 0;
    br -= br % p->block_align;  // a short read at EOF: whole frames only

    if (br > 0) {
        push_pcm(p, (const int16_t *)s_wav_buffer, br);
        p->position += br;
        p->pass_pushed = true;
        arm_underrun_locked(p);
        if (p->position < limit)
            return cb;
        // That read ended the pass: loop (or finish) on this tick, so the
        // next tick reads the next pass's first block.
    } else if (n < 0 || !p->pass_pushed) {
        // A read error, or a pass that found no frames at all (an empty
        // data chunk): finish instead of rewinding every tick.
        return finish_locked(p);
    }
    // End of the pass: go round again, or finish (the stream keeps playing
    // out what the ring still holds).
    return plays_again(p) ? wrap_locked(p) : finish_locked(p);
}

// One streaming step for the active QOA player p (s_lock held).  Each tick
// decodes only what the stream ring can take, a chunk at a time into the
// stack (qoa_dec_run, from RAM), so no tick decodes a whole frame and no
// PCM scratch exists; a frame's bytes are read into s_wav_buffer when the
// last one is done, at most one read per tick.  position stays virtual PCM.
static fp_callback_t qoa_update_locked(fileplayer_t *p) {
    fp_callback_t cb = {NULL, NULL};

    if (underrun_check_locked(p) && p->stop_on_underrun) {
        if (stop_locked(p))
            audio_stop_stream();
        return cb;
    }

    // One chunk: the WAV path's smallest read (256 mono / 128 stereo frames).
    int16_t pcm[FILEPLAYER_READ_MIN / 2];
    const uint32_t chunk = sizeof(pcm) / p->block_align;
    bool read = false, eof = false;
    uint32_t decoded = 0;  // this tick, dropped (seek) frames included
    uint32_t limit = pass_limit(p);  // the loop end, or the end of the data
    for (;;) {
        uint32_t remaining = p->position < limit ? limit - p->position : 0;
        uint32_t budget = bytes_that_fit(p, limit);  // virtual PCM bytes
        if (remaining == 0)
            break;  // the end of the pass
        if (budget < FILEPLAYER_READ_MIN && budget < remaining) {
            // The ring is (nearly) full: next tick. The stream plays from
            // here (after play() or resume(), it starts now).
            audio_stream_release();
            break;
        }
        if (!p->qoa_loaded) {
            if (read)
                break;
            read = true;
            // A full frame is qoa.frame_size and only the tail frame is
            // shorter, so one read holds a whole frame: at the end of the
            // file it comes back short.  Core 1 never asks the card for the
            // file size (a blocking call on Core 0's SD mutex).
            int n = sdcard_try_fread_at(p->file, p->qoa_file_pos, s_wav_buffer,
                                        (int)p->qoa.frame_size);
            if (n == SDCARD_BUSY)
                return cb;  // Core 0 owns the card: next tick
            if (n == 0) {
                eof = true;  // the file ends before its header's length
                break;
            }
            if (n < 0 || qoa_dec_begin(&p->qoa_dec, &p->qoa, s_wav_buffer,
                                       (size_t)n) == 0) {
                // Read error or a corrupt frame mid-file: finish (never
                // loop into the same bad frame), as a WAV read error does.
                return finish_locked(p);
            }
            // The next frame starts where this one's size field says.
            p->qoa_file_pos += qoa_frame_bytes(s_wav_buffer, (size_t)n);
            p->qoa_loaded = true;
        }
        // At least a slice (20 frames), even when the file's last few
        // frames are all the ring may take: what lies past them is dropped.
        uint32_t max = budget / p->block_align;
        if (max < 20)
            max = 20;
        if (max > chunk)
            max = chunk;
        // Bounded work per tick: the tick after play() (an empty ring) or a
        // seek (up to a frame to drop) spreads over a few ticks, not one.
        if (decoded + 20 > QOA_TICK_FRAMES)
            break;
        if (max > QOA_TICK_FRAMES - decoded)
            max = QOA_TICK_FRAMES - decoded;
        uint32_t k = qoa_dec_run(&p->qoa_dec, s_wav_buffer, pcm, max);
        decoded += k;
        if (k == 0) {
            if (p->qoa_dec.pos >= p->qoa_dec.samples) {
                p->qoa_loaded = false;  // frame done: the next one
                continue;
            }
            break;  // no whole slice fits: next tick
        }
        uint32_t from = p->qoa_skip < k ? p->qoa_skip : k;
        p->qoa_skip -= from;
        uint32_t frames = k - from;
        // Never past the virtual data size (a file whose frames hold more
        // than its header claims): the surplus is undeliverable.
        if (frames > remaining / p->block_align)
            frames = remaining / p->block_align;
        if (frames > 0) {
            push_pcm(p, pcm + from * p->channels, frames * p->block_align);
            p->position += frames * p->block_align;
            p->pass_pushed = true;
            arm_underrun_locked(p);
        }
    }

    // End of data (every frame delivered, or the file ended early): loop or
    // finish, as WAV does.
    if (p->position >= limit || eof)
        return p->pass_pushed && plays_again(p) ? wrap_locked(p)
                                                : finish_locked(p);
    return cb;
}

// Called from Core 1 every tick: tops the stream ring up from the active
// player's file.
void fileplayer_update(void) {
    if (!atomic_load(&s_initialized)) return;
    if (!mutex_try_enter(&s_lock, NULL))
        return;  // Core 0 is changing a player: next tick
    fp_callback_t cb = {NULL, NULL};
    fileplayer_t *p = s_active_player;
    if (p && p->file && p->state == FILEPLAYER_STATE_PLAYING)
        cb = p->type == FILEPLAYER_TYPE_QOA ? qoa_update_locked(p)
                                            : update_locked(p);
    mutex_exit(&s_lock);
    if (cb.fn)
        cb.fn(cb.arg);
}

// Clears on read, as documented ("since the last check"). Core 0 takes the
// lock blocking, like every other field update.
bool fileplayer_did_underrun(fileplayer_t *player) {
    if (!player || !atomic_load(&s_initialized)) return false;
    mutex_enter_blocking(&s_lock);
    bool hit = player->underran;
    player->underran = false;
    mutex_exit(&s_lock);
    return hit;
}
