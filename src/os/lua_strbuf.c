// See lua_strbuf.h. Compiled into the Lua library (PICODECK_LUA_SOURCES in
// cmake/picodeck_lua.cmake) because it uses VM internals.
#include "lua_strbuf.h"

#include "lapi.h"
#include "lgc.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"

_Static_assert(PICODECK_LUA_STRBUF_SHORT == LUAI_MAXSHORTLEN,
               "PICODECK_LUA_STRBUF_SHORT must match the VM's LUAI_MAXSHORTLEN");

char *picodeck_lua_strbuf_init(lua_State *L, picodeck_lua_strbuf_t *sb,
                               size_t n) {
  sb->n = n;
  if (n <= PICODECK_LUA_STRBUF_SHORT) {
    lua_pushnil(L);  // the slot push fills
    sb->p = sb->small;
    return sb->p;
  }
  // lua_pushlstring for a long string (lapi.c, luaS_newlstr), minus the copy:
  // the same size check, then the object anchored on the stack before the
  // collector may run. Its bytes stay unwritten until the caller fills them;
  // the collector never reads a string's bytes, and its hash is computed on
  // first use as a table key, after push.
  if (n >= (MAX_SIZE - sizeof(TString)) / sizeof(char)) luaM_toobig(L);
  TString *ts = luaS_createlngstrobj(L, n);
  setsvalue2s(L, L->top.p, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  sb->p = getlngstr(ts);
  return sb->p;
}

void picodeck_lua_strbuf_push(lua_State *L, picodeck_lua_strbuf_t *sb,
                              size_t used) {
  if (used > sb->n) used = sb->n;
  if (sb->p != sb->small && used == sb->n) return;  // the slot holds it already
  // A short result (interned here), or a long one cut short. The long string
  // is still in the slot below, so its bytes stay alive while they are copied.
  lua_pushlstring(L, sb->p, used);
  lua_replace(L, -2);
}
