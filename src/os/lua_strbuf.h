// Building a Lua string result in place, with no second copy.
//
// luaL_buffinitsize() hands back a scratch buffer (a boxed umm block once the
// result passes LUAL_BUFFERSIZE, 256 bytes here), and luaL_pushresultsize()
// then copies it into a newly allocated string. Both live in QMI PSRAM, where
// that copy is the expensive part: fb:getPixels of a 17 KB strip spent ~4 ms
// copying against ~2 ms reading the pixels. picodeck_lua_strbuf_init()
// creates the result string itself at its final size and returns its bytes,
// so the caller writes them once.
//
// Use it as luaL_buffinitsize/luaL_pushresultsize are used:
//
//   picodeck_lua_strbuf_t sb;
//   char *p = picodeck_lua_strbuf_init(L, &sb, n);   // pushes one slot
//   ... write up to n bytes at p (a call that raises is fine) ...
//   picodeck_lua_strbuf_push(L, &sb, used);          // used <= n
//
// init pushes one stack slot and push leaves the string in that slot (no other
// slot changes), so values pushed in between must be popped first. Until push
// returns, the slot is not a valid result: do not let Lua code see it. A
// result of at most PICODECK_LUA_STRBUF_SHORT bytes is written to `sb` and
// interned at push (Lua compares short strings by identity, so one must never
// be written in place); a longer one is the string object itself, so `used`
// short of `n` costs one copy of the `used` bytes. Bytes past `used` are
// never exposed.
//
// Part of the Lua library (cmake/picodeck_lua.cmake PICODECK_LUA_SOURCES),
// since it uses VM internals.
#pragma once

#include <stddef.h>

#include "lua.h"

// LUAI_MAXSHORTLEN (llimits.h, which lua_strbuf.c checks against).
#define PICODECK_LUA_STRBUF_SHORT 40

typedef struct {
  char *p;     // where the bytes go
  size_t n;    // the size init was given
  char small[PICODECK_LUA_STRBUF_SHORT];
} picodeck_lua_strbuf_t;

// Reserves an n-byte string result and returns where to write it. Raises
// (memory error) if the string cannot be allocated, like luaL_buffinitsize.
char *picodeck_lua_strbuf_init(lua_State *L, picodeck_lua_strbuf_t *sb,
                               size_t n);

// Completes the result with its first `used` bytes (clamped to n) and leaves
// it in the slot init pushed.
void picodeck_lua_strbuf_push(lua_State *L, picodeck_lua_strbuf_t *sb,
                              size_t used);
