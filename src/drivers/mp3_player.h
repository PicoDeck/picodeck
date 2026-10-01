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

// When the decoder decoded (mp3_sched.h, issue #28): frames decoded because
// the PCM ring was low, frames decoded ahead in Core 0's idle windows (and
// of those, how many ran past the window's end), the estimate of one
// frame's uncontended decode time, and whether decoding ahead is on.
typedef struct {
    uint32_t low_frames, idle_frames, overran, frame_us;
    bool decode_ahead;
} mp3_sched_stats_t;
void mp3_player_get_sched_stats(mp3_sched_stats_t *out);
void mp3_player_reset_sched_stats(void);
// On by default; off decodes as before (only when the ring is low), for an
// A/B measurement (the `xipstat mp3idle` dev command).
void mp3_player_set_decode_ahead(bool on);

// Fed mode: decoder reads from an external ring buffer instead of SD file.
// Used by video player to feed interleaved AVI audio data. Calling
// start_fed on a running session restarts it (fade out, empty the rings,
// reset the decoder and the volume to 100) and keeps its ring allocated.
bool     mp3_player_start_fed(uint32_t sample_rate, uint16_t channels);
// Decodes what has been fed so far and starts the mixer pulling it (fading
// in). Call after feeding the first chunks, and again after a seek.
void     mp3_player_start_fed_output(void);
// Feeding clears an end marked by mp3_player_fed_end.
uint32_t mp3_player_feed(const uint8_t *data, uint32_t len);
// Nothing more will be fed (the end of the audio): the decoder decodes the
// last frame, then the stream finishes as a file's does. Until then it
// decodes a frame only once the next one's header is fed, so the last frame
// fed waits for more. Core 0.
void     mp3_player_fed_end(void);
void     mp3_player_stop_fed(void);
uint32_t mp3_player_feed_space(void);
bool     mp3_player_is_fed_mode(void);
// The loop point of a looping video's audio, which the video player feeds
// on from its first chunk again after the last so that the decoder plays
// straight through it. Call it just before feeding the next pass's first
// chunk; one mark at a time (start_fed and stop_fed clear it). Core 0.
void     mp3_player_fed_mark(void);
// Once the decoder has decoded past the mark: true, with *frames the
// content frames the mixer has played beyond it (negative: still to play
// before it), and the mark cleared. False, *frames untouched, while it is
// still ahead of the decoder or none is set. Core 0, lock-free.
bool     mp3_player_fed_mark_reached(int32_t *frames);

// The mixer's MP3 source (audio_mix_render): adds `frames` frames of the
// playing MP3 into l and r. Runs in Core 1's DMA refill ISR on the device.
void mp3_player_mix(int32_t *l, int32_t *r, int frames);
