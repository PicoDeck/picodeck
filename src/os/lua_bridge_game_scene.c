#include "lua_bridge.h"
#include "lua_bridge_internal.h"
#include <stdlib.h>
#include <string.h>

#define MAX_SCENES 16
#define MAX_SCENE_STACK 4
#define MAX_POOLS 8
#define MAX_POOL_SIZE 64

typedef struct {
    char name[64];
    int ref;  // Lua registry reference to scene table
} scene_entry_t;

typedef struct {
    int create_ref;  // Lua function to create new object
    int objects[MAX_POOL_SIZE];  // idle objects, a stack of registry refs
    int count;
} object_pool_t;

static scene_entry_t s_scenes[MAX_SCENES];
static int s_scene_count = 0;
static int s_scene_stack[MAX_SCENE_STACK];
static int s_stack_depth = 0;
static object_pool_t s_pools[MAX_POOLS];
static int s_pool_count = 0;
static int s_globals_ref = LUA_NOREF;

static int find_scene(const char *name) {
    for (int i = 0; i < s_scene_count; i++) {
        if (strcmp(s_scenes[i].name, name) == 0) return i;
    }
    return -1;
}

static int l_scene_new(lua_State *L) {
    lua_newtable(L);
    return 1;
}

static int l_scene_add(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    
    // A cut name would never be found again.
    if (strlen(name) >= sizeof(s_scenes[0].name)) {
        return luaL_error(L, "scene name too long (max %d bytes)",
                          (int)sizeof(s_scenes[0].name) - 1);
    }
    
    int idx = find_scene(name);
    if (idx >= 0) {
        // Replacing: a stack entry holding the old ref follows the new one.
        int old_ref = s_scenes[idx].ref;
        luaL_unref(L, LUA_REGISTRYINDEX, old_ref);
        lua_pushvalue(L, 2);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        s_scenes[idx].ref = ref;
        for (int i = 0; i < s_stack_depth; i++) {
            if (s_scene_stack[i] == old_ref) s_scene_stack[i] = ref;
        }
        return 0;
    }
    if (s_scene_count >= MAX_SCENES) {
        return luaL_error(L, "too many scenes (max %d)", MAX_SCENES);
    }
    idx = s_scene_count++;
    
    strncpy(s_scenes[idx].name, name, sizeof(s_scenes[idx].name) - 1);
    lua_pushvalue(L, 2);
    s_scenes[idx].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    
    return 0;
}

static int l_scene_remove(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int idx = find_scene(name);
    if (idx < 0) return 0;
    
    // Off the stack too (no exit() runs): the ref is free to mean something
    // else from here on.
    int ref = s_scenes[idx].ref;
    int depth = 0;
    for (int i = 0; i < s_stack_depth; i++) {
        if (s_scene_stack[i] != ref) s_scene_stack[depth++] = s_scene_stack[i];
    }
    s_stack_depth = depth;
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    
    for (int i = idx; i < s_scene_count - 1; i++) {
        s_scenes[i] = s_scenes[i + 1];
    }
    s_scene_count--;
    
    return 0;
}

static int l_scene_has(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_pushboolean(L, find_scene(name) >= 0);
    return 1;
}

// Calls a scene's function with no arguments, if it has it. An error in it is
// the app's error: it propagates like any other, to the app's pcall or to the
// error screen.
static void call_scene_method(lua_State *L, int scene_ref, const char *method) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, scene_ref);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    
    lua_getfield(L, -1, method);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    
    lua_remove(L, -2);  // Remove the scene table: no self
    lua_call(L, 0, 0);
}

static int l_scene_switch(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int idx = find_scene(name);
    if (idx < 0) {
        return luaL_error(L, "scene '%s' not found", name);
    }
    
    if (s_stack_depth > 0) {
        int old_ref = s_scene_stack[s_stack_depth - 1];
        call_scene_method(L, old_ref, "exit");
    }
    
    s_scene_stack[0] = s_scenes[idx].ref;
    s_stack_depth = 1;
    
    call_scene_method(L, s_scenes[idx].ref, "enter");
    
    return 0;
}

static int l_scene_push(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int idx = find_scene(name);
    if (idx < 0) {
        return luaL_error(L, "scene '%s' not found", name);
    }
    
    if (s_stack_depth >= MAX_SCENE_STACK) {
        return luaL_error(L, "scene stack overflow (max %d)", MAX_SCENE_STACK);
    }
    
    s_scene_stack[s_stack_depth++] = s_scenes[idx].ref;
    call_scene_method(L, s_scenes[idx].ref, "enter");
    
    return 0;
}

static int l_scene_pop(lua_State *L) {
    if (s_stack_depth <= 1) {
        return luaL_error(L, "cannot pop last scene");
    }
    
    int old_ref = s_scene_stack[--s_stack_depth];
    call_scene_method(L, old_ref, "exit");
    
    int new_ref = s_scene_stack[s_stack_depth - 1];
    call_scene_method(L, new_ref, "enter");
    
    return 0;
}

static int l_scene_getCurrent(lua_State *L) {
    if (s_stack_depth == 0) {
        lua_pushnil(L);
        return 1;
    }
    
    int ref = s_scene_stack[s_stack_depth - 1];
    for (int i = 0; i < s_scene_count; i++) {
        if (s_scenes[i].ref == ref) {
            lua_pushstring(L, s_scenes[i].name);
            return 1;
        }
    }
    
    lua_pushnil(L);
    return 1;
}

static int l_scene_update(lua_State *L) {
    if (s_stack_depth == 0) return 0;
    
    float dt = (float)luaL_optnumber(L, 1, 0.016);
    int ref = s_scene_stack[s_stack_depth - 1];
    
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    
    lua_getfield(L, -1, "update");
    if (lua_isfunction(L, -1)) {
        lua_remove(L, -2);  // Remove scene table, keep only function
        lua_pushnumber(L, dt);
        lua_call(L, 1, 0);  // Call with 1 arg: dt
    } else {
        lua_pop(L, 2);
    }
    
    return 0;
}

static int l_scene_draw(lua_State *L) {
    if (s_stack_depth == 0) return 0;
    
    call_scene_method(L, s_scene_stack[s_stack_depth - 1], "draw");
    return 0;
}

static int l_scene_objectPool(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    
    if (s_pool_count >= MAX_POOLS) {
        return luaL_error(L, "too many object pools (max %d)", MAX_POOLS);
    }
    
    object_pool_t *pool = &s_pools[s_pool_count++];
    lua_pushvalue(L, 2);
    pool->create_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    pool->count = 0;
    
    for (int i = 0; i < MAX_POOL_SIZE; i++) {
        pool->objects[i] = LUA_NOREF;
    }
    
    lua_newtable(L);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    
    lua_pushlightuserdata(L, pool);
    lua_setfield(L, -2, "_pool");
    
    luaL_getmetatable(L, "picocalc.game.scene.pool");
    lua_setmetatable(L, -2);
    
    return 1;
}

// The C pool behind the table at index 1. _pool is an ordinary field the app
// can overwrite, so it counts only if it is one of s_pools.
static object_pool_t *check_pool(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "_pool");
    const void *p = lua_islightuserdata(L, -1) ? lua_touserdata(L, -1) : NULL;
    lua_pop(L, 1);
    for (int i = 0; i < s_pool_count; i++) {
        if (p == &s_pools[i]) return &s_pools[i];
    }
    luaL_error(L, "invalid pool");
    return NULL;
}

static int l_pool_acquire(lua_State *L) {
    object_pool_t *pool = check_pool(L);
    
    if (pool->count > 0) {
        int ref = pool->objects[--pool->count];
        pool->objects[pool->count] = LUA_NOREF;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return 1;
    }
    
    // A factory's error is the app's error, not an object.
    lua_rawgeti(L, LUA_REGISTRYINDEX, pool->create_ref);
    lua_call(L, 0, 1);
    return 1;
}

static int l_pool_release(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 2);
    
    object_pool_t *pool = check_pool(L);
    
    if (pool->count >= MAX_POOL_SIZE) {
        return 0;
    }
    
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    pool->objects[pool->count++] = ref;
    
    return 0;
}

static int l_pool_prewarm(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int count = luaL_checkinteger(L, 2);
    if (count < 0) count = 0;
    if (count > MAX_POOL_SIZE) count = MAX_POOL_SIZE;
    
    object_pool_t *pool = check_pool(L);
    
    for (int i = 0; i < count && pool->count < MAX_POOL_SIZE; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, pool->create_ref);
        lua_call(L, 0, 1);
        // The factory may have released objects into this very pool.
        if (pool->count >= MAX_POOL_SIZE) {
            lua_pop(L, 1);
            break;
        }
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        pool->objects[pool->count++] = ref;
    }
    
    return 0;
}

static int l_pool_clear(lua_State *L) {
    object_pool_t *pool = check_pool(L);
    
    for (int i = 0; i < pool->count; i++) {
        luaL_unref(L, LUA_REGISTRYINDEX, pool->objects[i]);
        pool->objects[i] = LUA_NOREF;
    }
    pool->count = 0;
    
    return 0;
}

static int l_scene_setGlobal(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checkany(L, 2);
    
    if (s_globals_ref == LUA_NOREF) {
        lua_newtable(L);
        s_globals_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_globals_ref);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
    
    return 0;
}

static int l_scene_getGlobal(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    
    if (s_globals_ref == LUA_NOREF) {
        lua_pushnil(L);
        return 1;
    }
    
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_globals_ref);
    lua_getfield(L, -1, key);
    lua_remove(L, -2);
    return 1;
}

static int l_scene_clearGlobals(lua_State *L) {
    if (s_globals_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_globals_ref);
        s_globals_ref = LUA_NOREF;
    }
    return 0;
}

static const luaL_Reg l_scene_lib[] = {
    {"new", l_scene_new},
    {"add", l_scene_add},
    {"remove", l_scene_remove},
    {"has", l_scene_has},
    {"switch", l_scene_switch},
    {"push", l_scene_push},
    {"pop", l_scene_pop},
    {"getCurrent", l_scene_getCurrent},
    {"update", l_scene_update},
    {"draw", l_scene_draw},
    {"objectPool", l_scene_objectPool},
    {"setGlobal", l_scene_setGlobal},
    {"getGlobal", l_scene_getGlobal},
    {"clearGlobals", l_scene_clearGlobals},
    {NULL, NULL}
};

static const luaL_Reg l_pool_methods[] = {
    {"acquire", l_pool_acquire},
    {"release", l_pool_release},
    {"prewarm", l_pool_prewarm},
    {"clear", l_pool_clear},
    {NULL, NULL}
};

void lua_bridge_game_scene_init(lua_State *L) {
    // Reset all static state from previous app launch — registry refs are
    // invalid in the new lua_State and must not be luaL_unref'd.
    s_scene_count = 0;
    s_stack_depth = 0;
    s_pool_count = 0;
    s_globals_ref = LUA_NOREF;
    memset(s_scenes, 0, sizeof(s_scenes));
    memset(s_scene_stack, 0, sizeof(s_scene_stack));
    memset(s_pools, 0, sizeof(s_pools));

    lb_register_type(L, "picocalc.game.scene.pool", l_pool_methods, NULL);

    lua_newtable(L);
    luaL_setfuncs(L, l_scene_lib, 0);
}
