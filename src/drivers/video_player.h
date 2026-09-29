#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The frame index is sized from the AVI header's frame count, capped here
// (8 bytes per entry).  Beyond the cap playback falls back to sequential
// chunk scanning.  The audio index covers the same stretch of the file and
// grows to hold every audio chunk in it (avi_index.h).
#define VIDEO_MAX_FRAME_INDEX 65536

#define VIDEO_BUFFER_POOL_SIZE 3
#define VIDEO_MAX_JPEG_SIZE (96 * 1024)

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t frame_count;
    uint32_t current_frame;

    bool playing;
    bool paused;
    bool loop;
    bool auto_flush;
    bool ended;          // reached the last frame with loop off (playing=false)

    uint16_t y_offset;
    uint16_t visible_height;

    void *priv;
} video_player_t;

bool video_player_init(void);
video_player_t *video_player_create(void);
void video_player_destroy(video_player_t *player);

bool video_player_load(video_player_t *player, const char *path);
void video_player_play(video_player_t *player);
void video_player_stop(video_player_t *player);
void video_player_pause(video_player_t *player);
void video_player_resume(video_player_t *player);

bool video_player_update(video_player_t *player);
// Seeks clamp to [0, frame_count-1] and never wrap.  A seek while paused
// decodes and presents the target frame immediately; a seek on an ended
// player restarts playback from the target.
void video_player_seek(video_player_t *player, uint32_t frame);
void video_player_seek_ms(video_player_t *player, uint32_t ms);
void video_player_seek_relative_ms(video_player_t *player, int32_t delta_ms);
float video_player_get_fps(video_player_t *player);
uint32_t video_player_get_frame_count(video_player_t *player);
uint32_t video_player_get_duration_ms(video_player_t *player);
uint32_t video_player_get_position_ms(video_player_t *player);
bool video_player_has_ended(video_player_t *player);

// On-screen display: progress bar + elapsed/total time drawn over the bottom
// of the video area.  Shown automatically on play/pause/seek; while playing it
// hides after the timeout (default 3000 ms), while paused or ended it stays.
void video_player_set_osd(video_player_t *player, bool enabled);
void video_player_show_osd(video_player_t *player);
void video_player_set_osd_timeout(video_player_t *player, uint32_t ms);

uint32_t video_player_get_dropped_frames(video_player_t *player);
void video_player_reset_stats(video_player_t *player);

// Called on Core 1 every tick: services pending next-frame SD prefetch
// requests so the read overlaps with Core 0's JPEG decode.
void video_prefetch_update(void);

bool video_player_has_audio(video_player_t *player);
void video_player_set_audio_volume(video_player_t *player, uint8_t volume);
uint8_t video_player_get_audio_volume(video_player_t *player);
void video_player_set_audio_muted(video_player_t *player, bool muted);
bool video_player_get_audio_muted(video_player_t *player);

#ifdef __cplusplus
}
#endif
