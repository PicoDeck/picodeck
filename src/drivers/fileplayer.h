#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "qoa.h"

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
    FILEPLAYER_TYPE_MP3,
    FILEPLAYER_TYPE_QOA
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
    bool loop;              // setLoopRange() was called: loop until stopped
    uint32_t loop_start;    // seconds into the data
    uint32_t loop_end;      // seconds; 0 = the end of the data
    int (*finish_callback)(void *);
    void *finish_callback_arg;
    int (*loop_callback)(void *);
    void *loop_callback_arg;
    bool stop_on_underrun;
    // Underrun tracking (Core 1 under s_lock): the stream's underrun count
    // as of the last tick, and whether it is trusted yet (the ring is
    // legitimately empty until the first push after play() or resume()).
    uint32_t under_base;
    bool under_armed;
    bool underran;          // sticky until didUnderrun() reads it or play()
    float rate;
    uint8_t repeats;        // plays asked for by play(); 0 = until stopped
    uint8_t plays;          // plays finished since play()
    bool pass_pushed;       // this pass through the data pushed audio
    // QOA (type == FILEPLAYER_TYPE_QOA).  position/data_size stay virtual
    // 16-bit PCM bytes, so position, offset and flow control are shared
    // with WAV; these track the compressed side.  The frame being decoded
    // sits in fileplayer.c's shared s_wav_buffer (one player streams at a
    // time), qoa_dec holds its decoder state between ticks.
    qoa_info_t qoa;
    qoa_dec_t qoa_dec;
    bool qoa_loaded;          // qoa_dec is part way through a frame
    uint32_t qoa_file_pos;    // file offset of the next frame to read
    uint32_t qoa_skip;        // frames of the next frame before the position
                              // (a seek lands mid-frame): decoded, dropped
} fileplayer_t;

void fileplayer_init(void);
void fileplayer_reset(void);
fileplayer_t *fileplayer_create(void);
void fileplayer_destroy(fileplayer_t *player);
bool fileplayer_load(fileplayer_t *player, const char *path);
// As fileplayer_load; on failure *reason (if not NULL) points at a static
// string saying why the file cannot be played.
bool fileplayer_load_err(fileplayer_t *player, const char *path,
                         const char **reason);
uint32_t fileplayer_get_sample_rate(const fileplayer_t *player);
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
// True if the stream starved while this player was playing since the last
// call (or play()); reading clears it.
bool fileplayer_did_underrun(fileplayer_t *player);
