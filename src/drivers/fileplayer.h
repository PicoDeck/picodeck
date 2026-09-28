#pragma once

#include <stdint.h>
#include <stdbool.h>

#define FILEPLAYER_BUFFER_SIZE 8192
#define FILEPLAYER_MAX_INSTANCES 2

typedef enum {
    FILEPLAYER_STATE_IDLE = 0,
    FILEPLAYER_STATE_PLAYING,
    FILEPLAYER_STATE_PAUSED,
    FILEPLAYER_STATE_STOPPED
} fileplayer_state_t;

typedef enum {
    FILEPLAYER_TYPE_UNKNOWN = 0,
    FILEPLAYER_TYPE_WAV,
    FILEPLAYER_TYPE_MP3
} fileplayer_type_t;

typedef struct {
    char path[256];
    fileplayer_state_t state;
    fileplayer_type_t type;
    uint32_t position;      // bytes of the data chunk consumed
    uint32_t length;        // frames
    uint8_t volume;         // left, 0-100
    uint8_t volume_r;       // right, 0-100
    uint8_t channels;
    bool in_use;            // slot handed out by fileplayer_create
    // The loaded WAV, owned by this player (fileplayer.c's player lock
    // guards it: Core 1 reads it, Core 0 opens and closes it).
    void *file;             // sdfile_t
    uint32_t sample_rate;
    uint32_t data_offset;   // file offset of the data chunk
    uint32_t data_size;     // bytes of sample data
    uint16_t block_align;   // bytes per frame
    bool loop;
    uint32_t loop_start;
    uint32_t loop_end;
    int (*finish_callback)(void *);
    void *finish_callback_arg;
    int (*loop_callback)(void *);
    void *loop_callback_arg;
    bool stop_on_underrun;
    float rate;
    uint8_t repeats;        // plays asked for by play(); 0 = until stopped
    uint8_t plays;          // plays finished since play()
} fileplayer_t;

void fileplayer_init(void);
void fileplayer_reset(void);
fileplayer_t *fileplayer_create(void);
void fileplayer_destroy(fileplayer_t *player);
bool fileplayer_load(fileplayer_t *player, const char *path);
bool fileplayer_play(fileplayer_t *player, uint8_t repeat_count);
void fileplayer_stop(fileplayer_t *player);
void fileplayer_pause(fileplayer_t *player);
void fileplayer_resume(fileplayer_t *player);
bool fileplayer_is_playing(const fileplayer_t *player);
uint32_t fileplayer_get_position(const fileplayer_t *player);
uint32_t fileplayer_get_length(const fileplayer_t *player);
void fileplayer_set_volume(fileplayer_t *player, uint8_t left, uint8_t right);
void fileplayer_get_volume(const fileplayer_t *player, uint8_t *left, uint8_t *right);
void fileplayer_set_loop_range(fileplayer_t *player, uint32_t start, uint32_t end);
void fileplayer_set_finish_callback(fileplayer_t *player, int (*cb)(void *), void *arg);
void fileplayer_set_loop_callback(fileplayer_t *player, int (*cb)(void *), void *arg);
void fileplayer_set_offset(fileplayer_t *player, uint32_t seconds);
uint32_t fileplayer_get_offset(const fileplayer_t *player);

void fileplayer_set_stop_on_underrun(fileplayer_t *player, bool flag);
void fileplayer_set_rate(fileplayer_t *player, float rate);
float fileplayer_get_rate(const fileplayer_t *player);

void fileplayer_update(void);
bool fileplayer_did_underrun(void);
