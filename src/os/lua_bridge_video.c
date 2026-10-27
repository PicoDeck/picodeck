#include "lua_bridge_internal.h"
#include "../drivers/video_player.h"

#define VIDEO_USERDATA "video_player"

// The live player at idx, or a Lua error. The pointer is NULL once __gc
// destroyed it (reachable only from a later finaliser: __gc is not a method).
static video_player_t *check_video(lua_State *L, int idx) {
    video_player_t **ud = lb_checkudata(L, idx, VIDEO_USERDATA);
    if (!*ud)
        luaL_error(L, "attempt to use a destroyed video player");
    return *ud;
}

static int l_video_new(lua_State *L) {
    // The userdata first: if it raises (out of memory) there is no player to
    // leak, and __gc ignores the NULL a failed create leaves behind.
    video_player_t **ud = lua_newuserdatauv(L, sizeof(video_player_t *), 0);
    *ud = NULL;
    luaL_setmetatable(L, VIDEO_USERDATA);
    video_player_t *player = video_player_create();
    if (!player) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create video player");
        return 2;
    }
    *ud = player;
    return 1;
}

static int l_video_load(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    const char *path = luaL_checkstring(L, 2);
    if (!fs_sandbox_check(L, path, false)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "access denied");
        return 2;
    }
    if (video_player_load(player, path)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, false);
    return 1;
}

static int l_video_play(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    video_player_play(player);
    return 0;
}

static int l_video_pause(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    video_player_pause(player);
    return 0;
}

static int l_video_resume(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    video_player_resume(player);
    return 0;
}

static int l_video_stop(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    video_player_stop(player);
    return 0;
}

static int l_video_update(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushboolean(L, video_player_update(player));
    return 1;
}

static int l_video_isPlaying(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushboolean(L, player->playing && !player->paused);
    return 1;
}

static int l_video_isPaused(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushboolean(L, player->paused);
    return 1;
}

static int l_video_seek(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    uint32_t frame = (uint32_t)lb_clamp_int(lb_checkint(L, 2), 0, LUA_MAXINTEGER);
    video_player_seek(player, frame);
    return 0;
}

static int l_video_getFPS(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushnumber(L, video_player_get_fps(player));
    return 1;
}

static int l_video_getSize(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushinteger(L, player->width);
    lua_pushinteger(L, player->height);
    return 2;
}

static int l_video_getInfo(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_newtable(L);
    lua_pushinteger(L, player->width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, player->height); lua_setfield(L, -2, "height");
    lua_pushinteger(L, player->frame_count); lua_setfield(L, -2, "frames");
    lua_pushinteger(L, player->current_frame); lua_setfield(L, -2, "current_frame");
    lua_pushinteger(L, video_player_get_dropped_frames(player)); lua_setfield(L, -2, "dropped_frames");
    lua_pushboolean(L, video_player_has_audio(player)); lua_setfield(L, -2, "has_audio");
    lua_pushinteger(L, video_player_get_duration_ms(player)); lua_setfield(L, -2, "duration_ms");
    lua_pushinteger(L, video_player_get_position_ms(player)); lua_setfield(L, -2, "position_ms");
    lua_pushboolean(L, video_player_has_ended(player)); lua_setfield(L, -2, "ended");
    if (player->fps_den) {
        lua_pushnumber(L, (lua_Number)player->fps_num / (lua_Number)player->fps_den);
        lua_setfield(L, -2, "fps");
    }
    return 1;
}

static int l_video_getFrameCount(lua_State *L) {
    lua_pushinteger(L, video_player_get_frame_count(check_video(L, 1)));
    return 1;
}

static int l_video_getDurationMs(lua_State *L) {
    lua_pushinteger(L, video_player_get_duration_ms(check_video(L, 1)));
    return 1;
}

static int l_video_getPositionMs(lua_State *L) {
    lua_pushinteger(L, video_player_get_position_ms(check_video(L, 1)));
    return 1;
}

static int l_video_seekMs(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_Integer ms = lb_checkint(L, 2);
    video_player_seek_ms(player, ms < 0 ? 0 : (uint32_t)ms);
    return 0;
}

static int l_video_seekRelativeMs(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    video_player_seek_relative_ms(player, (int32_t)lb_checkint(L, 2));
    return 0;
}

static int l_video_hasEnded(lua_State *L) {
    lua_pushboolean(L, video_player_has_ended(check_video(L, 1)));
    return 1;
}

static int l_video_setOSD(lua_State *L) {
    video_player_set_osd(check_video(L, 1), lua_toboolean(L, 2));
    return 0;
}

static int l_video_showOSD(lua_State *L) {
    video_player_show_osd(check_video(L, 1));
    return 0;
}

static int l_video_setOSDTimeout(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_Integer ms = lb_checkint(L, 2);
    video_player_set_osd_timeout(player, ms < 0 ? 0 : (uint32_t)ms);
    return 0;
}

static int l_video_hasAudio(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushboolean(L, video_player_has_audio(player));
    return 1;
}

static int l_video_setVolume(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    uint8_t vol = (uint8_t)lb_clamp_int(lb_checkint(L, 2), 0, 100);
    video_player_set_audio_volume(player, vol);
    return 0;
}

static int l_video_getVolume(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushinteger(L, video_player_get_audio_volume(player));
    return 1;
}

static int l_video_setMuted(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    video_player_set_audio_muted(player, lua_toboolean(L, 2));
    return 0;
}

static int l_video_isMuted(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushboolean(L, video_player_get_audio_muted(player));
    return 1;
}

static int l_video_getDroppedFrames(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    lua_pushinteger(L, video_player_get_dropped_frames(player));
    return 1;
}

static int l_video_resetStats(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    video_player_reset_stats(player);
    return 0;
}

static int l_video_setLoop(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    player->loop = lua_toboolean(L, 2);
    return 0;
}

static int l_video_setAutoFlush(lua_State *L) {
    video_player_t *player = check_video(L, 1);
    player->auto_flush = lua_toboolean(L, 2);
    return 0;
}

static int l_video_gc(lua_State *L) {
    video_player_t **ud = lb_checkudata(L, 1, VIDEO_USERDATA);
    if (*ud) {
        video_player_destroy(*ud);
        *ud = NULL;
    }
    return 0;
}

static const luaL_Reg video_methods[] = {
    {"load", l_video_load},
    {"play", l_video_play},
    {"pause", l_video_pause},
    {"resume", l_video_resume},
    {"stop", l_video_stop},
    {"update", l_video_update},
    {"seek", l_video_seek},
    {"seekMs", l_video_seekMs},
    {"seekRelativeMs", l_video_seekRelativeMs},
    {"getFrameCount", l_video_getFrameCount},
    {"getDurationMs", l_video_getDurationMs},
    {"getPositionMs", l_video_getPositionMs},
    {"hasEnded", l_video_hasEnded},
    {"setOSD", l_video_setOSD},
    {"showOSD", l_video_showOSD},
    {"setOSDTimeout", l_video_setOSDTimeout},
    {"isPlaying", l_video_isPlaying},
    {"isPaused", l_video_isPaused},
    {"getFPS", l_video_getFPS},
    {"getSize", l_video_getSize},
    {"getInfo", l_video_getInfo},
    {"getDroppedFrames", l_video_getDroppedFrames},
    {"resetStats", l_video_resetStats},
    {"setLoop", l_video_setLoop},
    {"setAutoFlush", l_video_setAutoFlush},
    {"hasAudio", l_video_hasAudio},
    {"setVolume", l_video_setVolume},
    {"getVolume", l_video_getVolume},
    {"setMuted", l_video_setMuted},
    {"isMuted", l_video_isMuted},
    {NULL, NULL}
};

static const luaL_Reg video_meta[] = {
    {"__gc", l_video_gc},
    {NULL, NULL}
};

static const luaL_Reg video_funcs[] = {
    {"player", l_video_new},
    {NULL, NULL}
};

void lua_bridge_video_init(lua_State *L) {
    lb_register_type(L, VIDEO_USERDATA, video_methods, video_meta);

    register_subtable(L, "video", video_funcs);
}
