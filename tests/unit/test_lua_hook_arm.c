// picodeck_lua_arm_hook (src/os/lua_hook_arm.c): the firmware's 1 ms timer
// arms the Lua service hook with it from an interrupt (lua_bridge.c), so it
// must cost the same at any call depth and still make the hook fire soon.
// Stock lua_sethook walked every frame on every tick, and ~1000 frames deep
// on the device that took longer than the tick: the timer IRQ re-fired
// forever and the watchdog reset the device (issue #21).
//
// Built against the real Lua core with the device's VM configuration
// (cmake/picodeck_lua.cmake). The VM sets trap flags itself (a stack
// reallocation traps every Lua frame so each luaV_execute reloads its base),
// so just before arming the test marks every Lua frame's trap with 2: any
// non-zero value means "check", so that changes nothing for the VM, and a 1
// afterwards is a frame the arm wrote.
#include "check.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "lstate.h"  // VM internals: CallInfo, isLua, the trap flag

#include "lua_hook_arm.h"

#include <stdlib.h>

static int s_fired;        // hook calls
static int s_fired_depth;  // Lua frames on the chain when it ran
static int s_armed_depth;  // Lua frames on the chain when it was armed
static int s_trapped;      // Lua frames the arm set the trap of

static int lua_frames(lua_State *L) {
  int n = 0;
  for (CallInfo *ci = L->ci; ci != NULL; ci = ci->previous)
    if (isLua(ci)) n++;
  return n;
}

#define TRAP_UNTOUCHED 2

static void mark_traps(lua_State *L) {
  for (CallInfo *ci = L->ci; ci != NULL; ci = ci->previous)
    if (isLua(ci)) ci->u.l.trap = TRAP_UNTOUCHED;
}

static int trapped_frames(lua_State *L) {
  int n = 0;
  for (CallInfo *ci = L->ci; ci != NULL; ci = ci->previous)
    if (isLua(ci) && ci->u.l.trap == 1) n++;
  return n;
}

// The service hook's shape (lua_bridge.c menu_lua_hook): disarm, then work.
static void service_hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  lua_sethook(L, NULL, 0, 0);
  s_fired++;
  s_fired_depth = lua_frames(L);
}

static void arm_now(lua_State *L) {
  mark_traps(L);
  picodeck_lua_arm_hook(L, service_hook);
  s_trapped = trapped_frames(L);
  s_armed_depth = lua_frames(L);
}

static int l_arm(lua_State *L) {
  arm_now(L);
  return 0;
}

// Arms from inside the allocator once the chain is `arm_depth` Lua
// frames deep: the VM is between two instructions of a Lua frame there (it is
// allocating that frame's callee's CallInfo or growing the stack), which is
// what the timer interrupt sees.
typedef struct {
  lua_State *L;
  int arm_depth;  // 0: never
  int armed;
} alloc_state_t;

static void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  alloc_state_t *st = (alloc_state_t *)ud;
  (void)osize;
  if (st->L && st->arm_depth && !st->armed &&
      lua_frames(st->L) >= st->arm_depth) {
    st->armed = 1;
    arm_now(st->L);
  }
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, nsize);
}

static lua_State *new_state(alloc_state_t *st) {
  lua_State *L = lua_newstate(test_alloc, st);
  st->L = L;
  luaL_openlibs(L);
  lua_register(L, "arm", l_arm);
  s_fired = s_fired_depth = s_armed_depth = s_trapped = 0;
  return L;
}

static void run(lua_State *L, const char *code) {
  if (luaL_dostring(L, code) != LUA_OK) {
    printf("Lua error: %s\n", lua_tostring(L, -1));
    CHECK(0);
  }
}

// 150 frames deep (the device's LUAI_MAXSTACK of 1000 slots allows ~900 of
// the smallest frames; depth only has to tell O(1) from O(depth)).
#define DEPTH 150

static const char *DIVE =
    "local function dive(n, at_bottom)\n"
    "  if n == 0 then\n"
    "    at_bottom()\n"
    "    local x = 0\n"
    "    for i = 1, 10 do x = x + i end\n"
    "    return x\n"
    "  end\n"
    "  return 1 + dive(n - 1, at_bottom)\n"
    "end\n";

// Armed from a C function at the bottom of the recursion: only the innermost
// Lua frame is trapped, and the hook fires there, before the dive unwinds.
static void test_arm_from_c_at_depth(void) {
  alloc_state_t st = {0};
  lua_State *L = new_state(&st);
  char code[512];
  snprintf(code, sizeof(code), "%s dive(%d, arm)", DIVE, DEPTH);
  run(L, code);
  CHECK(s_armed_depth > DEPTH);
  CHECK_EQ_INT(s_trapped, 1);
  CHECK_EQ_INT(s_fired, 1);
  CHECK_EQ_INT(s_fired_depth, s_armed_depth);
  lua_close(L);
}

// Armed while a Lua frame is running (from the allocator, as the timer
// interrupt would): one frame trapped, and the hook fires before the dive
// returns past the armed frame.
static void test_arm_inside_running_lua_frame(void) {
  alloc_state_t st = {0};
  lua_State *L = new_state(&st);
  st.arm_depth = DEPTH;
  char code[512];
  snprintf(code, sizeof(code), "%s dive(%d, function() end)", DIVE, DEPTH + 20);
  run(L, code);
  CHECK_EQ_INT(st.armed, 1);
  CHECK(s_trapped <= 1);
  CHECK_EQ_INT(s_fired, 1);
  CHECK(s_fired_depth >= s_armed_depth);
  lua_close(L);
}

// C frames above the innermost Lua frame (arm runs inside pcall inside
// pcall): the walk passes them, traps the Lua frame, and the hook fires when
// the pcalls return to it.
static void test_arm_through_c_frames(void) {
  alloc_state_t st = {0};
  lua_State *L = new_state(&st);
  char code[512];
  snprintf(code, sizeof(code),
           "%s dive(%d, function() pcall(pcall, arm) end)", DIVE, DEPTH);
  run(L, code);
  CHECK_EQ_INT(s_trapped, 1);
  CHECK_EQ_INT(s_fired, 1);
  CHECK(s_fired_depth >= s_armed_depth);
  lua_close(L);
}

// More C frames than the walk examines: no frame is trapped, yet the hook
// still fires at the next Lua call (luaV_execute reads L->hookmask on entry).
static void test_arm_beyond_frame_cap(void) {
  alloc_state_t st = {0};
  lua_State *L = new_state(&st);
  char code[512];
  snprintf(code, sizeof(code),
           "local f = {}\n"
           "for i = 1, %d do f[i] = pcall end\n"
           "f[#f + 1] = arm\n"
           "pcall(table.unpack(f))\n"
           "local function noop() end\n"
           "noop()\n",
           PICODECK_LUA_ARM_MAX_FRAMES + 4);
  run(L, code);
  CHECK_EQ_INT(s_trapped, 0);
  CHECK_EQ_INT(s_fired, 1);
  lua_close(L);
}

// The hook state is lua_sethook(L, hook, LUA_MASKCOUNT, 1)'s.
static void test_hook_state_matches_sethook(void) {
  alloc_state_t st = {0};
  lua_State *L = new_state(&st);
  picodeck_lua_arm_hook(L, service_hook);
  CHECK(lua_gethook(L) == service_hook);
  CHECK_EQ_INT(lua_gethookmask(L), LUA_MASKCOUNT);
  CHECK_EQ_INT(lua_gethookcount(L), 1);
  lua_sethook(L, NULL, 0, 0);
  lua_close(L);
}

int main(void) {
  test_arm_from_c_at_depth();
  test_arm_inside_running_lua_frame();
  test_arm_through_c_frames();
  test_arm_beyond_frame_cap();
  test_hook_state_matches_sethook();
  return check_report("test_lua_hook_arm");
}
