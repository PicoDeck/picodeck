// picodeck_lua_strbuf (src/os/lua_strbuf.c): a string result built in place,
// which the bulk bridge calls (fb:getPixels, fs.read, ...) return. Built
// against the real Lua core with the device's VM configuration
// (cmake/picodeck_lua.cmake), since a long string written in place has to
// behave as one Lua made: equal by content, usable as a table key, and short
// results interned so they compare by identity.
#include "check.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "lua_strbuf.h"

#include <stdlib.h>
#include <string.h>

static void fill(char *p, size_t n, unsigned seed) {
  for (size_t i = 0; i < n; i++) p[i] = (char)(seed + i * 7u + (i >> 8));
}

// The same bytes as fill(), made by Lua itself.
static void push_reference(lua_State *L, size_t n, unsigned seed) {
  char *ref = malloc(n ? n : 1);
  fill(ref, n, seed);
  lua_pushlstring(L, ref, n);
  free(ref);
}

// Builds an n-byte result, keeps `used` of it, and checks it against the
// string Lua makes from the same bytes: length, bytes, equality (raw and
// metamethod-free), and lookup as a table key in both directions.
static void check_result(lua_State *L, size_t n, size_t used) {
  int top = lua_gettop(L);
  picodeck_lua_strbuf_t sb;
  char *p = picodeck_lua_strbuf_init(L, &sb, n);
  CHECK(lua_gettop(L) == top + 1);
  fill(p, n, (unsigned)(n + used));
  picodeck_lua_strbuf_push(L, &sb, used);
  CHECK(lua_gettop(L) == top + 1);
  size_t want = used < n ? used : n;
  CHECK(lua_type(L, -1) == LUA_TSTRING);
  size_t len = 0;
  const char *s = lua_tolstring(L, -1, &len);
  CHECK(len == want);
  CHECK(s[len] == '\0');

  push_reference(L, want, (unsigned)(n + used));
  CHECK(lua_rawequal(L, -1, -2));
  CHECK(memcmp(lua_tostring(L, -1), s, want) == 0);

  // As a key: set through the reference, read back through the result, and
  // the other way round in a second table.
  lua_newtable(L);
  lua_pushvalue(L, -2);
  lua_pushinteger(L, 11);
  lua_rawset(L, -3);
  lua_pushvalue(L, -3);
  CHECK(lua_rawget(L, -2) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 11);
  lua_pop(L, 2);

  lua_newtable(L);
  lua_pushvalue(L, -3);
  lua_pushinteger(L, 22);
  lua_rawset(L, -3);
  lua_pushvalue(L, -2);
  CHECK(lua_rawget(L, -2) == LUA_TNUMBER);
  CHECK(lua_tointeger(L, -1) == 22);
  lua_pop(L, 2);

  lua_pop(L, 2);
  CHECK(lua_gettop(L) == top);
}

static void test_sizes(void) {
  lua_State *L = luaL_newstate();
  const size_t sizes[] = {0, 1, 2, 39, 40, 41, 42, 255, 256, 257,
                          4096, 17280, 153600};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    check_result(L, sizes[i], sizes[i]);
  lua_close(L);
}

// Fewer bytes than reserved: a long reservation cut down to a short result
// must come back interned; one cut to a long result just shorter.
static void test_short_reads(void) {
  lua_State *L = luaL_newstate();
  check_result(L, 100, 0);
  check_result(L, 100, 1);
  check_result(L, 100, 40);
  check_result(L, 100, 41);
  check_result(L, 100, 99);
  check_result(L, 40, 12);
  check_result(L, 5000, 4999);
  check_result(L, 10, 50);    // used past n is clamped
  check_result(L, 300, 900);
  lua_close(L);
}

// A short result is interned: the same object as the literal, so it works as
// a key that a Lua-made string finds (identity comparison).
static void test_short_interned(void) {
  lua_State *L = luaL_newstate();
  picodeck_lua_strbuf_t sb;
  char *p = picodeck_lua_strbuf_init(L, &sb, 5);
  memcpy(p, "hello", 5);
  picodeck_lua_strbuf_push(L, &sb, 5);
  lua_pushliteral(L, "hello");
  CHECK(lua_topointer(L, -1) == lua_topointer(L, -2));
  lua_close(L);
}

// The collector runs (fully, and in steps) while the reservation is open: the
// string must survive it, and keep the bytes written before and after.
static void test_gc_while_open(void) {
  lua_State *L = luaL_newstate();
  for (int mode = 0; mode < 2; mode++) {
    lua_gc(L, mode ? LUA_GCGEN : LUA_GCINC, 0, 0);
    picodeck_lua_strbuf_t sb;
    const size_t n = 20000;
    char *p = picodeck_lua_strbuf_init(L, &sb, n);
    fill(p, n / 2, 3);
    for (int i = 0; i < 200; i++) {  // garbage to collect
      lua_createtable(L, 64, 0);
      lua_pop(L, 1);
    }
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCSTEP, 0);
    char *tmp = malloc(n);
    fill(tmp, n, 3);
    memcpy(p + n / 2, tmp + n / 2, n - n / 2);
    picodeck_lua_strbuf_push(L, &sb, n);
    CHECK(lua_rawlen(L, -1) == n);
    CHECK(memcmp(lua_tostring(L, -1), tmp, n) == 0);
    free(tmp);
    lua_pop(L, 1);
    lua_gc(L, LUA_GCCOLLECT);
  }
  lua_close(L);
}

// An error between init and push leaves only garbage: the slot unwinds with
// the C frame and the string is collected (ASan would see a leak or a bad
// free here; Lua's own allocator accounting would assert in lua_close).
static int raise_mid_build(lua_State *L) {
  picodeck_lua_strbuf_t sb;
  char *p = picodeck_lua_strbuf_init(L, &sb, (size_t)lua_tointeger(L, 1));
  p[0] = 'x';
  return luaL_error(L, "raised mid-build");
}

static void test_error_mid_build(void) {
  lua_State *L = luaL_newstate();
  const lua_Integer sizes[] = {3, 64, 30000};
  for (int i = 0; i < 3; i++) {
    lua_pushcfunction(L, raise_mid_build);
    lua_pushinteger(L, sizes[i]);
    CHECK(lua_pcall(L, 1, 0, 0) == LUA_ERRRUN);
    CHECK(strstr(lua_tostring(L, -1), "raised mid-build") != NULL);
    lua_pop(L, 1);
  }
  lua_gc(L, LUA_GCCOLLECT);
  CHECK(lua_gettop(L) == 0);
  lua_close(L);
}

// A result in use from Lua: string functions, concatenation and hashing see
// the in-place bytes.
static int make_str(lua_State *L) {
  size_t n = (size_t)luaL_checkinteger(L, 1);
  picodeck_lua_strbuf_t sb;
  char *p = picodeck_lua_strbuf_init(L, &sb, n);
  for (size_t i = 0; i < n; i++) p[i] = (char)('a' + i % 26);
  picodeck_lua_strbuf_push(L, &sb, n);
  return 1;
}

static void test_from_lua(void) {
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  lua_register(L, "make_str", make_str);
  const char *code =
      "local s = make_str(100)\n"
      "assert(#s == 100 and s:sub(1, 3) == 'abc' and s:byte(27) == 97)\n"
      "local r = string.rep('abcdefghijklmnopqrstuvwxyz', 4):sub(1, 100)\n"
      "assert(s == r)\n"
      "local t = {[r] = 1}\n"
      "assert(t[s] == 1)\n"
      "assert(make_str(3) == 'abc')\n"
      "local k = {abc = 5}\n"
      "assert(k[make_str(3)] == 5)\n"
      "assert(make_str(0) == '')\n"
      "assert(#(s .. make_str(41)) == 141)\n";
  if (luaL_dostring(L, code) != LUA_OK) {
    fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
    CHECK(0);
  }
  lua_close(L);
}

int main(void) {
  test_sizes();
  test_short_reads();
  test_short_interned();
  test_gc_while_open();
  test_error_mid_build();
  test_from_lua();
  return check_report("test_lua_strbuf");
}
