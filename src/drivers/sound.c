#include "sound.h"
#include "audio.h"
#include "../hardware.h"
#include "sdcard.h"
#include "pico/time.h"
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "pico/critical_section.h"
#include "umm_malloc.h"
#include "wav.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Samples: a list of umm (PSRAM) allocations with no fixed limit, touched
// only on Core 0 (create, destroy, sound_init); the mixer reaches samples
// through players[].sample alone. Players: the mixer's SOUND_MAX_PLAYERS
// voices.
static struct {
    sound_sample_t *samples;
    sound_player_t players[SOUND_MAX_PLAYERS];
    uint32_t time_offset_us;
} s_context;

/* The mixer runs in audio.c's DMA refill ISR on Core 1 and reads
 * s_context.players[] and their samples. Everything that changes which
 * sample a player reads, or frees sample data, holds this lock (a spin lock
 * with IRQs off). The ISR holds it for one mix chunk: 32 frames (audio.c's
 * MIX_CHUNK) x up to SOUND_MAX_PLAYERS players, tens of microseconds, four
 * times per 128-frame DMA buffer; Core 0 holds it for field updates only.
 * Never nested: helpers named *_locked expect it held.
 *
 * Which spin lock: critical_section_init() would claim one of the eight
 * exclusive locks (24-31) that http.c/tcp.c claim per connection, so the
 * mixer uses a striped lock (16-23, shared only with the SDK's mutexes and
 * queues, whose hold times are a few instructions). It can never be
 * g_umm_critsec's lock: that one is claimed (lua_psram_alloc.c), so the
 * Core 1 audio path never waits on a Core 0 umm heap walk. */
static critical_section_t s_mix_cs;
static bool s_mix_cs_ready;

static __force_inline void mix_lock(void) {
    if (s_mix_cs_ready)
        critical_section_enter_blocking(&s_mix_cs);
}

static __force_inline void mix_unlock(void) {
    if (s_mix_cs_ready)
        critical_section_exit(&s_mix_cs);
}

static bool parse_wav_header(sound_sample_t *sample, uint8_t *data, uint32_t size) {
    wav_info_t info;
    wav_err_t err = wav_parse(data, size, &info);
    if (err != WAV_OK) {
        printf("sound: %s\n", wav_strerror(err));
        return false;
    }

    // Keep what was read (the file is capped at SOUND_MAX_SAMPLE_SIZE), in
    // whole frames.
    uint32_t data_offset = info.data_offset;
    uint32_t data_size = info.data_size;
    if (data_size > size - data_offset)
        data_size = size - data_offset;
    if (data_size > SOUND_MAX_SAMPLE_SIZE)
        data_size = SOUND_MAX_SAMPLE_SIZE;
    data_size -= data_size % info.block_align;
    if (data_size == 0)
        return false;

    sample->channels = (uint8_t)info.channels;
    sample->sample_rate = info.sample_rate;
    sample->bits_per_sample = (uint8_t)info.bits_per_sample;

    sample->data = umm_malloc(data_size);
    if (!sample->data)
        return false;

    memcpy(sample->data, data + data_offset, data_size);
    sample->length = data_size;
    sample->loaded = true;

    return true;
}

void sound_init(void) {
    if (!s_mix_cs_ready) {
        critical_section_init_with_lock_num(&s_mix_cs, next_striped_spin_lock_num());
        s_mix_cs_ready = true;
    }
    // Reclaim every sample (app exit must not leak PSRAM). Detach the
    // players under the lock first: the mixer may still be running for a
    // tone, a stream or the MP3.
    mix_lock();
    sound_sample_t *s = s_context.samples;
    memset(&s_context, 0, sizeof(s_context));
    mix_unlock();
    while (s) {
        sound_sample_t *next = s->next;
        if (s->data) umm_free(s->data);
        umm_free(s);
        s = next;
    }
}

/* Mixer entry point: called from audio.c's DMA refill hook (Core 1 ISR) to
 * produce `frames` frames of mixed sample audio at AUDIO_OUT_RATE. Each
 * active player advances through its sample data via a phase accumulator
 * (nearest-neighbor rate conversion). Accumulates into out_l/out_r (int32)
 * — the caller clips to int16 when mixing with other sources. */
void __time_critical_func(sound_mixer_process)(int32_t *out_l, int32_t *out_r, int frames) {
    memset(out_l, 0, frames * sizeof(*out_l));
    memset(out_r, 0, frames * sizeof(*out_r));

    mix_lock();
    for (int p = 0; p < SOUND_MAX_PLAYERS; p++) {
        sound_player_t *player = &s_context.players[p];
        if (!player->playing || player->paused || !player->sample || !player->sample->loaded)
            continue;

        sound_sample_t *sample = player->sample;
        uint32_t bpf = (sample->bits_per_sample / 8) * sample->channels;

        // The play range, clamped to the sample on both ends: a start at or
        // past the end (setPlayRange(start) beyond the sample, or start >=
        // end) used to send the loop reset below to data past the buffer.
        uint32_t effective_end = sample->length;
        if (player->play_end > 0) {
            uint64_t end_bytes = (uint64_t)player->play_end * bpf;
            if (end_bytes < effective_end)
                effective_end = (uint32_t)end_bytes;
        }
        if (bpf == 0 || effective_end < bpf)
            continue;  // nothing playable
        uint64_t start_bytes = (uint64_t)player->play_start * bpf;
        uint32_t effective_start = start_bytes < effective_end ? (uint32_t)start_bytes : 0;
        if (player->position < effective_start)
            player->position = effective_start;

        uint32_t step = (uint32_t)(sample->sample_rate * player->rate);

        for (int i = 0; i < frames; i++) {
            if (!player->playing)
                break;
            uint32_t pos = player->position;
            if (pos >= effective_end) {
                player->repeats_played++;
                if (player->repeat_count > 0 && player->repeats_played >= player->repeat_count) {
                    player->playing = false;
                    player->position = effective_start;
                    player->finish_pending = true;
                    break;
                }
                player->loop_pending = true;
                player->position = effective_start;
                pos = effective_start;
            }

            int16_t l16, r16;
            if (sample->bits_per_sample == 16) {
                l16 = *(int16_t *)(sample->data + pos);
                r16 = sample->channels >= 2 ? *(int16_t *)(sample->data + pos + 2) : l16;
            } else {
                l16 = ((int16_t)sample->data[pos] - 128) << 8;
                r16 = sample->channels >= 2 ? ((int16_t)sample->data[pos + 1] - 128) << 8 : l16;
            }
            out_l[i] += (l16 * player->volume) / 100;
            out_r[i] += (r16 * player->volume) / 100;

            player->phase += step;
            while (player->phase >= AUDIO_OUT_RATE) {
                player->phase -= AUDIO_OUT_RATE;
                player->position += bpf;
            }
        }
    }
    // Keep the public time base advancing (~us of audio mixed)
    s_context.time_offset_us += (uint32_t)(((uint64_t)frames * 1000000) / AUDIO_OUT_RATE);
    mix_unlock();
}

/* Fires deferred finish/loop callbacks — call from the Core 1 work pump
 * (NOT from the mixer ISR; callbacks trampoline into Lua). */
void sound_pump_callbacks(void) {
    for (int p = 0; p < SOUND_MAX_PLAYERS; p++) {
        sound_player_t *player = &s_context.players[p];
        if (!player->finish_pending && !player->loop_pending)
            continue;
        // Take the flags and the callbacks together under the lock, so a
        // player destroyed on Core 0 (which clears them under the same
        // lock) is never called back with its old arguments.
        int (*fin)(void *) = NULL, (*loop)(void *) = NULL;
        void *fin_arg = NULL, *loop_arg = NULL;
        mix_lock();
        if (player->finish_pending) {
            player->finish_pending = false;
            fin = player->finish_callback;
            fin_arg = player->finish_callback_arg;
        }
        if (player->loop_pending) {
            player->loop_pending = false;
            loop = player->loop_callback;
            loop_arg = player->loop_callback_arg;
        }
        mix_unlock();
        if (fin)
            fin(fin_arg);
        if (loop)
            loop(loop_arg);
    }
}

sound_sample_t *sound_sample_create(void) {
    sound_sample_t *s = umm_malloc(sizeof(*s));
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    s->next = s_context.samples;
    if (s->next)
        s->next->prev = s;
    s_context.samples = s;
    return s;
}

/* Replaces sample's contents with fresh's under the mixer lock and rewinds
 * any player using it (its position may be past the new end). Returns the
 * old data for the caller to free outside the lock. */
static uint8_t *swap_sample_data(sound_sample_t *sample, const sound_sample_t *fresh) {
    mix_lock();
    uint8_t *old = sample->data;
    sample->data = fresh->data;
    sample->length = fresh->length;
    sample->sample_rate = fresh->sample_rate;
    sample->bits_per_sample = fresh->bits_per_sample;
    sample->channels = fresh->channels;
    sample->loaded = fresh->loaded;
    for (int p = 0; p < SOUND_MAX_PLAYERS; p++) {
        sound_player_t *player = &s_context.players[p];
        if (player->sample == sample) {
            player->position = 0;
            player->phase = 0;
        }
    }
    mix_unlock();
    return old;
}

static void player_stop_locked(sound_player_t *player) {
    player->playing = false;
    player->paused = false;
    player->position = 0;
    player->repeat_count = 0;
    player->repeats_played = 0;
}

void sound_sample_destroy(sound_sample_t *sample) {
    if (!sample)
        return;
    // Detach every player still reading this sample before its data goes:
    // the mixer re-reads player->sample under the same lock, so once this
    // returns nothing on Core 1 can hold the pointer.
    mix_lock();
    for (int p = 0; p < SOUND_MAX_PLAYERS; p++) {
        sound_player_t *player = &s_context.players[p];
        if (player->sample == sample) {
            player_stop_locked(player);
            player->sample = NULL;
        }
    }
    mix_unlock();
    // Off the list (Core 0 only, so outside the mixer lock).
    if (sample->prev)
        sample->prev->next = sample->next;
    else
        s_context.samples = sample->next;
    if (sample->next)
        sample->next->prev = sample->prev;
    if (sample->data)
        umm_free(sample->data);
    umm_free(sample);
}

bool sound_sample_load(sound_sample_t *sample, const char *path) {
    if (!sample || !path)
        return false;

    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) {
        printf("sound: failed to open %s\n", path);
        return false;
    }

    uint8_t header[44];
    int read = sdcard_fread(f, header, sizeof(header));
    if (read < 44) {
        sdcard_fclose(f);
        printf("sound: file too small\n");
        return false;
    }

    uint32_t file_size = sdcard_fsize(path);
    file_size = file_size > SOUND_MAX_SAMPLE_SIZE ? SOUND_MAX_SAMPLE_SIZE : file_size;

    sdcard_fseek(f, 0);
    uint8_t *data = umm_malloc(file_size);
    if (!data) {
        sdcard_fclose(f);
        return false;
    }

    uint32_t bytes_read = sdcard_fread(f, data, file_size);
    sdcard_fclose(f);

    // Parse into a fresh sample, then swap it in under the mixer lock: the
    // sample may already be loaded and playing (sample:load() on a live
    // sample), and the old data is freed only once no player can read it.
    sound_sample_t fresh = {0};
    if (!parse_wav_header(&fresh, data, bytes_read)) {
        umm_free(data);
        printf("sound: failed to parse WAV\n");
        return false;
    }
    umm_free(data);
    uint8_t *old = swap_sample_data(sample, &fresh);
    if (old)
        umm_free(old);
    printf("sound: loaded %s (%lu Hz, %u bit, %u ch)\n",
           path, (unsigned long)sample->sample_rate, sample->bits_per_sample,
           sample->channels);
    return true;
}

uint32_t sound_sample_get_length(const sound_sample_t *sample) {
    if (!sample || !sample->loaded)
        return 0;
    return sample->length / (sample->channels * sample->bits_per_sample / 8);
}

uint32_t sound_sample_get_sample_rate(const sound_sample_t *sample) {
    if (!sample || !sample->loaded)
        return 0;
    return sample->sample_rate;
}

sound_player_t *sound_player_create(void) {
    for (int i = 0; i < SOUND_MAX_PLAYERS; i++) {
        sound_player_t *player = &s_context.players[i];
        if (!player->in_use) {
            // A free slot is stopped and detached (sound_player_destroy),
            // so the mixer skips it while it is being set up.
            player->volume = 100;
            player->play_start = 0;
            player->play_end = 0;
            player->rate = 1.0f;
            player->phase = 0;
            player->finish_pending = false;
            player->loop_pending = false;
            player->finish_callback = NULL;
            player->finish_callback_arg = NULL;
            player->loop_callback = NULL;
            player->loop_callback_arg = NULL;
            player->in_use = true;
            return player;
        }
    }
    return NULL;
}

void sound_player_destroy(sound_player_t *player) {
    if (!player)
        return;
    mix_lock();
    player_stop_locked(player);
    player->sample = NULL;
    player->finish_callback = NULL;
    player->finish_callback_arg = NULL;
    player->loop_callback = NULL;
    player->loop_callback_arg = NULL;
    player->finish_pending = false;
    player->loop_pending = false;
    player->in_use = false;
    mix_unlock();
}

bool sound_player_set_sample(sound_player_t *player, sound_sample_t *sample) {
    if (!player || !sample)
        return false;
    mix_lock();
    player->sample = sample;
    player->position = 0;
    player->phase = 0;
    mix_unlock();
    return true;
}

void sound_player_play(sound_player_t *player, uint8_t repeat_count) {
    if (!player)
        return;
    mix_lock();
    if (!player->sample || !player->sample->loaded) {
        mix_unlock();
        return;
    }
    player->paused = false;
    player->repeat_count = repeat_count;
    player->repeats_played = 0;
    player->position = 0;
    player->phase = 0;
    player->finish_pending = false;
    player->loop_pending = false;
    player->playing = true;
    mix_unlock();

    // Samples are mixed by audio_mix.c's render: the output must be running.
    audio_output_ensure_running();
}

void sound_player_stop(sound_player_t *player) {
    if (!player)
        return;
    mix_lock();
    player_stop_locked(player);
    mix_unlock();
}

void sound_player_set_volume(sound_player_t *player, uint8_t volume) {
    if (!player)
        return;
    if (volume > 100)
        volume = 100;
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

void sound_player_set_finish_callback(sound_player_t *player, int (*cb)(void *), void *arg) {
    if (!player) return;
    mix_lock();
    player->finish_callback = cb;
    player->finish_callback_arg = arg;
    mix_unlock();
}

void sound_player_set_loop_callback(sound_player_t *player, int (*cb)(void *), void *arg) {
    if (!player) return;
    mix_lock();
    player->loop_callback = cb;
    player->loop_callback_arg = arg;
    mix_unlock();
}

sound_sample_t *sound_sample_new_blank(float seconds, uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    sound_sample_t *sample = sound_sample_create();
    if (!sample) return NULL;

    uint32_t bytes_per_frame = (bits_per_sample / 8) * channels;
    uint32_t num_frames = (uint32_t)(seconds * sample_rate);
    uint32_t data_size = num_frames * bytes_per_frame;

    if (data_size > SOUND_MAX_SAMPLE_SIZE)
        data_size = SOUND_MAX_SAMPLE_SIZE;

    sample->data = umm_malloc(data_size);
    if (!sample->data) {
        sound_sample_destroy(sample);
        return NULL;
    }

    memset(sample->data, 0, data_size);
    sample->length = data_size;
    sample->sample_rate = sample_rate;
    sample->bits_per_sample = bits_per_sample;
    sample->channels = channels;
    sample->loaded = true;

    return sample;
}

sound_sample_t *sound_sample_get_subsample(const sound_sample_t *sample, uint32_t start_frame, uint32_t end_frame) {
    if (!sample || !sample->loaded || !sample->data)
        return NULL;

    uint32_t bytes_per_frame = (sample->bits_per_sample / 8) * sample->channels;
    uint32_t total_frames = sample->length / bytes_per_frame;

    if (start_frame >= total_frames) start_frame = total_frames;
    if (end_frame > total_frames) end_frame = total_frames;
    if (end_frame <= start_frame) return NULL;

    uint32_t num_frames = end_frame - start_frame;
    uint32_t data_size = num_frames * bytes_per_frame;

    sound_sample_t *sub = sound_sample_create();
    if (!sub) return NULL;

    sub->data = umm_malloc(data_size);
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
    for (int i = 0; i < SOUND_MAX_PLAYERS; i++) {
        if (s_context.players[i].playing)
            count++;
    }
    return count;
}

uint32_t sound_get_current_time(void) {
    return s_context.time_offset_us / 1000000;
}

void sound_reset_time(void) {
    s_context.time_offset_us = 0;
}
