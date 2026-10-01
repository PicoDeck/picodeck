// See lua_hook_arm.h. Compiled into the Lua library (PICODECK_LUA_SOURCES in
// cmake/picodeck_lua.cmake) because it uses VM internals.
#include "lua_hook_arm.h"

#include "ldebug.h"
#include "lstate.h"

// The walk must always reach the innermost Lua frame (lua_hook_arm.h): the
// most C calls the VM nests (getCcalls stops below LUAI_MAXCCALLS/10*11,
// luaE_checkcstack), plus a C function called from Lua and the frame the
// interrupt may catch being set up (prepCallInfo), must fit in the cap.
_Static_assert(LUAI_MAXCCALLS / 10 * 11 + 2 <= PICODECK_LUA_ARM_MAX_FRAMES,
               "PICODECK_LUA_ARM_MAX_FRAMES must cover LUAI_MAXCCALLS");

void picodeck_lua_arm_hook(lua_State *L, lua_Hook hook) {
  // lua_sethook(L, hook, LUA_MASKCOUNT, 1), field for field and in its
  // order, up to its settraps(L->ci) (ldebug.c), which visits every frame.
  L->hook = hook;
  L->basehookcount = 1;
  resethookcount(L);
  L->hookmask = LUA_MASKCOUNT;
  // Only the innermost Lua frame: its trap is what the running luaV_execute
  // (or the one a running C function returns to) reads next. A C frame's
  // union holds C-call state instead of the trap, so only Lua frames get it.
  CallInfo *ci = L->ci;
  for (int n = 0; ci != NULL && n < PICODECK_LUA_ARM_MAX_FRAMES;
       ci = ci->previous, n++) {
    if (isLua(ci)) {
      ci->u.l.trap = 1;
      break;
    }
  }
}
