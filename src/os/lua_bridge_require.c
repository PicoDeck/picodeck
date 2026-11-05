// require(name): the text-only, sandboxed module loader for app code (Lua's
// package library stays closed). "a.b" is searched as "<app dir>/a/b.lua",
// then "/system/lib/a/b.lua"; the chunk runs once with (name, path) and its
// result is cached per app in the registry (true when it returns nil). A
// module that requires itself, directly or through a cycle, raises instead of
// recursing; a module whose load raised may be required again.
#include "lua_bridge_internal.h"
#include "app_identity.h"
#include "module_path.h"
#include "../drivers/sdcard.h"
#include "lauxlib.h"

#define REQUIRE_LOADED_KEY "picodeck.require.loaded"

static char s_loading_tag;  // its address marks a module being loaded

// Reads the module file at path if the sandbox lets the app read it: true and
// *src (a umm buffer, NULL for an empty chunk) if it exists, false if not. One
// open, no stat: each stat walks the whole path again (~10 sector reads on a
// card of 40 apps), and an app of 20 modules paid three walks for each. Only
// a failed read asks why, keeping "not found" apart from "cannot read" (out of
// memory, a card error).
static bool module_read(lua_State *L, const char *path, char **src, int *n) {
  *src = NULL;
  *n = 0;
  if (!fs_sandbox_check(L, path, false)) return false;
  *src = sdcard_read_file(path, n);
  if (*src) return true;
  int size = sdcard_fsize(path);
  if (size > 0) luaL_error(L, "require: cannot read '%s'", path);
  return size == 0;  // sized 0 and unopenable: a directory reads as empty
}

static int l_require(lua_State *L) {
  size_t len;
  const char *name = luaL_checklstring(L, 1, &len);
  const app_identity_t *me = app_identity_current();
  if (!me) return luaL_error(L, "require: no app is running");

  lua_settop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, REQUIRE_LOADED_KEY);  // 2: loaded
  lua_pushvalue(L, 1);
  lua_rawget(L, 2);
  if (lua_touserdata(L, -1) == &s_loading_tag)
    return luaL_error(L, "require: circular require of '%s'", name);
  if (!lua_isnil(L, -1)) return 1;
  lua_pop(L, 1);

  char app_path[192], lib_path[192];
  if (!module_path(name, len, me->dir, app_path, sizeof app_path) ||
      !module_path(name, len, "/system/lib", lib_path, sizeof lib_path))
    return luaL_argerror(L, 1, "invalid module name");

  // Each chunk name is pushed before its read, not after: lua_pushfstring can
  // itself raise a Lua memory error. If that happened while `src` (a plain umm
  // buffer, not Lua-owned) was already allocated, the longjmp would skip the
  // umm_free below and leak it. Pushing first means the only allocation that
  // can fail here is one with nothing yet to leak.
  const char *path = app_path;
  char *src;
  int n;
  lua_pushfstring(L, "@%s", path);                          // 3: chunk name
  if (!module_read(L, path, &src, &n)) {
    path = lib_path;
    lua_pop(L, 1);
    lua_pushfstring(L, "@%s", path);                        // 3: chunk name
    if (!module_read(L, path, &src, &n))
      return luaL_error(L, "module '%s' not found:\n\tno file '%s'\n\tno file '%s'",
                        name, app_path, lib_path);
  }
  int st = luaL_loadbufferx(L, src ? src : "", src ? (size_t)n : 0,
                            lua_tostring(L, 3), "t");
  if (src) umm_free(src);
  if (st != LUA_OK) return lua_error(L);                    // syntax error
  // 4: chunk. Mark it loading, run it with (name, path).
  lua_pushvalue(L, 1);
  lua_pushlightuserdata(L, &s_loading_tag);
  lua_rawset(L, 2);
  lua_pushvalue(L, 1);
  lua_pushstring(L, path);
  if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
    lua_pushvalue(L, 1);                                    // forget the marker
    lua_pushnil(L);
    lua_rawset(L, 2);
    return lua_error(L);  // rethrow as is (an exit sentinel stays one)
  }
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
  }
  lua_pushvalue(L, 1);
  lua_pushvalue(L, -2);
  lua_rawset(L, 2);
  return 1;
}

void lua_bridge_require_init(lua_State *L) {
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, REQUIRE_LOADED_KEY);
  lua_pushcfunction(L, l_require);
  lua_setglobal(L, "require");
}
