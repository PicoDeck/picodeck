// Simulator audio implementation
// Replaces hardware audio/sound/fileplayer/mp3 drivers with SDL2-based output.
// All functions match the exact signatures from the real driver headers.

#include "hal/hal_audio.h"
#include "drivers/audio.h"
#include "drivers/sound.h"
#include "drivers/fileplayer.h"
#include "drivers/mp3_player.h"
#include "drivers/sdcard.h"
#include "drivers/wav.h"
#include "drivers/audio_ring.h"

#ifndef FPM_64BIT
#define FPM_64BIT
#endif
#include "mad.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <SDL2/SDL.h>

// ── Audio output config ─────────────────────────────────────────────────────
#define SIM_SAMPLE_RATE 44100
#define SIM_CHANNELS    2

// ── Tone generation state ───────────────────────────────────────────────────
volatile bool s_tone_playing = false;
static volatile uint32_t s_tone_freq = 0;
static volatile uint32_t s_tone_end_ms = 0;  // 0 = infinite
static volatile uint32_t s_tone_phase = 0;
static uint8_t s_master_volume = 100;

// ── PCM streaming state ─────────────────────────────────────────────────────
volatile bool s_stream_active = false;
static uint32_t s_stream_sample_rate = 44100;

// ── Sound sample/player state ───────────────────────────────────────────────
static sound_context_t s_sound_ctx;
static uint32_t s_sound_time_us = 0;
pthread_mutex_t s_sound_mutex = PTHREAD_MUTEX_INITIALIZER;


// ── MP3 player state ────────────────────────────────────────────────────────
#define MP3_DECODE_BUF_SIZE (8192 + MAD_BUFFER_GUARD)
#define MP3_PCM_RING_SIZE   32768

static struct mad_stream *s_mad_stream = NULL;
static struct mad_frame  *s_mad_frame  = NULL;
static struct mad_synth  *s_mad_synth  = NULL;
static sdfile_t s_mp3_file = NULL;
static uint8_t *s_mp3_decode_buf = NULL;
static int s_mp3_bytes_in_buf = 0;
static int s_mp3_buf_pos = 0;
static mp3_player_t s_mp3_player;
static bool s_mp3_initialized = false;
static pthread_mutex_t s_mp3_mutex = PTHREAD_MUTEX_INITIALIZER;

// ── Fed mode: compressed MP3 ring written by video player ──────────────────
#define FED_RING_SIZE (64 * 1024)
static bool     s_fed_mode = false;
static uint8_t *s_fed_ring = NULL;       // malloc'd ring buffer (no PIO PSRAM in sim)
static volatile uint32_t s_fed_wr = 0;
static volatile uint32_t s_fed_rd = 0;

static inline uint32_t fed_ring_available(void) {
    uint32_t wr = s_fed_wr, rd = s_fed_rd;
    return (wr >= rd) ? (wr - rd) : (FED_RING_SIZE - rd + wr);
}

static inline uint32_t fed_ring_free(void) {
    return FED_RING_SIZE - 1 - fed_ring_available();
}

static void fed_ring_read(uint8_t *dst, uint32_t len) {
    uint32_t rd = s_fed_rd;
    uint32_t to_end = FED_RING_SIZE - rd;
    if (len <= to_end) {
        memcpy(dst, s_fed_ring + rd, len);
    } else {
        memcpy(dst, s_fed_ring + rd, to_end);
        memcpy(dst + to_end, s_fed_ring, len - to_end);
    }
    s_fed_rd = (rd + len) % FED_RING_SIZE;
}

// MP3 PCM ring buffer (simple, no DMA needed)
static int16_t *s_mp3_pcm_ring = NULL;
static volatile size_t s_mp3_ring_rd = 0;
static volatile size_t s_mp3_ring_wr = 0;
#define MP3_RING_FRAMES (MP3_PCM_RING_SIZE / 4)  // stereo int16_t pairs

static inline size_t mp3_ring_available(void) {
    size_t wr = s_mp3_ring_wr, rd = s_mp3_ring_rd;
    return (wr >= rd) ? (wr - rd) : (MP3_RING_FRAMES - rd + wr);
}

static inline size_t mp3_ring_free(void) {
    return MP3_RING_FRAMES - 1 - mp3_ring_available();
}

static void mp3_ring_write_stereo(const int16_t *data, size_t frames) {
    while (frames > 0) {
        size_t wr = s_mp3_ring_wr;
        size_t free_space = mp3_ring_free();
        if (free_space == 0) break;
        size_t chunk = (frames < free_space) ? frames : free_space;
        size_t to_end = MP3_RING_FRAMES - wr;
        if (chunk <= to_end) {
            memcpy(s_mp3_pcm_ring + wr * 2, data, chunk * 4);
        } else {
            memcpy(s_mp3_pcm_ring + wr * 2, data, to_end * 4);
            memcpy(s_mp3_pcm_ring, data + to_end * 2, (chunk - to_end) * 4);
        }
        s_mp3_ring_wr = (wr + chunk) % MP3_RING_FRAMES;
        data += chunk * 2;
        frames -= chunk;
    }
}

// ── Helper: get current time in ms ──────────────────────────────────────────
static uint32_t sim_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ══════════════════════════════════════════════════════════════════════════════
// Audio API (tone generation + PCM streaming)
// ══════════════════════════════════════════════════════════════════════════════

void audio_init(void) {
    s_master_volume = 100;
    s_tone_playing = false;
    s_stream_active = false;
}

void audio_core1_init(void) {
    // No-op in simulator (no alarm pool needed)
}

alarm_pool_t *audio_get_core1_alarm_pool(void) {
    return NULL;
}

void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz < 20) freq_hz = 20;
    if (freq_hz > 20000) freq_hz = 20000;

    s_tone_freq = freq_hz;
    s_tone_phase = 0;
    s_tone_end_ms = (duration_ms > 0) ? (sim_time_ms() + duration_ms) : 0;
    s_tone_playing = true;
}

void audio_stop_tone(void) {
    s_tone_playing = false;
    s_tone_end_ms = 0;
}

void audio_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    s_master_volume = volume;
}

/* The PCM stream: the firmware's ring (src/drivers/audio_ring.h) between
 * the producers (fileplayer.c and mod_player.c on the Core 1 thread,
 * pushSamples from Core 0) and a consumer that stands in for audio.c's DMA
 * refill ISR: sim_stream_drain() pops frames at AUDIO_OUT_RATE by wall
 * clock, so audio_ring_free() means what it means on hardware and a
 * producer without flow control loses audio here too. (It used to return a
 * constant 4096, which made the simulator blind to flow-control bugs.) */
static audio_ring_t s_ring;
static uint64_t s_drain_last_us = 0;
static uint64_t s_drain_rem = 0;  // sub-frame remainder, in frame-microseconds

static uint64_t sim_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void audio_start_stream(uint32_t sample_rate) {
    audio_stop_tone();
    s_stream_active = false;
    s_stream_sample_rate = sample_rate ? sample_rate : AUDIO_OUT_RATE;
    audio_ring_clear(&s_ring);
    s_ring.phase = 0;
    s_drain_last_us = sim_time_us();
    s_drain_rem = 0;
    s_stream_active = true;
}

void audio_stop_stream(void) {
    s_stream_active = false;
}

void audio_stream_poll(void) {
    // No-op in simulator
}

uint32_t audio_ring_free(void) {
    return audio_ring_space(&s_ring);
}

void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns, uint32_t *ring_used) {
    if (isr_count) *isr_count = 0;
    if (underruns) *underruns = 0;
    if (ring_used) *ring_used = audio_ring_used(&s_ring);
}

void audio_push_samples(const int16_t *samples, int count) {
    if (!s_stream_active || !samples || count <= 0) return;
    audio_ring_push(&s_ring, samples, count);  // drops what doesn't fit
}

// The output (drivers/audio.h). Here each source queues its own audio to
// SDL, so there is no shared output: stopping it stops the stream.
void audio_output_ensure_running(void) {}
void audio_output_stop(void) { audio_stop_stream(); }
bool audio_output_running(void) { return false; }
uint32_t audio_output_isr_count(void) { return 0; }
void audio_apply_clock(void) {}
void audio_stream_reset_underruns(void) {}

// The DMA stand-in (Core 1 thread, every tick): play out the frames the
// output rate has consumed since the last call. Underrun frames are
// silence on hardware; here they are simply not queued to SDL.
static void sim_stream_drain(void) {
    uint64_t now = sim_time_us();
    if (!s_stream_active) {
        s_drain_last_us = now;
        return;
    }
    uint64_t due = (now - s_drain_last_us) * AUDIO_OUT_RATE + s_drain_rem;
    s_drain_last_us = now;
    s_drain_rem = due % 1000000u;
    uint64_t frames = due / 1000000u;
    if (frames > 2u * AUDIO_RING_SIZE)
        frames = 2u * AUDIO_RING_SIZE;  // a stalled thread: the ring is long empty

    int16_t buf[512];  // 256 stereo frames
    int n = 0;
    for (uint64_t i = 0; i < frames; i++) {
        int32_t l, r;
        if (!audio_ring_pop(&s_ring, s_stream_sample_rate, AUDIO_OUT_RATE, &l, &r))
            break;
        buf[n * 2] = (int16_t)(l * s_master_volume / 100);
        buf[n * 2 + 1] = (int16_t)(r * s_master_volume / 100);
        if (++n == 256) {
            hal_audio_push_samples(buf, n);
            n = 0;
        }
    }
    if (n)
        hal_audio_push_samples(buf, n);
}

// ══════════════════════════════════════════════════════════════════════════════
// Sound sample API
// ══════════════════════════════════════════════════════════════════════════════

// Same header parsing and clamping as src/drivers/sound.c (shared wav.c).
static bool sim_parse_wav_header(sound_sample_t *sample, uint8_t *data, uint32_t size) {
    wav_info_t info;
    wav_err_t err = wav_parse(data, size, &info);
    if (err != WAV_OK) {
        printf("sound: %s\n", wav_strerror(err));
        return false;
    }
    uint32_t data_offset = info.data_offset;
    uint32_t data_size = info.data_size;
    if (data_size > size - data_offset) data_size = size - data_offset;
    if (data_size > SOUND_MAX_SAMPLE_SIZE) data_size = SOUND_MAX_SAMPLE_SIZE;
    data_size -= data_size % info.block_align;
    if (data_size == 0) return false;

    sample->channels = (uint8_t)info.channels;
    sample->sample_rate = info.sample_rate;
    sample->bits_per_sample = (uint8_t)info.bits_per_sample;

    sample->data = malloc(data_size);
    if (!sample->data) return false;

    memcpy(sample->data, data + data_offset, data_size);
    sample->length = data_size;
    sample->loaded = true;
    return true;
}

void sound_init(void) {
    pthread_mutex_lock(&s_sound_mutex);
    // Reclaim any loaded sample data before dropping the pointers (mirrors
    // firmware sound_init; sim sample data is malloc/calloc'd, so plain free).
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        sound_sample_t *s = s_sound_ctx.samples[i];
        if (s) {
            if (s->data) free(s->data);
            free(s);
            s_sound_ctx.samples[i] = NULL;
        }
    }
    memset(&s_sound_ctx, 0, sizeof(s_sound_ctx));
    s_sound_time_us = 0;
    pthread_mutex_unlock(&s_sound_mutex);
}

void sound_update(void) {
    // Called from hal_audio_update() to mix active sound players into SDL output
    pthread_mutex_lock(&s_sound_mutex);

    bool any_playing = false;
    int16_t mix_buf[512];  // 256 stereo frames
    int mix_frames = 256;
    memset(mix_buf, 0, sizeof(mix_buf));

    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        sound_player_t *player = &s_sound_ctx.players[i];
        if (!player->playing || player->paused || !player->sample || !player->sample->loaded)
            continue;

        any_playing = true;
        sound_sample_t *sample = player->sample;

        uint32_t bytes_per_sample = sample->bits_per_sample / 8;
        uint32_t bytes_per_frame = bytes_per_sample * sample->channels;
        if (bytes_per_frame == 0) continue;

        uint32_t effective_end = sample->length;
        if (player->play_end > 0) {
            uint32_t end_bytes = player->play_end * bytes_per_frame;
            if (end_bytes < effective_end) effective_end = end_bytes;
        }
        // Clamped like src/drivers/sound.c: a start at or past the end
        // plays from the beginning.
        uint64_t start_bytes = (uint64_t)player->play_start * bytes_per_frame;
        uint32_t effective_start = start_bytes < effective_end ? (uint32_t)start_bytes : 0;

        // Calculate how many source frames to generate for target sample rate
        float rate_ratio = (float)sample->sample_rate / SIM_SAMPLE_RATE * player->rate;
        int advance_bytes = (int)(bytes_per_frame * rate_ratio);
        if (advance_bytes < 1) advance_bytes = 1;

        for (int f = 0; f < mix_frames; f++) {
            uint32_t pos = player->position;
            if (pos < effective_start) {
                player->position = effective_start;
                pos = effective_start;
            }
            if (pos + bytes_per_frame > effective_end) {
                player->repeats_played++;
                if (player->repeat_count > 0 && player->repeats_played >= player->repeat_count) {
                    player->playing = false;
                    break;
                }
                player->position = effective_start;
                pos = effective_start;
            }
            if (pos + bytes_per_frame > sample->length) break;

            int16_t left, right;
            if (sample->bits_per_sample == 16) {
                memcpy(&left, sample->data + pos, 2);
                if (sample->channels >= 2)
                    memcpy(&right, sample->data + pos + 2, 2);
                else
                    right = left;
            } else {
                left = ((int16_t)sample->data[pos] - 128) << 8;
                if (sample->channels >= 2)
                    right = ((int16_t)sample->data[pos + 1] - 128) << 8;
                else
                    right = left;
            }

            int32_t lv = (int32_t)left * player->volume / 100;
            int32_t rv = (int32_t)right * player->volume / 100;

            // Mix (additive)
            int32_t ml = (int32_t)mix_buf[f * 2 + 0] + lv;
            int32_t mr = (int32_t)mix_buf[f * 2 + 1] + rv;
            if (ml > 32767) ml = 32767; if (ml < -32768) ml = -32768;
            if (mr > 32767) mr = 32767; if (mr < -32768) mr = -32768;
            mix_buf[f * 2 + 0] = (int16_t)ml;
            mix_buf[f * 2 + 1] = (int16_t)mr;

            player->position += advance_bytes;
        }
    }

    if (any_playing) {
        // Apply master volume
        for (int i = 0; i < mix_frames * 2; i++) {
            int32_t s = (int32_t)mix_buf[i] * s_master_volume / 100;
            if (s > 32767) s = 32767; if (s < -32768) s = -32768;
            mix_buf[i] = (int16_t)s;
        }
        hal_audio_push_samples(mix_buf, mix_frames);
        s_sound_time_us += (uint32_t)(mix_frames * 1000000ULL / SIM_SAMPLE_RATE);
    }

    pthread_mutex_unlock(&s_sound_mutex);
}

sound_sample_t *sound_sample_create(void) {
    pthread_mutex_lock(&s_sound_mutex);
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        if (!s_sound_ctx.samples[i]) {
            s_sound_ctx.samples[i] = calloc(1, sizeof(sound_sample_t));
            pthread_mutex_unlock(&s_sound_mutex);
            return s_sound_ctx.samples[i];
        }
    }
    pthread_mutex_unlock(&s_sound_mutex);
    return NULL;
}

static void sim_player_stop_locked(sound_player_t *player) {
    player->playing = false;
    player->paused = false;
    player->position = 0;
    player->repeat_count = 0;
    player->repeats_played = 0;
}

void sound_sample_destroy(sound_sample_t *sample) {
    if (!sample) return;
    pthread_mutex_lock(&s_sound_mutex);
    // Detach every player still reading this sample before its data goes
    // (mirrors firmware sound.c): the Core 1 thread mixes under this mutex.
    for (int p = 0; p < SOUND_MAX_SAMPLES; p++) {
        sound_player_t *player = &s_sound_ctx.players[p];
        if (player->sample == sample) {
            sim_player_stop_locked(player);
            player->sample = NULL;
        }
    }
    if (sample->data) free(sample->data);
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        if (s_sound_ctx.samples[i] == sample) {
            s_sound_ctx.samples[i] = NULL;
            break;
        }
    }
    free(sample);
    pthread_mutex_unlock(&s_sound_mutex);
}

bool sound_sample_load(sound_sample_t *sample, const char *path) {
    if (!sample || !path) return false;

    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) {
        printf("sound: failed to open %s\n", path);
        return false;
    }

    int file_size = sdcard_fsize(path);
    if (file_size <= 0) { sdcard_fclose(f); return false; }
    if (file_size > SOUND_MAX_SAMPLE_SIZE) file_size = SOUND_MAX_SAMPLE_SIZE;

    uint8_t *data = malloc(file_size);
    if (!data) { sdcard_fclose(f); return false; }

    int bytes_read = sdcard_fread(f, data, file_size);
    sdcard_fclose(f);

    // Parse into a fresh sample and swap it in under the mixer mutex (mirrors
    // firmware sound.c): the sample may be loaded and playing already.
    sound_sample_t fresh = {0};
    if (!sim_parse_wav_header(&fresh, data, bytes_read)) {
        free(data);
        printf("sound: failed to parse WAV %s\n", path);
        return false;
    }
    free(data);
    pthread_mutex_lock(&s_sound_mutex);
    uint8_t *old = sample->data;
    sample->data = fresh.data;
    sample->length = fresh.length;
    sample->sample_rate = fresh.sample_rate;
    sample->bits_per_sample = fresh.bits_per_sample;
    sample->channels = fresh.channels;
    sample->loaded = fresh.loaded;
    for (int p = 0; p < SOUND_MAX_SAMPLES; p++) {
        if (s_sound_ctx.players[p].sample == sample)
            s_sound_ctx.players[p].position = 0;
    }
    pthread_mutex_unlock(&s_sound_mutex);
    free(old);
    printf("sound: loaded %s (%u Hz, %u bit, %u ch)\n",
           path, sample->sample_rate, sample->bits_per_sample, sample->channels);
    return true;
}

uint32_t sound_sample_get_length(const sound_sample_t *sample) {
    if (!sample || !sample->loaded) return 0;
    uint32_t bpf = sample->channels * sample->bits_per_sample / 8;
    return bpf ? sample->length / bpf : 0;
}

uint32_t sound_sample_get_sample_rate(const sound_sample_t *sample) {
    if (!sample || !sample->loaded) return 0;
    return sample->sample_rate;
}

sound_player_t *sound_player_create(void) {
    pthread_mutex_lock(&s_sound_mutex);
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        sound_player_t *p = &s_sound_ctx.players[i];
        if (!p->in_use) {
            p->volume = 100;
            p->rate = 1.0f;
            p->play_start = 0;
            p->play_end = 0;
            p->phase = 0;
            p->finish_callback = NULL;
            p->finish_callback_arg = NULL;
            p->loop_callback = NULL;
            p->loop_callback_arg = NULL;
            p->finish_pending = false;
            p->loop_pending = false;
            p->in_use = true;
            pthread_mutex_unlock(&s_sound_mutex);
            return p;
        }
    }
    pthread_mutex_unlock(&s_sound_mutex);
    return NULL;
}

// The player never owns its sample (the Lua bridge anchors it as a user
// value); destroying a player only stops it and frees the slot.
void sound_player_destroy(sound_player_t *player) {
    if (!player) return;
    pthread_mutex_lock(&s_sound_mutex);
    sim_player_stop_locked(player);
    player->sample = NULL;
    player->finish_callback = NULL;
    player->finish_callback_arg = NULL;
    player->loop_callback = NULL;
    player->loop_callback_arg = NULL;
    player->finish_pending = false;
    player->loop_pending = false;
    player->in_use = false;
    pthread_mutex_unlock(&s_sound_mutex);
}

bool sound_player_set_sample(sound_player_t *player, sound_sample_t *sample) {
    if (!player || !sample) return false;
    pthread_mutex_lock(&s_sound_mutex);
    player->sample = sample;
    player->position = 0;
    pthread_mutex_unlock(&s_sound_mutex);
    return true;
}

void sound_player_play(sound_player_t *player, uint8_t repeat_count) {
    if (!player) return;
    pthread_mutex_lock(&s_sound_mutex);
    if (player->sample && player->sample->loaded) {
        player->paused = false;
        player->repeat_count = repeat_count;
        player->repeats_played = 0;
        player->position = 0;
        player->playing = true;
    }
    pthread_mutex_unlock(&s_sound_mutex);
}

void sound_player_stop(sound_player_t *player) {
    if (!player) return;
    pthread_mutex_lock(&s_sound_mutex);
    sim_player_stop_locked(player);
    pthread_mutex_unlock(&s_sound_mutex);
}

void sound_player_set_volume(sound_player_t *player, uint8_t volume) {
    if (!player) return;
    if (volume > 100) volume = 100;
    player->volume = volume;
}

uint8_t sound_player_get_volume(const sound_player_t *player) {
    return player ? player->volume : 0;
}

bool sound_player_is_playing(const sound_player_t *player) {
    return player && player->playing;
}

void sound_player_set_play_range(sound_player_t *player, uint32_t start, uint32_t end) {
    if (!player) return;
    player->play_start = start;
    player->play_end = end;
}

void sound_player_set_rate(sound_player_t *player, float rate) {
    if (!player) return;
    if (rate < 0.0f) rate = 0.0f;
    player->rate = rate;
}

float sound_player_get_rate(const sound_player_t *player) {
    return player ? player->rate : 1.0f;
}

sound_sample_t *sound_sample_new_blank(float seconds, uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    sound_sample_t *sample = sound_sample_create();
    if (!sample) return NULL;

    uint32_t bytes_per_frame = (bits_per_sample / 8) * channels;
    uint32_t num_frames = (uint32_t)(seconds * sample_rate);
    uint32_t data_size = num_frames * bytes_per_frame;
    if (data_size > SOUND_MAX_SAMPLE_SIZE) data_size = SOUND_MAX_SAMPLE_SIZE;

    sample->data = calloc(1, data_size);
    if (!sample->data) {
        sound_sample_destroy(sample);
        return NULL;
    }

    sample->length = data_size;
    sample->sample_rate = sample_rate;
    sample->bits_per_sample = bits_per_sample;
    sample->channels = channels;
    sample->loaded = true;
    return sample;
}

sound_sample_t *sound_sample_get_subsample(const sound_sample_t *sample, uint32_t start_frame, uint32_t end_frame) {
    if (!sample || !sample->loaded || !sample->data) return NULL;

    uint32_t bytes_per_frame = (sample->bits_per_sample / 8) * sample->channels;
    uint32_t total_frames = bytes_per_frame ? sample->length / bytes_per_frame : 0;

    if (start_frame >= total_frames) start_frame = total_frames;
    if (end_frame > total_frames) end_frame = total_frames;
    if (end_frame <= start_frame) return NULL;

    uint32_t num_frames = end_frame - start_frame;
    uint32_t data_size = num_frames * bytes_per_frame;

    sound_sample_t *sub = sound_sample_create();
    if (!sub) return NULL;

    sub->data = malloc(data_size);
    if (!sub->data) {
        sound_sample_destroy(sub);
        return NULL;
    }

    memcpy(sub->data, sample->data + start_frame * bytes_per_frame, data_size);
    sub->length = data_size;
    sub->sample_rate = sample->sample_rate;
    sub->bits_per_sample = sample->bits_per_sample;
    sub->channels = sample->channels;
    sub->loaded = true;
    return sub;
}

int sound_get_playing_source_count(void) {
    int count = 0;
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        if (s_sound_ctx.players[i].playing) count++;
    }
    return count;
}

uint32_t sound_get_current_time(void) {
    return s_sound_time_us / 1000000;
}

void sound_reset_time(void) {
    s_sound_time_us = 0;
}

// ══════════════════════════════════════════════════════════════════════════════
// File Player API: the firmware src/drivers/fileplayer.c (simulator/CMakeLists)
// streams into the ring above, paced by sim_stream_drain().
// ══════════════════════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════════════════════
// MP3 Player API (libmad-based)
// ══════════════════════════════════════════════════════════════════════════════

static int skip_id3v2(struct mad_stream *stream) {
    const unsigned char *ptr = stream->buffer;
    size_t len = stream->bufend - stream->buffer;
    if (len < 10) return 0;
    if (ptr[0] == 'I' && ptr[1] == 'D' && ptr[2] == '3') {
        unsigned long size =
            ((unsigned long)(ptr[6] & 0x7f) << 21) |
            ((unsigned long)(ptr[7] & 0x7f) << 14) |
            ((unsigned long)(ptr[8] & 0x7f) << 7)  |
            ((unsigned long)(ptr[9] & 0x7f));
        size += 10;
        if (ptr[5] & 0x10) size += 10;
        if (size > len) size = len;
        mad_stream_skip(stream, size);
        return (int)size;
    }
    return 0;
}

// Mirrors src/drivers/mp3_player.c: what a refill achieved. The decode
// loop continues only on new data; retrying on a buffered partial frame
// with nothing new to add spun this thread (and the firmware's Core 1)
// forever at the end of a file that stops mid-frame.
typedef enum { MP3_REFILL_GOT_DATA, MP3_REFILL_WAIT, MP3_REFILL_EOF } mp3_refill_t;

static mp3_refill_t mp3_refill_decode_buffer(void) {
    if (s_mp3_buf_pos > 0 && s_mp3_bytes_in_buf > 0)
        memmove(s_mp3_decode_buf, s_mp3_decode_buf + s_mp3_buf_pos, s_mp3_bytes_in_buf);
    s_mp3_buf_pos = 0;

    mp3_refill_t result = MP3_REFILL_WAIT;
    int space = MP3_DECODE_BUF_SIZE - s_mp3_bytes_in_buf - MAD_BUFFER_GUARD;
    if (space > 0) {
        if (s_fed_mode) {
            uint32_t avail = fed_ring_available();
            uint32_t to_read = ((uint32_t)space < avail) ? (uint32_t)space : avail;
            if (to_read > 4096) to_read = 4096;
            if (to_read > 0) {
                fed_ring_read(s_mp3_decode_buf + s_mp3_bytes_in_buf, to_read);
                s_mp3_bytes_in_buf += (int)to_read;
                result = MP3_REFILL_GOT_DATA;
            }
        } else if (!s_mp3_file) {
            result = MP3_REFILL_EOF;
        } else {
            int to_read = (space > 4096) ? 4096 : space;
            int br = sdcard_fread(s_mp3_file, s_mp3_decode_buf + s_mp3_bytes_in_buf, to_read);
            if (br > 0) {
                s_mp3_bytes_in_buf += br;
                result = MP3_REFILL_GOT_DATA;
            } else {
                result = MP3_REFILL_EOF;
            }
        }
    }

    memset(s_mp3_decode_buf + s_mp3_bytes_in_buf, 0, MAD_BUFFER_GUARD);
    return result;
}

// End of file, no whole frame left: loop back (at most once per update) or
// finish. Returns true when decoding should go on.
static bool mp3_end_of_stream(bool *rewound) {
    if (s_mp3_player.loop && !*rewound) {
        *rewound = true;
        sdcard_fseek(s_mp3_file, 0);
        s_mp3_bytes_in_buf = 0;
        s_mp3_buf_pos = 0;
        mad_stream_init(s_mad_stream);
        mad_frame_init(s_mad_frame);
        mad_synth_init(s_mad_synth);
        return mp3_refill_decode_buffer() == MP3_REFILL_GOT_DATA;
    }
    if (!s_mp3_player.loop)
        s_mp3_player.playing = false;
    return false;
}

#define MP3_MAX_ERRORS_PER_UPDATE 32

static void mp3_decode_fill_ring(void) {
    if (!s_mp3_player.playing || s_mp3_player.paused || !s_mad_stream)
        return;
    if (!s_fed_mode && !s_mp3_file)
        return;

    size_t avail = mp3_ring_available();
    if (avail > MP3_RING_FRAMES / 2) return;

    int max_frames = 3;
    int frames_decoded = 0;
    int errors_this_update = 0;
    bool rewound = false;
    while (frames_decoded < max_frames && mp3_ring_free() >= 1152) {
        mad_stream_buffer(s_mad_stream,
                          s_mp3_decode_buf + s_mp3_buf_pos,
                          s_mp3_bytes_in_buf + MAD_BUFFER_GUARD);

        if (mad_frame_decode(s_mad_frame, s_mad_stream) != 0) {
            if (s_mad_stream->next_frame) {
                int consumed = (int)(s_mad_stream->next_frame - (s_mp3_decode_buf + s_mp3_buf_pos));
                if (consumed > 0 && consumed <= s_mp3_bytes_in_buf) {
                    s_mp3_buf_pos += consumed;
                    s_mp3_bytes_in_buf -= consumed;
                }
            }

            bool need_data = s_mad_stream->error == MAD_ERROR_BUFLEN ||
                             (s_mad_stream->error == MAD_ERROR_LOSTSYNC &&
                              s_mp3_bytes_in_buf < 256);
            if (need_data) {
                mp3_refill_t r = mp3_refill_decode_buffer();
                if (r == MP3_REFILL_GOT_DATA)
                    continue;
                if (r == MP3_REFILL_WAIT)
                    break;
                if (mp3_end_of_stream(&rewound))
                    continue;
                break;
            }

            if (MAD_RECOVERABLE(s_mad_stream->error)) {
                if (++errors_this_update >= MP3_MAX_ERRORS_PER_UPDATE)
                    break;
                continue;
            }

            printf("[MP3] libmad error: 0x%04x\n", s_mad_stream->error);
            s_mp3_player.playing = false;
            break;
        }

        // Track consumed bytes on success
        if (s_mad_stream->next_frame) {
            int consumed = (int)(s_mad_stream->next_frame - (s_mp3_decode_buf + s_mp3_buf_pos));
            if (consumed > 0 && consumed <= s_mp3_bytes_in_buf) {
                s_mp3_buf_pos += consumed;
                s_mp3_bytes_in_buf -= consumed;
            }
        }

        mad_synth_frame(s_mad_synth, s_mad_frame);
        frames_decoded++;

        struct mad_pcm *pcm = &s_mad_synth->pcm;
        unsigned int nsamples = pcm->length;

        if (pcm->channels == 2) {
            mp3_ring_write_stereo(pcm->samplesX[0], nsamples);
        } else {
            // Mono: duplicate to stereo
            int16_t stereo[2304];  // 1152 * 2
            for (unsigned int i = 0; i < nsamples; i++) {
                stereo[i * 2 + 0] = pcm->samplesX[i][0];
                stereo[i * 2 + 1] = pcm->samplesX[i][0];
            }
            mp3_ring_write_stereo(stereo, nsamples);
        }
    }
}

void mp3_player_reset(void) {
    if (!s_mp3_initialized) return;
    pthread_mutex_lock(&s_mp3_mutex);
    if (s_mp3_file) { sdcard_fclose(s_mp3_file); s_mp3_file = NULL; }
    memset(&s_mp3_player, 0, sizeof(s_mp3_player));
    s_mp3_player.volume = 100;
    s_mp3_bytes_in_buf = 0;
    s_mp3_buf_pos = 0;
    s_mp3_ring_rd = s_mp3_ring_wr = 0;
    if (s_mad_stream) mad_stream_init(s_mad_stream);
    if (s_mad_frame)  mad_frame_init(s_mad_frame);
    if (s_mad_synth)  mad_synth_init(s_mad_synth);
    pthread_mutex_unlock(&s_mp3_mutex);
}

void mp3_player_deinit(void) {
    if (!s_mp3_initialized) return;
    pthread_mutex_lock(&s_mp3_mutex);
    if (s_mp3_file) { sdcard_fclose(s_mp3_file); s_mp3_file = NULL; }
    free(s_mad_stream); s_mad_stream = NULL;
    free(s_mad_frame);  s_mad_frame = NULL;
    free(s_mad_synth);  s_mad_synth = NULL;
    free(s_mp3_pcm_ring); s_mp3_pcm_ring = NULL;
    free(s_mp3_decode_buf); s_mp3_decode_buf = NULL;
    s_mp3_initialized = false;
    pthread_mutex_unlock(&s_mp3_mutex);
}

bool mp3_player_init(void) {
    if (s_mp3_initialized) return true;

    printf("[MP3] Initializing libmad decoder (simulator)...\n");

    if (!s_mad_stream) {
        s_mad_stream = malloc(sizeof(struct mad_stream));
        if (!s_mad_stream) { printf("[MP3] FAILED to alloc mad_stream\n"); return false; }
    }
    if (!s_mad_frame) {
        s_mad_frame = malloc(sizeof(struct mad_frame));
        if (!s_mad_frame) { printf("[MP3] FAILED to alloc mad_frame\n"); return false; }
    }
    if (!s_mad_synth) {
        s_mad_synth = malloc(sizeof(struct mad_synth));
        if (!s_mad_synth) { printf("[MP3] FAILED to alloc mad_synth\n"); return false; }
    }

    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);

    if (!s_mp3_decode_buf) {
        s_mp3_decode_buf = malloc(MP3_DECODE_BUF_SIZE);
        if (!s_mp3_decode_buf) { printf("[MP3] FAILED to alloc decode buffer\n"); return false; }
    }

    if (!s_mp3_pcm_ring) {
        s_mp3_pcm_ring = malloc(MP3_RING_FRAMES * 2 * sizeof(int16_t));
        if (!s_mp3_pcm_ring) { printf("[MP3] FAILED to alloc PCM ring\n"); return false; }
    }

    memset(&s_mp3_player, 0, sizeof(s_mp3_player));
    s_mp3_player.volume = 100;
    s_mp3_initialized = true;
    printf("[MP3] Initialized (simulator)\n");
    return true;
}

mp3_player_t *mp3_player_create(void) {
    if (!s_mp3_initialized) mp3_player_init();
    return &s_mp3_player;
}

void mp3_player_destroy(mp3_player_t *player) {
    (void)player;
    mp3_player_stop(player);
}

bool mp3_player_load(mp3_player_t *player, const char *path) {
    if (!player || !path) return false;

    pthread_mutex_lock(&s_mp3_mutex);

    player->playing = false;
    player->paused = false;

    if (s_mp3_file) { sdcard_fclose(s_mp3_file); s_mp3_file = NULL; }

    s_mp3_file = sdcard_fopen(path, "rb");
    if (!s_mp3_file) {
        printf("mp3_player: failed to open %s\n", path);
        pthread_mutex_unlock(&s_mp3_mutex);
        return false;
    }

    int rd = sdcard_fread(s_mp3_file, s_mp3_decode_buf, MP3_DECODE_BUF_SIZE - MAD_BUFFER_GUARD);
    if (rd <= 0) {
        sdcard_fclose(s_mp3_file); s_mp3_file = NULL;
        pthread_mutex_unlock(&s_mp3_mutex);
        return false;
    }
    s_mp3_bytes_in_buf = rd;
    s_mp3_buf_pos = 0;
    s_mp3_ring_rd = s_mp3_ring_wr = 0;

    memset(s_mp3_decode_buf + s_mp3_bytes_in_buf, 0, MAD_BUFFER_GUARD);

    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);

    mad_stream_buffer(s_mad_stream, s_mp3_decode_buf, s_mp3_bytes_in_buf + MAD_BUFFER_GUARD);

    int tag_len = skip_id3v2(s_mad_stream);
    if (tag_len > 0) {
        s_mp3_buf_pos += tag_len;
        s_mp3_bytes_in_buf -= tag_len;
    }

    if (mad_header_decode(&s_mad_frame->header, s_mad_stream) != 0) {
        printf("mp3_player: not an MP3 file (%s)\n", path);
        sdcard_fclose(s_mp3_file); s_mp3_file = NULL;
        pthread_mutex_unlock(&s_mp3_mutex);
        return false;
    }

    player->sample_rate = s_mad_frame->header.samplerate;
    player->channels = MAD_NCHANNELS(&s_mad_frame->header);
    player->length = 0;
    player->position = 0;

    printf("mp3_player: loaded %s (%u Hz, %u ch)\n", path,
           (unsigned)player->sample_rate, (unsigned)player->channels);

    // Re-init for clean decode
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);
    s_mp3_buf_pos = 0;

    pthread_mutex_unlock(&s_mp3_mutex);
    return true;
}

bool mp3_player_play(mp3_player_t *player, uint8_t repeat_count) {
    (void)repeat_count;
    if (!player || !s_mp3_file) return false;

    pthread_mutex_lock(&s_mp3_mutex);
    player->playing = true;
    player->paused = false;
    s_mp3_ring_rd = s_mp3_ring_wr = 0;

    // Pre-fill ring
    mp3_decode_fill_ring();

    pthread_mutex_unlock(&s_mp3_mutex);
    return true;
}

void mp3_player_stop(mp3_player_t *player) {
    if (!player) return;
    pthread_mutex_lock(&s_mp3_mutex);
    player->playing = false;
    player->paused = false;
    player->position = 0;
    if (s_mp3_file) { sdcard_fclose(s_mp3_file); s_mp3_file = NULL; }
    s_mp3_bytes_in_buf = 0;
    s_mp3_buf_pos = 0;
    s_mp3_ring_rd = s_mp3_ring_wr = 0;
    pthread_mutex_unlock(&s_mp3_mutex);
}

void mp3_player_pause(mp3_player_t *player) {
    if (!player || !player->playing) return;
    pthread_mutex_lock(&s_mp3_mutex);
    player->paused = true;
    pthread_mutex_unlock(&s_mp3_mutex);
}

void mp3_player_resume(mp3_player_t *player) {
    if (!player || !player->paused || !player->playing) return;
    pthread_mutex_lock(&s_mp3_mutex);
    player->paused = false;
    pthread_mutex_unlock(&s_mp3_mutex);
}

bool mp3_player_is_playing(const mp3_player_t *player) {
    return player && player->playing && !player->paused;
}

uint32_t mp3_player_get_position(const mp3_player_t *player) {
    if (!player) return 0;
    return player->position;
}

uint32_t mp3_player_get_length(const mp3_player_t *player) {
    if (!player) return 0;
    return player->length;
}

void mp3_player_set_volume(mp3_player_t *player, uint8_t volume) {
    if (!player) return;
    if (volume > 100) volume = 100;
    player->volume = volume;
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

// Called from Core 1 thread every 5ms
void mp3_player_update(void) {
    if (!s_mp3_initialized) return;
    if (pthread_mutex_trylock(&s_mp3_mutex) != 0) return;

    if (s_mp3_player.playing && !s_mp3_player.paused) {
        // Backpressure: skip decode/drain if SDL already has enough queued audio
        Uint32 queued = SDL_GetQueuedAudioSize(1);
        if (queued > 8820) {  // ~50ms at 44100Hz stereo 16-bit
            pthread_mutex_unlock(&s_mp3_mutex);
            return;
        }

        mp3_decode_fill_ring();

        // Drain ring buffer to SDL audio with resampling
        size_t avail = mp3_ring_available();
        if (avail > 1) {
            double ratio = (double)s_mp3_player.sample_rate / SIM_SAMPLE_RATE;
            int16_t buf[512];  // 256 stereo frames

            while (avail > 1) {
                uint32_t dst_frames = (uint32_t)(avail / ratio);
                if (dst_frames == 0) break;
                if (dst_frames > 256) dst_frames = 256;

                // How many source frames this batch consumes
                uint32_t src_consumed = (uint32_t)(dst_frames * ratio) + 1;
                if (src_consumed > avail) src_consumed = avail;

                size_t rd = s_mp3_ring_rd;
                for (uint32_t i = 0; i < dst_frames; i++) {
                    double src_idx = i * ratio;
                    uint32_t idx0 = (uint32_t)src_idx;
                    double frac = src_idx - idx0;
                    uint32_t idx1 = idx0 + 1;
                    if (idx1 >= src_consumed) idx1 = src_consumed - 1;

                    size_t ri0 = (rd + idx0) % MP3_RING_FRAMES;
                    size_t ri1 = (rd + idx1) % MP3_RING_FRAMES;

                    int32_t l = (int32_t)(s_mp3_pcm_ring[ri0 * 2] +
                                (s_mp3_pcm_ring[ri1 * 2] - s_mp3_pcm_ring[ri0 * 2]) * frac);
                    int32_t r = (int32_t)(s_mp3_pcm_ring[ri0 * 2 + 1] +
                                (s_mp3_pcm_ring[ri1 * 2 + 1] - s_mp3_pcm_ring[ri0 * 2 + 1]) * frac);

                    l = l * s_mp3_player.volume / 100;
                    r = r * s_mp3_player.volume / 100;
                    if (l > 32767) l = 32767; if (l < -32768) l = -32768;
                    if (r > 32767) r = 32767; if (r < -32768) r = -32768;
                    buf[i * 2 + 0] = (int16_t)l;
                    buf[i * 2 + 1] = (int16_t)r;
                }
                s_mp3_ring_rd = (rd + src_consumed) % MP3_RING_FRAMES;
                s_mp3_player.position += src_consumed;

                // Apply master volume and push to SDL
                for (uint32_t i = 0; i < dst_frames * 2; i++) {
                    int32_t s = (int32_t)buf[i] * s_master_volume / 100;
                    if (s > 32767) s = 32767; if (s < -32768) s = -32768;
                    buf[i] = (int16_t)s;
                }
                hal_audio_push_samples(buf, dst_frames);

                avail -= src_consumed;
            }
        }
    }

    pthread_mutex_unlock(&s_mp3_mutex);
}

// ── Fed mode API (video player audio) ─────────────────────────────────────────

bool mp3_player_start_fed(uint32_t sample_rate, uint16_t channels) {
    if (!s_mp3_initialized) {
        if (!mp3_player_init()) return false;
    }

    pthread_mutex_lock(&s_mp3_mutex);

    s_mp3_player.playing = false;
    if (s_mp3_file) { sdcard_fclose(s_mp3_file); s_mp3_file = NULL; }

    // Allocate fed ring if needed
    if (!s_fed_ring) {
        s_fed_ring = malloc(FED_RING_SIZE);
        if (!s_fed_ring) {
            pthread_mutex_unlock(&s_mp3_mutex);
            return false;
        }
    }

    s_fed_mode = true;
    s_fed_wr = 0;
    s_fed_rd = 0;

    s_mp3_bytes_in_buf = 0;
    s_mp3_buf_pos = 0;
    s_mp3_ring_rd = s_mp3_ring_wr = 0;

    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);

    memset(&s_mp3_player, 0, sizeof(s_mp3_player));
    s_mp3_player.sample_rate = sample_rate;
    s_mp3_player.channels = channels;
    s_mp3_player.playing = true;
    s_mp3_player.volume = 100;

    printf("[MP3] Fed mode started (sim): %u Hz, %u ch\n",
           (unsigned)sample_rate, (unsigned)channels);

    pthread_mutex_unlock(&s_mp3_mutex);
    return true;
}

uint32_t mp3_player_feed(const uint8_t *data, uint32_t len) {
    if (!s_fed_mode || !s_fed_ring) return 0;

    uint32_t free_space = fed_ring_free();
    uint32_t to_write = (len < free_space) ? len : free_space;
    if (to_write == 0) return 0;

    uint32_t wr = s_fed_wr;
    uint32_t to_end = FED_RING_SIZE - wr;
    if (to_write <= to_end) {
        memcpy(s_fed_ring + wr, data, to_write);
    } else {
        memcpy(s_fed_ring + wr, data, to_end);
        memcpy(s_fed_ring, data + to_end, to_write - to_end);
    }
    s_fed_wr = (wr + to_write) % FED_RING_SIZE;
    return to_write;
}

void mp3_player_stop_fed(void) {
    if (!s_fed_mode) return;

    pthread_mutex_lock(&s_mp3_mutex);
    s_mp3_player.playing = false;
    s_fed_mode = false;
    s_fed_wr = 0;
    s_fed_rd = 0;
    s_mp3_bytes_in_buf = 0;
    s_mp3_buf_pos = 0;
    s_mp3_ring_rd = s_mp3_ring_wr = 0;
    printf("[MP3] Fed mode stopped (sim)\n");
    pthread_mutex_unlock(&s_mp3_mutex);
}

uint32_t mp3_player_feed_space(void) {
    if (!s_fed_mode) return 0;
    return fed_ring_free();
}

bool mp3_player_is_fed_mode(void) {
    return s_fed_mode;
}

void mp3_player_start_dma_fed(void) {
    // Simulator: no DMA, decode happens in mp3_player_update() via SDL
}

// ══════════════════════════════════════════════════════════════════════════════
// hal_audio_update enhancement — drives tone generation + sound player mixing
// ══════════════════════════════════════════════════════════════════════════════

void hal_audio_update(void) {
    // Tone generation
    if (s_tone_playing) {
        if (s_tone_end_ms > 0 && sim_time_ms() >= s_tone_end_ms) {
            s_tone_playing = false;
        } else {
            // Generate square wave samples
            int16_t buf[512];  // 256 stereo frames
            int frames = 256;
            for (int i = 0; i < frames; i++) {
                // Square wave: half period high, half period low
                uint32_t half_period = SIM_SAMPLE_RATE / (s_tone_freq * 2);
                if (half_period == 0) half_period = 1;
                int16_t val = ((s_tone_phase / half_period) % 2 == 0) ? 16384 : -16384;
                int32_t scaled = (int32_t)val * s_master_volume / 100;
                buf[i * 2 + 0] = (int16_t)scaled;
                buf[i * 2 + 1] = (int16_t)scaled;
                s_tone_phase++;
            }
            hal_audio_push_samples(buf, frames);
        }
    }

    // Sound sample player mixing
    sound_update();

    // File player streaming, then the stream's paced drain
    fileplayer_update();
    sim_stream_drain();

    // MP3 decode + playback
    mp3_player_update();
}
