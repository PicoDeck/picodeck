#pragma once

#include <stdbool.h>
#include <stdint.h>

// The one audio mixer. The device's output (audio.c's DMA refill ISR, on
// Core 1) and the simulator's (simulator/sim_audio.c, its Core 1 thread)
// both render it. Every source adds into each AUDIO_OUT_RATE frame: the
// sample players (sound.c), the MP3 player (mp3_player.c), the PCM stream
// ring (fileplayer, MOD, the native/Lua stream API) and the square-wave
// tone; then the sum is clipped and scaled by the master volume. The
// source API is audio.h's; this file adds what the outputs (and the
// simulator's test channel) need.
// Host-tested in tests/unit/test_audio_mix.c.

// Once at boot, before anything plays (audio_init calls it).
void audio_mix_init(void);

// Renders `frames` frames of the mix into lr as interleaved stereo int16.
// Only ever called from the output's render context, never concurrently.
void audio_mix_render(int16_t *lr, int frames);

bool audio_tone_playing(void);
bool audio_stream_active(void);
uint8_t audio_get_volume(void);  // the master volume, 0-100
