// Argument checks for the bridge's hottest paths, in one call and no stack
// traffic.
//
// Every picocalc.* call checks its arguments through the Lua C API, and on
// the device that is a large share of a short call: luaL_checkudata looks the
// type name up in the registry (hash the name, strcmp it, a table get, a raw
// compare: ~8 us of a 28 us fb:getPixels(x, y, 1, 1)), and an integer
// argument costs lua_isinteger plus lua_tointeger. Code and data share the
// 16 KB XIP cache with the Lua heap and stack in PSRAM, so each extra API
// function walked is mostly instruction-fetch misses. These read the stack
// slot directly. They answer only the common case and never raise; the
// caller falls back to the standard API (and its errors) for anything else:
// see lb_checkudata / lb_checkint in lua_bridge.c.
//
// Part of the Lua library (cmake/picodeck_lua.cmake PICODECK_LUA_SOURCES),
// since it uses VM internals.
#pragma once

#include "lua.h"

// The memory block of the full userdata at `idx` (a stack index, positive or
// negative) when its metatable is the table `mt` (lua_topointer of it), else
// NULL: another type, not a full userdata, or a pseudo-index.
void *picodeck_lua_udata_with_mt(lua_State *L, int idx, const void *mt);

// 1 with *out set when the value at `idx` is a Lua integer (math.type
// "integer"); 0 for anything else, floats with integral values included, and
// for pseudo-indices.
int picodeck_lua_tointeger_strict(lua_State *L, int idx, lua_Integer *out);

// A table's array part, for the bridge's bulk loops over number sequences
// (particles, wireframe vertices): values read and written in place instead
// of a lua_rawgeti/lua_rawseti and a stack push and pop each. Every getter
// and setter answers only for key i (1-based) inside the array part and
// returns 0 otherwise, so the caller takes the standard API for that key
// (lb_array_* in lua_bridge_internal.h). The view is valid until the table
// is resized: a raw set of a key the table does not hold may resize it, so
// refresh the view after any standard-API write.
typedef struct {
  void *slots;    // TValue array (internal)
  unsigned size;  // luaH_realasize
} picodeck_lua_array_t;

// The array part of the table at stack index idx (size 0 for anything else).
void picodeck_lua_array_of(lua_State *L, int idx, picodeck_lua_array_t *a);
// lua_tonumber of key i, for a float or integer value only (no strings).
int picodeck_lua_array_getnum(const picodeck_lua_array_t *a, lua_Integer i,
                              float *out);
// The value of key i when it is a Lua integer.
int picodeck_lua_array_getint(const picodeck_lua_array_t *a, lua_Integer i,
                              lua_Integer *out);
// Raw sets of key i to a float, an integer or nil. Numbers and nil need no
// collector barrier.
int picodeck_lua_array_setnum(const picodeck_lua_array_t *a, lua_Integer i,
                              float v);
int picodeck_lua_array_setint(const picodeck_lua_array_t *a, lua_Integer i,
                              lua_Integer v);
int picodeck_lua_array_setnil(const picodeck_lua_array_t *a, lua_Integer i);
