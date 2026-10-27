#include "lua_bridge_internal.h"
#include "../drivers/mod_player.h"

#define MODPLAYER_USERDATA "modplayer"

// There is one MOD player (mod_player_create returns the same singleton and
// resets it). So create() hands out one Lua handle: while that handle is
// alive, create() returns it again, untouched. The registry holds it in a
// weak-valued table (keyed by &s_mod_owner), so it is still collected when
// the app drops it. s_mod_owner is the handle that owns the player: only its
// __gc tears the player down. A handle that became unreachable but whose
// finaliser has not run yet is already gone from the weak table, so a
// create() in that window makes a new owner, and the stale finaliser must
// not stop the new owner's music. Compared, never dereferenced.
static const void *s_mod_owner = NULL;

// The live player at idx, or a Lua error. The pointer is NULL once __gc ran
// (reachable only from a later finaliser: __gc is not a method).
static mod_player_t *check_modplayer(lua_State *L, int idx) {
    mod_player_t **ud = lb_checkudata(L, idx, MODPLAYER_USERDATA);
    if (!*ud)
        luaL_error(L, "attempt to use a destroyed modplayer");
    return *ud;
}

static int l_mod_create(lua_State *L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_mod_owner);  // weak cache
    if (lua_rawgeti(L, -1, 1) == LUA_TUSERDATA) {
        mod_player_t **live = lb_checkudata(L, -1, MODPLAYER_USERDATA);
        if (*live && (const void *)live == s_mod_owner)
            return 1;  // the one live handle
    }
    lua_pop(L, 1);  // cache[1]: nil, or a handle that no longer owns the player

    // The userdata first: if it raises (out of memory) nothing is reset.
    mod_player_t **ud = lua_newuserdatauv(L, sizeof(mod_player_t *), 0);
    *ud = NULL;
    luaL_setmetatable(L, MODPLAYER_USERDATA);
    mod_player_t *p = mod_player_create();
    if (!p) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create MOD player");
        return 2;
    }
    *ud = p;
    s_mod_owner = ud;
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, 1);  // cache[1] = handle
    return 1;
}

static int l_mod_load(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    const char *path = luaL_checkstring(L, 2);
    if (!fs_sandbox_check(L, path, false)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "access denied");
        return 2;
    }
    lua_pushboolean(L, mod_player_load(p, path));
    return 1;
}

static int l_mod_play(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    bool loop = false;
    if (lua_isboolean(L, 2)) loop = lua_toboolean(L, 2);
    mod_player_play(p, loop);
    return 0;
}

static int l_mod_stop(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_stop(p);
    return 0;
}

static int l_mod_pause(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_pause(p);
    return 0;
}

static int l_mod_resume(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_resume(p);
    return 0;
}

static int l_mod_is_playing(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    lua_pushboolean(L, mod_player_is_playing(p));
    return 1;
}

static int l_mod_set_volume(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    int vol = (int)lb_checkint(L, 2);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    mod_player_set_volume(p, (uint8_t)vol);
    return 0;
}

static int l_mod_get_volume(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    lua_pushinteger(L, mod_player_get_volume(p));
    return 1;
}

static int l_mod_set_loop(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_set_loop(p, lua_toboolean(L, 2));
    return 0;
}

// Only the owning handle stops the player and frees its module (see
// s_mod_owner); a stale handle just goes dead.
static int l_mod_gc(lua_State *L) {
    mod_player_t **ud = lb_checkudata(L, 1, MODPLAYER_USERDATA);
    if (*ud && (const void *)ud == s_mod_owner) {
        mod_player_destroy(*ud);
        s_mod_owner = NULL;
    }
    *ud = NULL;
    return 0;
}

static const luaL_Reg modplayer_methods[] = {
    {"load",       l_mod_load},
    {"play",       l_mod_play},
    {"stop",       l_mod_stop},
    {"pause",      l_mod_pause},
    {"resume",     l_mod_resume},
    {"isPlaying",  l_mod_is_playing},
    {"setVolume",  l_mod_set_volume},
    {"getVolume",  l_mod_get_volume},
    {"setLoop",    l_mod_set_loop},
    {NULL, NULL}
};

static const luaL_Reg modplayer_meta[] = {
    {"__gc", l_mod_gc},
    {NULL, NULL}
};

static const luaL_Reg modplayer_funcs[] = {
    {"create", l_mod_create},
    {NULL, NULL}
};

void lua_bridge_mod_init(lua_State *L) {
    // A fresh lua_State: the previous app's handle was finalised by
    // lua_close (which cleared s_mod_owner); this is the safety net.
    s_mod_owner = NULL;
    lua_newtable(L);  // weak-valued handle cache
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &s_mod_owner);

    // Create the modplayer metatable
    lb_register_type(L, MODPLAYER_USERDATA, modplayer_methods, modplayer_meta);

    // Register picocalc.modplayer subtable
    register_subtable(L, "modplayer", modplayer_funcs);
}
