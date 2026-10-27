// picodeck_lua_udata_with_mt / picodeck_lua_tointeger_strict
// (src/os/lua_fastarg.c): the bridge's one-call argument checks. Each must
// answer exactly the common case it claims (a full userdata with that
// metatable; a Lua integer) and say "no" to everything else, including
// indices it does not handle, so the caller's standard path decides. Built
// against the real Lua core with the device's VM configuration.
#include "check.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "lua_fastarg.h"

static const void *new_type(lua_State *L, const char *name) {
  luaL_newmetatable(L, name);
  const void *mt = lua_topointer(L, -1);
  lua_pop(L, 1);
  return mt;
}

static void test_udata(void) {
  lua_State *L = luaL_newstate();
  const void *mt_a = new_type(L, "test.a");
  const void *mt_b = new_type(L, "test.b");

  int *pa = lua_newuserdatauv(L, sizeof(int), 0);   // 1: a
  luaL_setmetatable(L, "test.a");
  lua_newuserdatauv(L, sizeof(int), 0);              // 2: b
  luaL_setmetatable(L, "test.b");
  lua_newuserdatauv(L, sizeof(int), 0);              // 3: no metatable
  static int x;
  lua_pushlightuserdata(L, &x);                      // 4: light
  lua_newtable(L);                                   // 5: a table with mt a
  luaL_setmetatable(L, "test.a");
  lua_pushinteger(L, 7);                             // 6

  CHECK(picodeck_lua_udata_with_mt(L, 1, mt_a) == pa);
  CHECK(picodeck_lua_udata_with_mt(L, 1, mt_a) == luaL_testudata(L, 1, "test.a"));
  CHECK(picodeck_lua_udata_with_mt(L, -6, mt_a) == pa);   // negative index
  CHECK(picodeck_lua_udata_with_mt(L, 1, mt_b) == NULL);
  CHECK(picodeck_lua_udata_with_mt(L, 2, mt_a) == NULL);
  CHECK(picodeck_lua_udata_with_mt(L, 2, mt_b) != NULL);
  CHECK(picodeck_lua_udata_with_mt(L, 3, mt_a) == NULL);
  CHECK(picodeck_lua_udata_with_mt(L, 3, NULL) == NULL);   // no mt != NULL mt
  CHECK(picodeck_lua_udata_with_mt(L, 4, mt_a) == NULL);
  CHECK(picodeck_lua_udata_with_mt(L, 5, mt_a) == NULL);   // a table, not udata
  CHECK(picodeck_lua_udata_with_mt(L, 6, mt_a) == NULL);
  CHECK(picodeck_lua_udata_with_mt(L, 7, mt_a) == NULL);   // past the top
  CHECK(picodeck_lua_udata_with_mt(L, 100, mt_a) == NULL);
  CHECK(picodeck_lua_udata_with_mt(L, -7, mt_a) == NULL);  // below the frame
  CHECK(picodeck_lua_udata_with_mt(L, LUA_REGISTRYINDEX, mt_a) == NULL);
  CHECK(picodeck_lua_udata_with_mt(L, 0, mt_a) == NULL);
  lua_close(L);
}

// Inside a C function: indices are relative to its frame, and an upvalue
// pseudo-index is never answered (the caller's standard path handles it).
static int probe(lua_State *L) {
  const void *mt = lua_touserdata(L, lua_upvalueindex(2));
  void *p = lua_touserdata(L, 1);
  lua_Integer v = 0;
  CHECK(picodeck_lua_udata_with_mt(L, 1, mt) == p);
  CHECK(picodeck_lua_udata_with_mt(L, lua_upvalueindex(1), mt) == NULL);
  CHECK(picodeck_lua_tointeger_strict(L, 2, &v) && v == 42);
  CHECK(!picodeck_lua_tointeger_strict(L, 3, &v));  // past this frame's args
  CHECK(!picodeck_lua_tointeger_strict(L, lua_upvalueindex(1), &v));
  return 0;
}

static void test_in_c_frame(void) {
  lua_State *L = luaL_newstate();
  const void *mt = new_type(L, "test.c");
  lua_pushinteger(L, 5);  // below the frame: index 1 must not see it
  lua_newuserdatauv(L, 4, 0);
  luaL_setmetatable(L, "test.c");      // upvalue 1: an object
  lua_pushlightuserdata(L, (void *)mt);  // upvalue 2: the mt pointer
  lua_pushcclosure(L, probe, 2);
  lua_newuserdatauv(L, 4, 0);
  luaL_setmetatable(L, "test.c");
  lua_pushinteger(L, 42);
  CHECK(lua_pcall(L, 2, 0, 0) == LUA_OK);
  lua_close(L);
}

static void test_integers(void) {
  lua_State *L = luaL_newstate();
  lua_pushinteger(L, 0);            // 1
  lua_pushinteger(L, -2147483647 - 1);  // 2
  lua_pushinteger(L, 2147483647);   // 3
  lua_pushnumber(L, 3.0f);          // 4: integral float
  lua_pushnumber(L, 3.5f);          // 5
  lua_pushliteral(L, "7");          // 6: numeric string
  lua_pushnil(L);                   // 7
  lua_pushboolean(L, 1);            // 8
  lua_Integer v = 99;
  CHECK(picodeck_lua_tointeger_strict(L, 1, &v) && v == 0);
  CHECK(picodeck_lua_tointeger_strict(L, 2, &v) && v == -2147483647 - 1);
  CHECK(picodeck_lua_tointeger_strict(L, 3, &v) && v == 2147483647);
  CHECK(picodeck_lua_tointeger_strict(L, -6, &v) && v == 2147483647);
  v = 99;
  CHECK(!picodeck_lua_tointeger_strict(L, 4, &v) && v == 99);  // untouched
  CHECK(!picodeck_lua_tointeger_strict(L, 5, &v));
  CHECK(!picodeck_lua_tointeger_strict(L, 6, &v));
  CHECK(!picodeck_lua_tointeger_strict(L, 7, &v));
  CHECK(!picodeck_lua_tointeger_strict(L, 8, &v));
  CHECK(!picodeck_lua_tointeger_strict(L, 9, &v));    // past the top
  CHECK(!picodeck_lua_tointeger_strict(L, -9, &v));   // below the frame
  CHECK(!picodeck_lua_tointeger_strict(L, LUA_REGISTRYINDEX, &v));
  CHECK(v == 99);
  lua_close(L);
}

// Each array-part getter agrees with lua_rawgeti + lua_tonumber/tointeger
// wherever it answers, and declines (0) for strings, booleans, nil, keys
// outside the array part and keys in the hash part.
static void test_array_reads(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  CHECK(luaL_dostring(L, "t = {1, 2.5, '3', nil, true, 7, 2^31 - 1, -0.0}\n"
                         "t[100] = 9; t.k = 1") == LUA_OK);
  lua_getglobal(L, "t");
  picodeck_lua_array_t a;
  picodeck_lua_array_of(L, -1, &a);
  CHECK(a.size >= 8);
  int answered = 0;
  for (lua_Integer i = -1; i <= 101; i++) {
    float f;
    lua_Integer n;
    lua_rawgeti(L, -1, i);
    int t = lua_type(L, -1);
    float want_f = (float)lua_tonumber(L, -1);
    int isint = lua_isinteger(L, -1);
    lua_Integer want_i = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (picodeck_lua_array_getnum(&a, i, &f)) {
      answered++;
      CHECK(t == LUA_TNUMBER);
      CHECK(f == want_f);
    } else {
      CHECK(t != LUA_TNUMBER || i < 1 || (unsigned)i > a.size);
    }
    if (picodeck_lua_array_getint(&a, i, &n)) {
      CHECK(isint && n == want_i);
    }
  }
  CHECK(answered == 5);  // 1, 2.5, 7, 2^31-1 (a float here), -0.0
  float f;
  CHECK(!picodeck_lua_array_getnum(&a, 100, &f));  // hash part: declined
  lua_pushinteger(L, 3);
  picodeck_lua_array_of(L, -1, &a);  // not a table
  CHECK(a.size == 0 && !picodeck_lua_array_getnum(&a, 1, &f));
  lua_close(L);
}

// Writes land as lua_rawseti would leave them: values, subtypes, the length
// operator after nils at the tail, and a full collection afterwards.
static void test_array_writes(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  CHECK(luaL_dostring(L, "t = {} for i = 1, 12 do t[i] = i end") == LUA_OK);
  lua_getglobal(L, "t");
  picodeck_lua_array_t a;
  picodeck_lua_array_of(L, -1, &a);
  CHECK(a.size >= 12);
  CHECK(picodeck_lua_array_setnum(&a, 1, 1.5f));
  CHECK(picodeck_lua_array_setint(&a, 2, -7));
  for (lua_Integer i = 7; i <= 12; i++) CHECK(picodeck_lua_array_setnil(&a, i));
  CHECK(!picodeck_lua_array_setnum(&a, 0, 1.0f));
  CHECK(!picodeck_lua_array_setnum(&a, (lua_Integer)a.size + 1, 1.0f));
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  CHECK(luaL_dostring(L,
      "assert(t[1] == 1.5 and math.type(t[1]) == 'float')\n"
      "assert(t[2] == -7 and math.type(t[2]) == 'integer')\n"
      "assert(t[6] == 6 and t[7] == nil and #t == 6)\n"
      "t[7] = 'x'; assert(#t == 7)\n") == LUA_OK);
  lua_close(L);
}

int main(void) {
  test_udata();
  test_array_reads();
  test_array_writes();
  test_in_c_frame();
  test_integers();
  return check_report("test_lua_fastarg");
}
