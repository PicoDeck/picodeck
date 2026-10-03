#include "lua_bridge_internal.h"
#include "../drivers/sound.h"
#include "../drivers/fileplayer.h"
#include "../drivers/mp3_player.h"
#include "../drivers/sdcard.h"
// Note: sound.h, fileplayer.h, mp3_player.h are kept for functions not in
// g_api.soundplayer (e.g. sound_sample_new_blank, sound_player_set_rate,
// fileplayer_set_finish_callback, mp3_player_get_position, init/reset calls).

#define SAMPLE_USERDATA "sound_sample"
#define PLAYER_USERDATA "sound_player"
#define FILEPLAYER_USERDATA "fileplayer"
#define MP3PLAYER_USERDATA "mp3player"

// ── Sample / sampleplayer lifetimes ─────────────────────────────────────────
// A sample userdata owns its sound_sample_t (its __gc frees it). A player
// userdata keeps the sample it plays in user value PLAYER_UV_SAMPLE, so the
// collector cannot free a sample a player still points at; a path-built
// player (sampleplayer(path), sample:play()) gets a sample userdata of its
// own there too. When a player and its sample die in the same cycle the
// sample's finalizer may run first: sound_sample_destroy detaches the player
// (under the mixer lock) before the data goes. Both userdata hold NULL once
// freed and are rejected from then on.
#define PLAYER_UV_SAMPLE 1

static sound_sample_t *check_sample(lua_State *L, int idx) {
    sound_sample_t **ud = lb_checkudata(L, idx, SAMPLE_USERDATA);
    if (!*ud)
        luaL_argerror(L, idx, "sample has been freed");
    return *ud;
}

static sound_player_t *check_player(lua_State *L, int idx) {
    sound_player_t **ud = lb_checkudata(L, idx, PLAYER_USERDATA);
    if (!*ud)
        luaL_argerror(L, idx, "sampleplayer has been freed");
    return *ud;
}

// Pushes an empty sample userdata; the caller stores the sample in *ud, so a
// failed load leaves nothing for __gc to free.
static sound_sample_t **push_sample_ud(lua_State *L) {
    sound_sample_t **ud = lua_newuserdatauv(L, sizeof(*ud), 0);
    *ud = NULL;
    luaL_setmetatable(L, SAMPLE_USERDATA);
    return ud;
}

// Pushes an empty player userdata with its sample user value.
static sound_player_t **push_player_ud(lua_State *L) {
    sound_player_t **ud = lua_newuserdatauv(L, sizeof(*ud), 1);
    *ud = NULL;
    luaL_setmetatable(L, PLAYER_USERDATA);
    return ud;
}

// Anchors the sample userdata at sample_idx on the player at player_idx.
static void anchor_sample(lua_State *L, int player_idx, int sample_idx) {
    player_idx = lua_absindex(L, player_idx);
    lua_pushvalue(L, sample_idx);
    lua_setiuservalue(L, player_idx, PLAYER_UV_SAMPLE);
}

// __gc NULLs the pointer (the slot may already belong to another player),
// and a later finaliser can still reach the handle: reject it.
static fileplayer_t *check_fileplayer(lua_State *L, int idx) {
    fileplayer_t **ud = lb_checkudata(L, idx, FILEPLAYER_USERDATA);
    if (!*ud)
        luaL_error(L, "attempt to use a destroyed fileplayer");
    return *ud;
}

static mp3_player_t *check_mp3player(lua_State *L, int idx) {
    mp3_player_t **ud = lb_checkudata(L, idx, MP3PLAYER_USERDATA);
    if (!*ud)
        luaL_error(L, "attempt to use a destroyed mp3player");
    return *ud;
}

static int l_sound_sample_new(lua_State *L) {
    if (lua_isnumber(L, 1)) {
        float seconds = (float)lua_tonumber(L, 1);
        uint32_t sample_rate = 44100;
        uint8_t bits = 16;
        uint8_t channels = 1;

        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "sampleRate");
            if (!lua_isnil(L, -1)) sample_rate = (uint32_t)lb_clamp_int(
                lb_checkint_at(L, -1, 2, "field 'sampleRate'"), 0, LUA_MAXINTEGER);
            lua_pop(L, 1);

            lua_getfield(L, 2, "bits");
            if (!lua_isnil(L, -1)) bits = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 2, "channels");
            if (!lua_isnil(L, -1)) channels = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }

        sound_sample_t **ud = push_sample_ud(L);
        *ud = sound_sample_new_blank(seconds, sample_rate, bits, channels);
        if (!*ud) {
            lua_pushnil(L);
            lua_pushstring(L, "failed to allocate sample buffer");
            return 2;
        }
        return 1;
    }

    const char *path = luaL_optstring(L, 1, NULL);
    if (path) {
        if (!fs_sandbox_check(L, path, false)) {
            lua_pushnil(L);
            lua_pushstring(L, "access denied");
            return 2;
        }
        sound_sample_t **ud = push_sample_ud(L);
        *ud = (sound_sample_t *)g_api.soundplayer->sampleLoad(path);
        if (!*ud) {
            lua_pushnil(L);
            lua_pushstring(L, "failed to load sample");
            return 2;
        }
        return 1;
    }

    // No path: create an empty (unloaded) sample
    sound_sample_t **ud = push_sample_ud(L);
    *ud = sound_sample_create();
    if (!*ud) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create sample");
        return 2;
    }
    return 1;
}

static int l_sound_sample_load(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    const char *path = luaL_checkstring(L, 2);

    if (!fs_sandbox_check(L, path, false)) {
        lua_pushnil(L);
        lua_pushstring(L, "access denied");
        return 2;
    }
    if (sound_sample_load(sample, path)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "failed to load WAV file");
    return 2;
}

static int l_sound_sample_getLength(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    lua_pushinteger(L, sound_sample_get_length(sample));
    return 1;
}

static int l_sound_sample_getSampleRate(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    lua_pushinteger(L, sound_sample_get_sample_rate(sample));
    return 1;
}

static int l_sound_sample_getFormat(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    lua_newtable(L);
    lua_pushinteger(L, sample->bits_per_sample);
    lua_setfield(L, -2, "bits");
    lua_pushinteger(L, sample->channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, sample->sample_rate);
    lua_setfield(L, -2, "sampleRate");
    return 1;
}

static int l_sound_sample_decompress(lua_State *L) {
    check_sample(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

static int l_sound_sample_getSubsample(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    uint32_t start = (uint32_t)lb_clamp_int(lb_checkint(L, 2), 0, LUA_MAXINTEGER);
    uint32_t end = (uint32_t)lb_clamp_int(lb_checkint(L, 3), 0, LUA_MAXINTEGER);

    sound_sample_t **ud = push_sample_ud(L);
    *ud = sound_sample_get_subsample(sample, start, end);
    if (!*ud) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create subsample");
        return 2;
    }
    return 1;
}

// sample:play([repeatCount], [rate]) — convenience: create temp player, play, return it
static int l_sound_sample_play(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    uint8_t repeat = (uint8_t)lb_clamp_int(lb_optint(L, 2, 1), 0, 255);
    float rate = (float)luaL_optnumber(L, 3, 1.0);

    sound_player_t **ud = push_player_ud(L);
    sound_player_t *player = (sound_player_t *)g_api.soundplayer->playerNew();
    if (!player) return luaL_error(L, "failed to create player");
    *ud = player;
    anchor_sample(L, -1, 1);

    g_api.soundplayer->playerSetSample(player, sample);
    sound_player_set_rate(player, rate);
    g_api.soundplayer->playerPlay(player, repeat);
    return 1;
}

// sample:playAt(when, [vol], [rightvol], [rate])
static int l_sound_sample_playAt(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    (void)luaL_checknumber(L, 2); // 'when' — ignored on this hardware (no scheduler)
    uint8_t vol = (uint8_t)lb_clamp_int(lb_optint(L, 3, 100), 0, 100);
    (void)lb_optint(L, 4, vol); // rightvol — mono PWM, use left
    float rate = (float)luaL_optnumber(L, 5, 1.0);

    sound_player_t **ud = push_player_ud(L);
    sound_player_t *player = (sound_player_t *)g_api.soundplayer->playerNew();
    if (!player) return luaL_error(L, "failed to create player");
    *ud = player;
    anchor_sample(L, -1, 1);

    g_api.soundplayer->playerSetSample(player, sample);
    g_api.soundplayer->playerSetVolume(player, vol);
    sound_player_set_rate(player, rate);
    g_api.soundplayer->playerPlay(player, 1);
    return 1;
}

// sample:save(filename) — write WAV file to SD card
static int l_sound_sample_save(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    const char *path = luaL_checkstring(L, 2);

    if (!sample->loaded || !sample->data || sample->length == 0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "sample has no data");
        return 2;
    }
    if (!fs_sandbox_check(L, path, true)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "access denied");
        return 2;
    }

    sdfile_t f = sdcard_fopen(path, "w");
    if (!f) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "failed to open file for writing");
        return 2;
    }

    // Build 44-byte WAV header
    uint32_t data_size = sample->length;
    uint32_t file_size = 36 + data_size;
    uint16_t channels = sample->channels;
    uint32_t sr = sample->sample_rate;
    uint16_t bps = sample->bits_per_sample;
    uint16_t block_align = channels * (bps / 8);
    uint32_t byte_rate = sr * block_align;

    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &file_size, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16;
    memcpy(hdr + 16, &fmt_size, 4);
    uint16_t audio_fmt = 1; // PCM
    memcpy(hdr + 20, &audio_fmt, 2);
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &sr, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_size, 4);

    int wrote = sdcard_fwrite(f, hdr, 44);
    if (wrote < 44) {
        sdcard_fclose(f);
        lua_pushboolean(L, false);
        lua_pushstring(L, "failed to write WAV header");
        return 2;
    }
    wrote = sdcard_fwrite(f, sample->data, data_size);
    sdcard_fclose(f);

    if (wrote < (int)data_size) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "failed to write sample data");
        return 2;
    }

    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_sample_gc(lua_State *L) {
    sound_sample_t **ud = lb_checkudata(L, 1, SAMPLE_USERDATA);
    if (*ud) {
        g_api.soundplayer->sampleFree(*ud);   // detaches any player still using it
        *ud = NULL;
    }
    return 0;
}

static int l_sound_sampleplayer_new(lua_State *L) {
    sound_sample_t *sample = NULL;
    int sample_idx = 0;   // stack index of the sample userdata to anchor

    if (lua_isuserdata(L, 1)) {
        sample = check_sample(L, 1);
        sample_idx = 1;
    } else if (lua_isstring(L, 1)) {
        if (!fs_sandbox_check(L, lua_tostring(L, 1), false)) {
            lua_pushnil(L);
            lua_pushstring(L, "access denied");
            return 2;
        }
        // The path-loaded sample is a sample userdata of its own, anchored on
        // the player like any other: the collector frees it with (or after)
        // the player, and a later setSample lets it go.
        sound_sample_t **sud = push_sample_ud(L);
        *sud = (sound_sample_t *)g_api.soundplayer->sampleLoad(lua_tostring(L, 1));
        if (!*sud) {
            lua_pushnil(L);
            lua_pushstring(L, "failed to load sample");
            return 2;
        }
        sample = *sud;
        sample_idx = lua_gettop(L);
    }

    sound_player_t **ud = push_player_ud(L);
    *ud = (sound_player_t *)g_api.soundplayer->playerNew();
    if (!*ud) {
        lua_pushnil(L);
        lua_pushfstring(L, "at most %d sampleplayers can exist at once",
                        SOUND_MAX_PLAYERS);
        return 2;
    }
    if (sample) {
        anchor_sample(L, -1, sample_idx);
        g_api.soundplayer->playerSetSample(*ud, sample);
    }
    return 1;
}

static int l_sound_sampleplayer_setSample(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    sound_sample_t *sample = check_sample(L, 2);
    // The previous sample (if any) loses its anchor here and is collected
    // once nothing else refers to it.
    anchor_sample(L, 1, 2);
    g_api.soundplayer->playerSetSample(player, sample);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_sampleplayer_play(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    uint8_t repeat = (uint8_t)lb_clamp_int(lb_optint(L, 2, 1), 0, 255);
    g_api.soundplayer->playerPlay(player, repeat);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_sampleplayer_stop(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    g_api.soundplayer->playerStop(player);
    return 0;
}

static int l_sound_sampleplayer_isPlaying(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_pushboolean(L, g_api.soundplayer->playerIsPlaying(player));
    return 1;
}

static int l_sound_sampleplayer_setVolume(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    uint8_t vol = (uint8_t)lb_clamp_int(lb_checkint(L, 2), 0, 100);
    g_api.soundplayer->playerSetVolume(player, vol);
    return 0;
}

static int l_sound_sampleplayer_getVolume(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_pushinteger(L, g_api.soundplayer->playerGetVolume(player));
    return 1;
}

static int l_sound_sampleplayer_setPaused(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    player->paused = lua_toboolean(L, 2);
    return 0;
}

static int l_sound_sampleplayer_getLength(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    if (player->sample) {
        lua_pushinteger(L, sound_sample_get_length(player->sample));
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

static int l_sound_sampleplayer_setOffset(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_Number seconds = luaL_checknumber(L, 2);
    if (player->sample && player->sample->loaded) {
        uint32_t bytes_per_sample = player->sample->bits_per_sample / 8;
        uint32_t bytes_per_frame = bytes_per_sample * player->sample->channels;
        uint32_t frame = (uint32_t)(seconds * player->sample->sample_rate);
        uint32_t byte_offset = frame * bytes_per_frame;
        if (byte_offset > player->sample->length)
            byte_offset = player->sample->length;
        player->position = byte_offset;
    }
    return 0;
}

static int l_sound_sampleplayer_getOffset(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    if (player->sample && player->sample->loaded && player->sample->sample_rate > 0) {
        uint32_t bytes_per_sample = player->sample->bits_per_sample / 8;
        uint32_t bytes_per_frame = bytes_per_sample * player->sample->channels;
        lua_Number seconds = (lua_Number)(player->position / bytes_per_frame)
                             / (lua_Number)player->sample->sample_rate;
        lua_pushnumber(L, seconds);
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// Returns the sample userdata the player plays (the anchored one), or nil.
static int l_sound_sampleplayer_getSample(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    if (!player->sample) {
        lua_pushnil(L);
        return 1;
    }
    lua_getiuservalue(L, 1, PLAYER_UV_SAMPLE);
    return 1;
}

static int l_sound_sampleplayer_setPlayRange(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    uint32_t start = (uint32_t)lb_clamp_int(lb_checkint(L, 2), 0, LUA_MAXINTEGER);
    uint32_t end = (uint32_t)lb_clamp_int(lb_checkint(L, 3), 0, LUA_MAXINTEGER);
    sound_player_set_play_range(player, start, end);
    return 0;
}

static int l_sound_sampleplayer_setRate(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    float rate = (float)luaL_checknumber(L, 2);
    sound_player_set_rate(player, rate);
    return 0;
}

static int l_sound_sampleplayer_getRate(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_pushnumber(L, sound_player_get_rate(player));
    return 1;
}

static void release_player_cbs(lua_State *L, sound_player_t *player);

static int l_sound_sampleplayer_gc(lua_State *L) {
    sound_player_t **ud = lb_checkudata(L, 1, PLAYER_USERDATA);
    if (*ud) {
        // Free first (detaches the player's callbacks under the mixer lock,
        // so Core 1 stops firing them), then give the slots back: releasing
        // first left a window for a callback into a slot being reused.
        sound_player_t detached = **ud;  // the callback args playerFree clears
        g_api.soundplayer->playerFree(*ud);
        release_player_cbs(L, &detached);
        *ud = NULL;
    }
    return 0;
}

static int l_sound_fileplayer_new(lua_State *L) {
    size_t buffer_size = luaL_optinteger(L, 1, 8192);
    (void)buffer_size;

    fileplayer_t *player = (fileplayer_t *)g_api.soundplayer->filePlayerNew();
    if (!player) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create fileplayer");
        return 2;
    }

    fileplayer_t **ud = lua_newuserdata(L, sizeof(fileplayer_t *));
    *ud = player;
    luaL_setmetatable(L, FILEPLAYER_USERDATA);
    return 1;
}

static int l_sound_fileplayer_load(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    const char *path = luaL_checkstring(L, 2);
    if (!fs_sandbox_check(L, path, false)) {
        lua_pushnil(L);
        lua_pushstring(L, "access denied");
        return 2;
    }
    // Not through the void native filePlayerLoad: Lua gets the reason.
    const char *reason = NULL;
    if (!fileplayer_load_err(player, path, &reason)) {
        lua_pushnil(L);
        lua_pushstring(L, reason ? reason : "cannot play file");
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_fileplayer_play(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t repeat = (uint8_t)lb_clamp_int(lb_optint(L, 2, 1), 0, 255);
    g_api.soundplayer->filePlayerPlay(player, repeat);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_fileplayer_stop(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    g_api.soundplayer->filePlayerStop(player);
    return 0;
}

static int l_sound_fileplayer_pause(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    g_api.soundplayer->filePlayerPause(player);
    return 0;
}

static int l_sound_fileplayer_isPlaying(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushboolean(L, g_api.soundplayer->filePlayerIsPlaying(player));
    return 1;
}

static int l_sound_fileplayer_getLength(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushinteger(L, fileplayer_get_length(player));
    return 1;
}

static int l_sound_fileplayer_getSampleRate(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushinteger(L, (lua_Integer)fileplayer_get_sample_rate(player));
    return 1;
}

static int l_sound_fileplayer_getOffset(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushinteger(L, g_api.soundplayer->filePlayerGetOffset(player));
    return 1;
}

static int l_sound_fileplayer_setOffset(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint32_t seconds = (uint32_t)lb_clamp_int(lb_checkint(L, 2), 0, LUA_MAXINTEGER);
    g_api.soundplayer->filePlayerSetOffset(player, seconds);
    return 0;
}

static int l_sound_fileplayer_setVolume(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t left = (uint8_t)lb_clamp_int(lb_checkint(L, 2), 0, 100);
    uint8_t right = (uint8_t)lb_clamp_int(lb_optint(L, 3, left), 0, 100);
    // API takes a single volume value; use left channel value (sets both channels)
    (void)right;
    g_api.soundplayer->filePlayerSetVolume(player, left);
    return 0;
}

static int l_sound_fileplayer_getVolume(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t vol = g_api.soundplayer->filePlayerGetVolume(player);
    lua_pushinteger(L, vol);
    lua_pushinteger(L, vol);
    return 2;
}

static int l_sound_fileplayer_setLoopRange(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint32_t start = (uint32_t)lb_clamp_int(lb_optint(L, 2, 0), 0, LUA_MAXINTEGER);
    uint32_t end = (uint32_t)lb_clamp_int(lb_optint(L, 3, 0), 0, LUA_MAXINTEGER);
    fileplayer_set_loop_range(player, start, end);
    return 0;
}

static int l_sound_fileplayer_resume(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    g_api.soundplayer->filePlayerResume(player);
    return 0;
}

// ── Cross-core callback system ──────────────────────────────────────────────
// Audio callbacks fire on Core 1 (sound_update / fileplayer_update).  Lua runs
// on Core 0.  The trampoline sets a volatile pending flag (and requests a
// service pass); Core 0 polls it in lua_bridge_sound_poll(), run by the
// service pass (count hook, sys.sleep, terminal waits, input.update), and
// fires the Lua function reference.

typedef struct {
    lua_State *L;
    int ref;
    volatile uint8_t pending;
} sound_lua_cb_t;

// Callback slots: fileplayer finish/loop, sampleplayer finish/loop
#define MAX_FILEPLAYER_CBS 2
static sound_lua_cb_t s_fp_finish_cbs[MAX_FILEPLAYER_CBS];
static sound_lua_cb_t s_fp_loop_cbs[MAX_FILEPLAYER_CBS];

#define MAX_SAMPLEPLAYER_CBS SOUND_MAX_PLAYERS
static sound_lua_cb_t s_sp_finish_cbs[MAX_SAMPLEPLAYER_CBS];
static sound_lua_cb_t s_sp_loop_cbs[MAX_SAMPLEPLAYER_CBS];

// Trampolines — run on Core 1, just set pending flags (the slot's, and the
// service pass's so the next hook call fires it)
static int trampoline_set_pending(void *arg) {
    sound_lua_cb_t *cb = (sound_lua_cb_t *)arg;
    cb->pending = 1;
    lua_bridge_request_service();
    return 0;
}

// Helper: find or allocate a callback slot for a given player
static sound_lua_cb_t *find_or_alloc_cb(sound_lua_cb_t *arr, int count,
                                         void *match_arg) {
    // Try to reuse an existing slot for this player
    for (int i = 0; i < count; i++) {
        if (arr[i].ref != 0 && match_arg == &arr[i])
            return &arr[i];
    }
    // Allocate a free slot
    for (int i = 0; i < count; i++) {
        if (arr[i].ref == 0)
            return &arr[i];
    }
    return NULL;
}

// Helper: register a Lua callback into a slot array.
// Returns the slot pointer (for wiring the C callback), or NULL on failure.
static sound_lua_cb_t *register_lua_cb(lua_State *L, sound_lua_cb_t *arr,
                                        int count, void *match_arg) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    sound_lua_cb_t *cb = find_or_alloc_cb(arr, count, match_arg);
    if (!cb)
        return NULL;
    if (cb->ref != 0)
        luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
    lua_pushvalue(L, 2);
    cb->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    cb->L = L;
    cb->pending = 0;
    return cb;
}

// Fires one pending slot's callback. A failed callback's error is printed
// and popped: lua_bridge_service runs this from sys.sleep and the terminal
// waits too, whose C frames live for the whole wait, so every error left on
// the stack used to pile up there until lua_rawgeti wrote past the stack.
static void fire_sound_cb(lua_State *L, sound_lua_cb_t *cb, const char *what) {
    if (!cb->pending || cb->ref == 0)
        return;
    cb->pending = 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        // sys.exit() in the callback is not an error: the exit request is
        // sticky, and the service pass raises it again.
        if (!lua_bridge_is_exit_sentinel(L, -1)) {
            const char *err = lua_tostring(L, -1);
            printf("[SOUND-LUA] %s callback error: %s\n", what,
                   err ? err : "(not a string)");
        }
        lua_pop(L, 1);
    }
}

// Poll all callback slots and fire any pending Lua functions.
// Called on Core 0 by lua_bridge_service (count hook, sys.sleep, terminal
// waits).
void lua_bridge_sound_poll(lua_State *L) {
    for (int i = 0; i < MAX_FILEPLAYER_CBS; i++)
        fire_sound_cb(L, &s_fp_finish_cbs[i], "fileplayer finish");
    for (int i = 0; i < MAX_FILEPLAYER_CBS; i++)
        fire_sound_cb(L, &s_fp_loop_cbs[i], "fileplayer loop");
    for (int i = 0; i < MAX_SAMPLEPLAYER_CBS; i++)
        fire_sound_cb(L, &s_sp_finish_cbs[i], "sampleplayer finish");
    for (int i = 0; i < MAX_SAMPLEPLAYER_CBS; i++)
        fire_sound_cb(L, &s_sp_loop_cbs[i], "sampleplayer loop");
}

// ── Fileplayer callbacks ────────────────────────────────────────────────────

static int l_sound_fileplayer_setFinishCallback(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    sound_lua_cb_t *cb = register_lua_cb(L, s_fp_finish_cbs, MAX_FILEPLAYER_CBS,
                                          player->finish_callback_arg);
    if (!cb) return luaL_error(L, "too many fileplayer finish callbacks");
    fileplayer_set_finish_callback(player, trampoline_set_pending, cb);
    return 0;
}

static int l_sound_fileplayer_setLoopCallback(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    sound_lua_cb_t *cb = register_lua_cb(L, s_fp_loop_cbs, MAX_FILEPLAYER_CBS,
                                          player->loop_callback_arg);
    if (!cb) return luaL_error(L, "too many fileplayer loop callbacks");
    fileplayer_set_loop_callback(player, trampoline_set_pending, cb);
    return 0;
}

// ── Sampleplayer callbacks ──────────────────────────────────────────────────

static int l_sound_sampleplayer_setFinishCallback(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    sound_lua_cb_t *cb = register_lua_cb(L, s_sp_finish_cbs, MAX_SAMPLEPLAYER_CBS,
                                          player->finish_callback_arg);
    if (!cb) return luaL_error(L, "too many sampleplayer finish callbacks");
    sound_player_set_finish_callback(player, trampoline_set_pending, cb);
    return 0;
}

static int l_sound_sampleplayer_setLoopCallback(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    sound_lua_cb_t *cb = register_lua_cb(L, s_sp_loop_cbs, MAX_SAMPLEPLAYER_CBS,
                                          player->loop_callback_arg);
    if (!cb) return luaL_error(L, "too many sampleplayer loop callbacks");
    sound_player_set_loop_callback(player, trampoline_set_pending, cb);
    return 0;
}

// A collected sampleplayer gives its callback slots back (they used to stay
// taken, so the ninth player ever given a callback failed).
static void release_cb(lua_State *L, sound_lua_cb_t *arr, int count, void *arg) {
    for (int i = 0; i < count; i++) {
        if (arg == &arr[i] && arr[i].ref != 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, arr[i].ref);
            arr[i].ref = 0;
            arr[i].pending = 0;
        }
    }
}

static void release_player_cbs(lua_State *L, sound_player_t *player) {
    release_cb(L, s_sp_finish_cbs, MAX_SAMPLEPLAYER_CBS, player->finish_callback_arg);
    release_cb(L, s_sp_loop_cbs, MAX_SAMPLEPLAYER_CBS, player->loop_callback_arg);
}

static int l_sound_fileplayer_didUnderrun(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushboolean(L, fileplayer_did_underrun(player));
    return 1;
}

static int l_sound_fileplayer_setStopOnUnderrun(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    bool flag = lua_toboolean(L, 2);
    fileplayer_set_stop_on_underrun(player, flag);
    return 0;
}

static int l_sound_fileplayer_setRate(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    float rate = (float)luaL_checknumber(L, 2);
    fileplayer_set_rate(player, rate);
    return 0;
}

static int l_sound_fileplayer_getRate(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushnumber(L, fileplayer_get_rate(player));
    return 1;
}

static int l_sound_fileplayer_gc(lua_State *L) {
    fileplayer_t **ud = lb_checkudata(L, 1, FILEPLAYER_USERDATA);
    if (*ud) {
        // As for sampleplayers: free first (fileplayer_destroy detaches the
        // callbacks under the player lock), then give the Lua callback slots
        // back. They used to stay taken, so the third fileplayer ever given
        // a callback failed with "too many ... callbacks".
        void *finish_arg = (*ud)->finish_callback_arg;
        void *loop_arg = (*ud)->loop_callback_arg;
        g_api.soundplayer->filePlayerFree(*ud);
        release_cb(L, s_fp_finish_cbs, MAX_FILEPLAYER_CBS, finish_arg);
        release_cb(L, s_fp_loop_cbs, MAX_FILEPLAYER_CBS, loop_arg);
        *ud = NULL;
    }
    return 0;
}

// There is one MP3 player (mp3_player_create returns the same &s_player and
// mp3_player_destroy stops it), so collecting one of two handles used to stop
// the other's music. As for picocalc.modplayer (lua_bridge_mod.c):
// mp3player() hands out one Lua handle, returned again while it is alive; the
// registry holds it in a weak-valued table keyed by &s_mp3_owner, and only
// the owning handle's __gc stops the player. A handle that became unreachable
// but whose finaliser has not run yet is already gone from the weak table, so
// an mp3player() in that window makes a new owner and the stale finaliser
// must not stop it. Compared, never dereferenced.
static const void *s_mp3_owner = NULL;

static int l_sound_mp3player_new(lua_State *L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_mp3_owner);  // weak cache
    if (lua_rawgeti(L, -1, 1) == LUA_TUSERDATA) {
        mp3_player_t **live = lb_checkudata(L, -1, MP3PLAYER_USERDATA);
        if (*live && (const void *)live == s_mp3_owner)
            return 1;  // the one live handle
    }
    lua_pop(L, 1);  // cache[1]: nil, or a handle that no longer owns the player

    // The userdata first: if it raises (out of memory) nothing is created.
    mp3_player_t **ud = lua_newuserdatauv(L, sizeof(mp3_player_t *), 0);
    *ud = NULL;
    luaL_setmetatable(L, MP3PLAYER_USERDATA);
    mp3_player_t *player = (mp3_player_t *)g_api.soundplayer->mp3PlayerNew();
    if (!player) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create mp3 player");
        return 2;
    }
    *ud = player;
    s_mp3_owner = ud;
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, 1);  // cache[1] = handle
    return 1;
}

static int l_sound_mp3player_load(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    const char *path = luaL_checkstring(L, 2);
    if (!fs_sandbox_check(L, path, false)) {
        lua_pushnil(L);
        lua_pushstring(L, "access denied");
        return 2;
    }
    if (!mp3_player_load(player, path)) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to load mp3");
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_mp3player_play(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    uint8_t repeat = (uint8_t)lb_clamp_int(lb_optint(L, 2, 1), 0, 255);
    g_api.soundplayer->mp3PlayerSetLoop(player, repeat == 0);
    g_api.soundplayer->mp3PlayerPlay(player, repeat);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_mp3player_stop(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    g_api.soundplayer->mp3PlayerStop(player);
    return 0;
}

static int l_sound_mp3player_pause(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    g_api.soundplayer->mp3PlayerPause(player);
    return 0;
}

static int l_sound_mp3player_resume(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    g_api.soundplayer->mp3PlayerResume(player);
    return 0;
}

static int l_sound_mp3player_isPlaying(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushboolean(L, g_api.soundplayer->mp3PlayerIsPlaying(player));
    return 1;
}

static int l_sound_mp3player_getPosition(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, mp3_player_get_position(player));
    return 1;
}

static int l_sound_mp3player_getLength(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, mp3_player_get_length(player));
    return 1;
}

static int l_sound_mp3player_setVolume(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    uint8_t vol = (uint8_t)lb_clamp_int(lb_checkint(L, 2), 0, 100);
    g_api.soundplayer->mp3PlayerSetVolume(player, vol);
    return 0;
}

static int l_sound_mp3player_getVolume(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, g_api.soundplayer->mp3PlayerGetVolume(player));
    return 1;
}

static int l_sound_mp3player_getSampleRate(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, mp3_player_get_sample_rate(player));
    return 1;
}

static int l_sound_mp3player_setLoop(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    bool loop = lua_toboolean(L, 2);
    g_api.soundplayer->mp3PlayerSetLoop(player, loop);
    return 0;
}

// Only the owning handle stops the player (see s_mp3_owner); a stale handle
// just goes dead.
static int l_sound_mp3player_gc(lua_State *L) {
    mp3_player_t **ud = lb_checkudata(L, 1, MP3PLAYER_USERDATA);
    if (*ud && (const void *)ud == s_mp3_owner) {
        g_api.soundplayer->mp3PlayerFree(*ud);
        s_mp3_owner = NULL;
    }
    *ud = NULL;
    return 0;
}

static int l_sound_playingSources(lua_State *L) {
    lua_pushinteger(L, sound_get_playing_source_count());
    return 1;
}

static int l_sound_getCurrentTime(lua_State *L) {
    lua_pushinteger(L, sound_get_current_time());
    return 1;
}

static int l_sound_resetTime(lua_State *L) {
    (void)L;
    sound_reset_time();
    return 0;
}

static const luaL_Reg sound_sample_methods[] = {
    {"load", l_sound_sample_load},
    {"getLength", l_sound_sample_getLength},
    {"getSampleRate", l_sound_sample_getSampleRate},
    {"getFormat", l_sound_sample_getFormat},
    {"decompress", l_sound_sample_decompress},
    {"getSubsample", l_sound_sample_getSubsample},
    {"play", l_sound_sample_play},
    {"playAt", l_sound_sample_playAt},
    {"save", l_sound_sample_save},
    {NULL, NULL}
};

static const luaL_Reg sound_player_methods[] = {
    {"setSample", l_sound_sampleplayer_setSample},
    {"getSample", l_sound_sampleplayer_getSample},
    {"play", l_sound_sampleplayer_play},
    {"stop", l_sound_sampleplayer_stop},
    {"isPlaying", l_sound_sampleplayer_isPlaying},
    {"setPaused", l_sound_sampleplayer_setPaused},
    {"getLength", l_sound_sampleplayer_getLength},
    {"setOffset", l_sound_sampleplayer_setOffset},
    {"getOffset", l_sound_sampleplayer_getOffset},
    {"setVolume", l_sound_sampleplayer_setVolume},
    {"getVolume", l_sound_sampleplayer_getVolume},
    {"setPlayRange", l_sound_sampleplayer_setPlayRange},
    {"setRate", l_sound_sampleplayer_setRate},
    {"getRate", l_sound_sampleplayer_getRate},
    {"setFinishCallback", l_sound_sampleplayer_setFinishCallback},
    {"setLoopCallback", l_sound_sampleplayer_setLoopCallback},
    {NULL, NULL}
};

static const luaL_Reg sound_fileplayer_methods[] = {
    {"load", l_sound_fileplayer_load},
    {"play", l_sound_fileplayer_play},
    {"stop", l_sound_fileplayer_stop},
    {"pause", l_sound_fileplayer_pause},
    {"resume", l_sound_fileplayer_resume},
    {"isPlaying", l_sound_fileplayer_isPlaying},
    {"getLength", l_sound_fileplayer_getLength},
    {"getSampleRate", l_sound_fileplayer_getSampleRate},
    {"getOffset", l_sound_fileplayer_getOffset},
    {"setOffset", l_sound_fileplayer_setOffset},
    {"setVolume", l_sound_fileplayer_setVolume},
    {"getVolume", l_sound_fileplayer_getVolume},
    {"setLoopRange", l_sound_fileplayer_setLoopRange},
    {"didUnderrun", l_sound_fileplayer_didUnderrun},
    {"setFinishCallback", l_sound_fileplayer_setFinishCallback},
    {"setLoopCallback", l_sound_fileplayer_setLoopCallback},
    {"setStopOnUnderrun", l_sound_fileplayer_setStopOnUnderrun},
    {"setRate", l_sound_fileplayer_setRate},
    {"getRate", l_sound_fileplayer_getRate},
    {NULL, NULL}
};

static const luaL_Reg sound_mp3player_methods[] = {
    {"load", l_sound_mp3player_load},
    {"play", l_sound_mp3player_play},
    {"stop", l_sound_mp3player_stop},
    {"pause", l_sound_mp3player_pause},
    {"resume", l_sound_mp3player_resume},
    {"isPlaying", l_sound_mp3player_isPlaying},
    {"getPosition", l_sound_mp3player_getPosition},
    {"getLength", l_sound_mp3player_getLength},
    {"setVolume", l_sound_mp3player_setVolume},
    {"getVolume", l_sound_mp3player_getVolume},
    {"getSampleRate", l_sound_mp3player_getSampleRate},
    {"setLoop", l_sound_mp3player_setLoop},
    {NULL, NULL}
};

static const luaL_Reg sound_funcs[] = {
    {"sample", l_sound_sample_new},
    {"sampleplayer", l_sound_sampleplayer_new},
    {"fileplayer", l_sound_fileplayer_new},
    {"mp3player", l_sound_mp3player_new},
    {"getCurrentTime", l_sound_getCurrentTime},
    {"resetTime", l_sound_resetTime},
    {"playingSources", l_sound_playingSources},
    {NULL, NULL}
};

// Finalisers stay out of the method tables (lb_register_type).
static const luaL_Reg sound_sample_meta[] = {
    {"__gc", l_sound_sample_gc}, {NULL, NULL}};
static const luaL_Reg sound_player_meta[] = {
    {"__gc", l_sound_sampleplayer_gc}, {NULL, NULL}};
static const luaL_Reg sound_fileplayer_meta[] = {
    {"__gc", l_sound_fileplayer_gc}, {NULL, NULL}};
static const luaL_Reg sound_mp3player_meta[] = {
    {"__gc", l_sound_mp3player_gc}, {NULL, NULL}};

void lua_bridge_sound_init(lua_State *L) {
    lb_register_type(L, SAMPLE_USERDATA, sound_sample_methods, sound_sample_meta);
    lb_register_type(L, PLAYER_USERDATA, sound_player_methods, sound_player_meta);
    lb_register_type(L, FILEPLAYER_USERDATA, sound_fileplayer_methods,
                     sound_fileplayer_meta);
    lb_register_type(L, MP3PLAYER_USERDATA, sound_mp3player_methods,
                     sound_mp3player_meta);

    // Reset audio state from any previous app (stop timers, close files, clear players)
    sound_init();
    fileplayer_reset();
    mp3_player_reset();

    // Clear Lua callback refs from previous app
    memset(s_fp_finish_cbs, 0, sizeof(s_fp_finish_cbs));
    memset(s_fp_loop_cbs, 0, sizeof(s_fp_loop_cbs));
    memset(s_sp_finish_cbs, 0, sizeof(s_sp_finish_cbs));
    memset(s_sp_loop_cbs, 0, sizeof(s_sp_loop_cbs));

    fileplayer_init();
    mp3_player_init();

    // A fresh lua_State: the previous app's mp3player handle was finalised
    // by lua_close (which cleared s_mp3_owner); this is the safety net.
    s_mp3_owner = NULL;
    lua_newtable(L);  // weak-valued handle cache
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &s_mp3_owner);

    register_subtable(L, "sound", sound_funcs);
}
