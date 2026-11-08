#pragma once

#include <stdint.h>
#include <stdbool.h>

// Sample players that exist at once: the mixer's voices. Samples have no
// fixed limit (each is a umm allocation until destroyed).
#define SOUND_MAX_PLAYERS 8
#define SOUND_MAX_SAMPLE_SIZE (64 * 1024)

typedef struct sound_sample {
    uint8_t *data;
    uint32_t length;
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
    bool loaded;
    struct sound_sample *prev, *next;  // every live sample (sound_init frees them)
} sound_sample_t;

typedef struct {
    sound_sample_t *sample;
    bool in_use;        // slot allocated: sound_player_create .. sound_player_destroy
    bool playing;
    bool paused;
    bool loop;          // sound_player_set_loop: repeat until stopped, whatever play's count
    uint32_t position;
    uint32_t phase;     // rate-conversion accumulator (Hz units at AUDIO_OUT_RATE)
    uint8_t volume;
    uint8_t repeat_count;
    uint8_t repeats_played;
    uint32_t play_start;
    uint32_t play_end;
    float rate;
    int (*finish_callback)(void *);
    void *finish_callback_arg;
    int (*loop_callback)(void *);
    void *loop_callback_arg;
    volatile bool finish_pending;  // set by mixer (Core 1 ISR), fired by pump
    volatile bool loop_pending;
    volatile bool in_pump;         // the pump is calling this player's callbacks
} sound_player_t;

void sound_init(void);

/* The mixer is driven from audio.c's DMA refill hook (Core 1 ISR) — no
 * playback timer exists anymore. sound_mixer_process adds up to
 * SOUND_MAX_PLAYERS players' PCM into out_l/out_r (int32 accumulation,
 * clipped by the caller) at AUDIO_OUT_RATE frames. sound_pump_callbacks
 * fires deferred finish/loop callbacks from the Core 1 work pump. */
void sound_mixer_process(int32_t *out_l, int32_t *out_r, int frames);
void sound_pump_callbacks(void);
// Core 1's tick gate (audio_core1_idle, while the output runs): true when no voice has a
// callback waiting for sound_pump_callbacks. Reads only SRAM, from RAM.
bool sound_core1_idle(void);

sound_sample_t *sound_sample_create(void);
/* Frees the sample. Any player still pointing at it is stopped and detached
 * first, under the mixer lock, so the mixer never reads freed data. */
void sound_sample_destroy(sound_sample_t *sample);
bool sound_sample_load(sound_sample_t *sample, const char *path);
uint32_t sound_sample_get_length(const sound_sample_t *sample);
uint32_t sound_sample_get_sample_rate(const sound_sample_t *sample);

sound_player_t *sound_player_create(void);
/* Stops the player and frees its slot. The player never owns its sample:
 * whoever created the sample frees it (the Lua bridge keeps it alive as a
 * uservalue of the player userdata). */
void sound_player_destroy(sound_player_t *player);
bool sound_player_set_sample(sound_player_t *player, sound_sample_t *sample);
void sound_player_play(sound_player_t *player, uint8_t repeat_count);
void sound_player_stop(sound_player_t *player);
// true: repeat until stopped, also through later play() calls whatever their
// count. false: play() counts again, and a sound now repeating for ever ends at
// the end of this pass.
void sound_player_set_loop(sound_player_t *player, bool loop);
void sound_player_set_volume(sound_player_t *player, uint8_t volume);
uint8_t sound_player_get_volume(const sound_player_t *player);
bool sound_player_is_playing(const sound_player_t *player);
void sound_player_set_play_range(sound_player_t *player, uint32_t start, uint32_t end);
void sound_player_set_rate(sound_player_t *player, float rate);
float sound_player_get_rate(const sound_player_t *player);
void sound_player_set_finish_callback(sound_player_t *player, int (*cb)(void *), void *arg);
void sound_player_set_loop_callback(sound_player_t *player, int (*cb)(void *), void *arg);

/* Parking: a player's state moved off its voice into a copy the mixer never
 * reads, so the voice is free for another player while the owner of the copy
 * (the Lua bridge's temporary players) can still read it and play it again.
 * Core 0 only. A park is refused (false, nothing changes) while the voice is
 * audible, the mixer's flag for a callback waits for sound_pump_callbacks, the
 * pump is calling a callback, or a callback it called has left work for its owner:
 * cb_busy(arg) true for either callback's argument (NULL: none). All of it is one
 * step under the mixer lock, so cb_busy only reads, and a callback is never caught
 * between the pump taking the flag and the owner seeing what it did. */
bool sound_player_park(sound_player_t *voice, sound_player_t *parked,
                       bool (*cb_busy)(void *arg));
void sound_player_unpark(sound_player_t *voice, const sound_player_t *parked);
/* A voice's index, 0 .. SOUND_MAX_PLAYERS - 1. */
int sound_player_voice(const sound_player_t *voice);
/* Bit i set: voice i has a player that a park may take (not audible: stopped,
 * finished or paused). No lock: sound_player_park decides. */
uint32_t sound_voices_parkable(void);

sound_sample_t *sound_sample_new_blank(float seconds, uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
sound_sample_t *sound_sample_get_subsample(const sound_sample_t *sample, uint32_t start_frame, uint32_t end_frame);
int sound_get_playing_source_count(void);

/* The audio clock: milliseconds of output rendered since sound_init() or
 * sound_reset_time(). It stands still until the output starts (the app's first
 * sound) and wraps after ~27 hours (2^32 frames at 44.1 kHz). */
uint32_t sound_get_current_time(void);
void sound_reset_time(void);
