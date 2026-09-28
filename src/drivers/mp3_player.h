#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MP3_WORKING_BUFFER_SIZE 8192

typedef struct {
    void *decoder;
    uint8_t *working_buffer;
    bool playing;
    bool paused;
    uint32_t length;
    uint8_t volume;
    bool loop;
    uint32_t sample_rate;
    uint16_t channels;
} mp3_player_t;

bool mp3_player_init(void);
void mp3_player_deinit(void);
void mp3_player_reset(void);
mp3_player_t *mp3_player_create(void);
void mp3_player_destroy(mp3_player_t *player);
bool mp3_player_load(mp3_player_t *player, const char *path);
bool mp3_player_play(mp3_player_t *player, uint8_t repeat_count);
void mp3_player_stop(mp3_player_t *player);
void mp3_player_pause(mp3_player_t *player);
void mp3_player_resume(mp3_player_t *player);
bool mp3_player_is_playing(const mp3_player_t *player);
// Content frames played since play() (keeps counting across loops).
uint32_t mp3_player_get_position(const mp3_player_t *player);
uint32_t mp3_player_get_length(const mp3_player_t *player);
void mp3_player_set_volume(mp3_player_t *player, uint8_t volume);
uint8_t mp3_player_get_volume(const mp3_player_t *player);
void mp3_player_set_loop(mp3_player_t *player, bool loop);
uint32_t mp3_player_get_sample_rate(const mp3_player_t *player);
void mp3_player_update(void);

// Diagnostics: output frames the mixer found no MP3 data for (heard as dropouts).
uint32_t mp3_player_staging_underruns(void);
void mp3_player_reset_staging_underruns(void);

// Starvation forensics snapshot/reset. out[11]:
// [0] underruns (as above) [1] updates [2] update mutex-skips [3] refill calls
// [4] refills finding ring empty [5] decode runs [6] frames decoded
// [7] mad errors [8] SD read fails [9] max update us [10] total update us (lo)
void mp3_player_get_diag(uint32_t out[11]);
void mp3_player_reset_diag(void);

// Fed mode: decoder reads from an external ring buffer instead of SD file.
// Used by video player to feed interleaved AVI audio data.
bool     mp3_player_start_fed(uint32_t sample_rate, uint16_t channels);
// Decodes what has been fed so far and starts the mixer pulling it (fading
// in). Call after feeding the first chunks, and again after a seek.
void     mp3_player_start_fed_output(void);
uint32_t mp3_player_feed(const uint8_t *data, uint32_t len);
void     mp3_player_stop_fed(void);
uint32_t mp3_player_feed_space(void);
bool     mp3_player_is_fed_mode(void);

// The mixer's MP3 source (audio_mix_render): adds `frames` frames of the
// playing MP3 into l and r. Runs in Core 1's DMA refill ISR on the device.
void mp3_player_mix(int32_t *l, int32_t *r, int frames);
