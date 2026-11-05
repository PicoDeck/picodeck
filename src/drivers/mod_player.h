#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct mod_player mod_player_t;

bool mod_player_init(void);
void mod_player_deinit(void);
void mod_player_reset(void);

mod_player_t *mod_player_create(void);
void mod_player_destroy(mod_player_t *player);
bool mod_player_load(mod_player_t *player, const char *path);
void mod_player_play(mod_player_t *player, bool loop);
void mod_player_stop(mod_player_t *player);
void mod_player_pause(mod_player_t *player);
void mod_player_resume(mod_player_t *player);
bool mod_player_is_playing(const mod_player_t *player);
void mod_player_set_volume(mod_player_t *player, uint8_t volume);
uint8_t mod_player_get_volume(const mod_player_t *player);
void mod_player_set_loop(mod_player_t *player, bool loop);

// Called from Core 1 every 5ms alongside mp3_player_update()
void mod_player_update(void);
bool mod_player_core1_idle(void);  // false once a player exists (runs from RAM)
