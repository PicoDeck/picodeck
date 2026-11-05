// MOD music player — firmware-integrated, mirrors mp3_player.c pattern.
// Uses pocketmod (single-header MOD decoder) for decoding, and the
// audio_start_stream()/audio_push_samples() path for PCM output.
//
// Core 1 calls mod_player_update() every tick to render + push PCM.
//
// Locking: s_mod_mutex guards s_player, its MOD data and its pocketmod
// context. Core 0 (create/load/play/stop/destroy) takes it blocking and
// frees MOD data only after detaching it under the lock; Core 1's update
// try-locks it and skips the tick while Core 0 holds it, so Core 1 never
// waits on Core 0 and never renders freed data.

#include "mod_player.h"
#include "audio.h"
#include "sdcard.h"
#include "pico/mutex.h"
#include "pico/platform.h"

#include "umm_malloc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define POCKETMOD_IMPLEMENTATION
#include "pocketmod.h"

// Render at 22050 Hz — good enough for tracker music, halves CPU vs 44100
#define MOD_SAMPLE_RATE  22050

// Render buffer: 128 stereo frames per update tick (5ms @ 22050 = ~110 frames)
#define MOD_RENDER_FRAMES  128
#define MOD_RENDER_BUF_SIZE  (MOD_RENDER_FRAMES * 2 * sizeof(float))

struct mod_player {
    pocketmod_context ctx;
    uint8_t *mod_data;      // MOD file loaded into PSRAM (umm_malloc)
    uint32_t mod_size;
    bool playing;
    bool paused;
    bool loop;
    uint8_t volume;         // 0-100
};

// Heap-allocated to save ~4KB BSS (pocketmod_context is ~2.5KB alone)
static mod_player_t *s_player = NULL;
static atomic_bool s_initialized;
static mutex_t s_mod_mutex;

// Render buffers allocated in PSRAM alongside s_player
static float *s_render_buf = NULL;
static int16_t *s_pcm_buf = NULL;

bool mod_player_init(void) {
    if (atomic_load(&s_initialized)) return true;
    // Only init mutex here — PSRAM heap may not be ready yet.
    // Actual PSRAM allocations are deferred to mod_player_create().
    mutex_init(&s_mod_mutex);
    atomic_store(&s_initialized, true);  // published after the mutex
    return true;
}

// Allocate player + render buffers in PSRAM (called lazily from create)
static bool mod_player_alloc(void) {
    if (s_player) return true;

    mod_player_t *p = (mod_player_t *)umm_malloc(sizeof(mod_player_t));
    float *render = (float *)umm_malloc(MOD_RENDER_FRAMES * 2 * sizeof(float));
    int16_t *pcm = (int16_t *)umm_malloc(MOD_RENDER_FRAMES * 2 * sizeof(int16_t));
    if (!p || !render || !pcm) {
        umm_free(p);
        umm_free(render);
        umm_free(pcm);
        printf("[MOD] OOM allocating player\n");
        return false;
    }
    memset(p, 0, sizeof(mod_player_t));
    p->volume = 100;

    mutex_enter_blocking(&s_mod_mutex);
    s_player = p;
    s_render_buf = render;
    s_pcm_buf = pcm;
    mutex_exit(&s_mod_mutex);
    return true;
}

// Stop playback and detach the MOD data (s_mod_mutex held). Returns the
// data for the caller to free after unlocking.
static uint8_t *stop_and_detach_locked(mod_player_t *player) {
    if (player->playing) {
        player->playing = false;
        player->paused = false;
        audio_stop_stream();
    }
    uint8_t *data = player->mod_data;
    player->mod_data = NULL;
    player->mod_size = 0;
    return data;
}

static void stop_and_free_data(mod_player_t *player) {
    mutex_enter_blocking(&s_mod_mutex);
    uint8_t *data = stop_and_detach_locked(player);
    mutex_exit(&s_mod_mutex);
    umm_free(data);
}

void mod_player_deinit(void) {
    if (!atomic_load(&s_initialized)) return;
    mutex_enter_blocking(&s_mod_mutex);
    mod_player_t *p = s_player;
    uint8_t *data = p ? stop_and_detach_locked(p) : NULL;
    float *render = s_render_buf;
    int16_t *pcm = s_pcm_buf;
    s_player = NULL;
    s_render_buf = NULL;
    s_pcm_buf = NULL;
    mutex_exit(&s_mod_mutex);
    // The mutex stays initialised: Core 1 may still try-lock it.
    umm_free(data);
    umm_free(render);
    umm_free(pcm);
    umm_free(p);
}

void mod_player_reset(void) {
    if (!s_player) return;
    stop_and_free_data(s_player);
}

mod_player_t *mod_player_create(void) {
    if (!atomic_load(&s_initialized)) mod_player_init();
    if (!mod_player_alloc()) return NULL;
    // Single instance — only one MOD can play at a time
    mod_player_reset();
    s_player->volume = 100;
    s_player->loop = false;
    return s_player;
}

void mod_player_destroy(mod_player_t *player) {
    if (!player) return;
    stop_and_free_data(player);
}

bool mod_player_load(mod_player_t *player, const char *path) {
    if (!player || !path) return false;

    // Stop any current playback and free the previous MOD data
    stop_and_free_data(player);

    // Open and read the MOD file from SD card via sdcard API
    // (works on both real hardware and simulator)
    int fsize = sdcard_fsize(path);
    if (fsize <= 0 || fsize > 512 * 1024) {
        printf("[MOD] File missing, empty, or too large: %s (%d bytes)\n", path, fsize);
        return false;
    }

    sdfile_t fil = sdcard_fopen(path, "rb");
    if (!fil) {
        printf("[MOD] Failed to open: %s\n", path);
        return false;
    }

    // Allocate in PSRAM
    uint8_t *data = (uint8_t *)umm_malloc((uint32_t)fsize);
    if (!data) {
        sdcard_fclose(fil);
        printf("[MOD] OOM allocating %d bytes\n", fsize);
        return false;
    }

    int bytes_read = sdcard_fread(fil, data, fsize);
    sdcard_fclose(fil);

    if (bytes_read != fsize) {
        umm_free(data);
        printf("[MOD] Read error: read=%d expected=%d\n", bytes_read, fsize);
        return false;
    }

    // Validate the MOD file by trying to init pocketmod, then publish it
    // (the player is stopped, so Core 1 is not rendering this context).
    mutex_enter_blocking(&s_mod_mutex);
    bool ok = pocketmod_init(&player->ctx, data, fsize, MOD_SAMPLE_RATE);
    if (ok) {
        player->mod_data = data;
        player->mod_size = (uint32_t)fsize;
    }
    mutex_exit(&s_mod_mutex);
    if (!ok) {
        umm_free(data);
        printf("[MOD] Invalid MOD file: %s\n", path);
        return false;
    }

    printf("[MOD] Loaded: %s (%u bytes)\n", path, fsize);
    return true;
}

void mod_player_play(mod_player_t *player, bool loop) {
    if (!player || !atomic_load(&s_initialized)) return;

    mutex_enter_blocking(&s_mod_mutex);
    if (!player->mod_data) {
        mutex_exit(&s_mod_mutex);
        return;
    }

    player->loop = loop;
    player->paused = false;

    // Re-init pocketmod from the start
    pocketmod_init(&player->ctx, player->mod_data, (int)player->mod_size, MOD_SAMPLE_RATE);

    // Start audio output stream
    audio_start_stream(MOD_SAMPLE_RATE);

    player->playing = true;

    mutex_exit(&s_mod_mutex);
    printf("[MOD] Playing (loop=%d)\n", loop);
}

void mod_player_stop(mod_player_t *player) {
    if (!player || !atomic_load(&s_initialized)) return;

    mutex_enter_blocking(&s_mod_mutex);

    if (player->playing) {
        player->playing = false;
        player->paused = false;
        audio_stop_stream();
    }

    mutex_exit(&s_mod_mutex);
}

void mod_player_pause(mod_player_t *player) {
    if (!player || !player->playing) return;
    player->paused = true;
}

void mod_player_resume(mod_player_t *player) {
    if (!player || !player->playing) return;
    player->paused = false;
}

bool mod_player_is_playing(const mod_player_t *player) {
    return player && player->playing && !player->paused;
}

void mod_player_set_volume(mod_player_t *player, uint8_t volume) {
    if (!player) return;
    if (volume > 100) volume = 100;
    player->volume = volume;
}

uint8_t mod_player_get_volume(const mod_player_t *player) {
    return player ? player->volume : 0;
}

void mod_player_set_loop(mod_player_t *player, bool loop) {
    if (!player) return;
    player->loop = loop;
}

// Core 1's tick gate. The player is in PSRAM, so this does not look inside it:
// an app that made one and is not playing it costs the walk it always did.
bool __time_critical_func(mod_player_core1_idle)(void) {
    return s_player == NULL;
}

// Called from Core 1 every ~1ms
void mod_player_update(void) {
    if (!atomic_load(&s_initialized)) return;
    if (!mutex_try_enter(&s_mod_mutex, NULL))
        return;  // Core 0 is loading/stopping: next tick

    mod_player_t *p = s_player;  // alloc is deferred to mod_player_create()
    if (!p || !p->playing || p->paused || !p->mod_data)
        goto out;

    // Flow control: only render when ring buffer has room.
    // Without this, pocketmod advances its clock but samples are dropped
    // by audio_push_samples() when the ring is full — causing severe corruption.
    uint32_t free = audio_ring_free();
    if (free < MOD_RENDER_FRAMES)
        goto out;

    // Render a chunk of float stereo PCM via pocketmod
    int bytes_rendered = pocketmod_render(&p->ctx, s_render_buf,
                                           (int)MOD_RENDER_BUF_SIZE);
    int frames = bytes_rendered / (int)(2 * sizeof(float));

    if (frames <= 0) {
        // Song finished
        if (p->loop) {
            // Re-init to restart from beginning (renders next tick)
            pocketmod_init(&p->ctx, p->mod_data, (int)p->mod_size, MOD_SAMPLE_RATE);
            goto out;
        }
        p->playing = false;
        audio_stop_stream();
        goto out;
    }

    // Check if song has looped (pocketmod tracks this internally)
    if (!p->loop && pocketmod_loop_count(&p->ctx) > 0) {
        p->playing = false;
        audio_stop_stream();
        goto out;
    }

    // Convert float [-1,1] stereo to int16_t stereo with volume scaling.
    // pocketmod normalizes output across channels (4-ch MOD = ±0.25 peak),
    // so apply 2x gain to bring levels into audible range without clipping.
    uint32_t vol = (uint32_t)p->volume * 256 / 100;  // 0..256
    for (int i = 0; i < frames * 2; i++) {
        float sample = s_render_buf[i];
        int32_t pcm = (int32_t)(sample * (32767.0f * 3.0f));
        pcm = (pcm * (int32_t)vol) >> 8;
        if (pcm > 32767) pcm = 32767;
        if (pcm < -32768) pcm = -32768;
        s_pcm_buf[i] = (int16_t)pcm;
    }

    // Push to audio output ring buffer
    audio_push_samples(s_pcm_buf, frames);

    // Periodic debug: log DMA stats every ~2 seconds
    static uint32_t s_debug_counter = 0;
    if (++s_debug_counter >= 400) {
        s_debug_counter = 0;
        uint32_t isr_cnt, underruns, ring_used;
        audio_stream_debug(&isr_cnt, &underruns, &ring_used);
        uint32_t ring_free = audio_ring_free();
        printf("[MOD] DMA ISR=%lu underruns=%lu ring=%lu free=%lu\n",
               (unsigned long)isr_cnt, (unsigned long)underruns,
               (unsigned long)ring_used, (unsigned long)ring_free);
    }
out:
    mutex_exit(&s_mod_mutex);
}
