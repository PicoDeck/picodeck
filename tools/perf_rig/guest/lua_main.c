// The Lua workload: the real Lua core with the device's defines, one empty
// for-loop, one FORLOOP bytecode per iteration. The device figure to compare
// with is ~180 ns an iteration at 200 MHz. The VM runs hook-free, as on firmware.
#include "lauxlib.h"
#include "lua.h"
#include "rig.h"
#include <stddef.h>
#include <string.h>

extern char __heap_base[];
static char *s_heap;

// A bump allocator in QMI PSRAM. Nothing is reused, which is fine for one short
// chunk; the loop allocates nothing.
static void *rig_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  if (nsize == 0) return NULL;
  if (ptr && nsize <= osize) return ptr;
  char *p = s_heap;
  s_heap += (nsize + 15) & ~(size_t)7;
  if (ptr) memcpy(p, ptr, osize);
  return p;
}

// lua_numfmt.c's entry, which the device's luaconf.h patch calls: only used for
// number-to-string conversions, and this chunk performs none.
int picodeck_lua_sprintf(char *buf, size_t n, const char *fmt, ...) {
  (void)n;
  (void)fmt;
  buf[0] = 0;
  return 0;
}

int main(void) {
  s_heap = __heap_base;
  lua_State *L = lua_newstate(rig_lua_alloc, NULL);
  if (!L || luaL_loadstring(L, "for i = 1, " RIG_LUA_ITERS " do end")) {
    RIG_STAT(0, 1);
    return 0;
  }
  RIG_PHASE(PHASE_GEOM);
  const int err = lua_pcall(L, 0, 0, 0);
  RIG_PHASE(PHASE_IDLE);
  RIG_STAT(0, (uint32_t)err);
  return 0;
}
