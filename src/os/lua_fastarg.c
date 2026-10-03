// See lua_fastarg.h. Compiled into the Lua library (PICODECK_LUA_SOURCES in
// cmake/picodeck_lua.cmake) because it uses VM internals.
#include "lua_fastarg.h"

#include "lobject.h"
#include "lstate.h"
#include "ltable.h"

// index2value (lapi.c) for stack indices only: NULL for a pseudo-index or an
// index outside the frame (where index2value would hand back the nil
// sentinel), so the caller takes the standard path.
static const TValue *arg_value(lua_State *L, int idx) {
  CallInfo *ci = L->ci;
  if (idx > 0) {
    StkId o = ci->func.p + idx;
    return o < L->top.p ? s2v(o) : NULL;
  }
  if (idx < 0 && idx > LUA_REGISTRYINDEX && -idx <= L->top.p - (ci->func.p + 1))
    return s2v(L->top.p + idx);
  return NULL;
}

void *picodeck_lua_udata_with_mt(lua_State *L, int idx, const void *mt) {
  const TValue *o = arg_value(L, idx);
  if (o == NULL || !ttisfulluserdata(o)) return NULL;
  Udata *u = uvalue(o);
  if ((const void *)u->metatable != mt || mt == NULL) return NULL;
  return getudatamem(u);
}

int picodeck_lua_tointeger_strict(lua_State *L, int idx, lua_Integer *out) {
  const TValue *o = arg_value(L, idx);
  if (o == NULL || !ttisinteger(o)) return 0;
  *out = ivalue(o);
  return 1;
}

void picodeck_lua_array_of(lua_State *L, int idx, picodeck_lua_array_t *a) {
  const TValue *o = arg_value(L, idx);
  if (o != NULL && ttistable(o)) {
    Table *t = hvalue(o);
    a->slots = t->array;
    a->size = luaH_realasize(t);
  } else {
    a->slots = NULL;
    a->size = 0;
  }
}

// Key i's slot, or NULL outside the array part.
static TValue *array_slot(const picodeck_lua_array_t *a, lua_Integer i) {
  if ((lua_Unsigned)i - 1u >= (lua_Unsigned)a->size) return NULL;
  return &((TValue *)a->slots)[i - 1];
}

int picodeck_lua_array_getnum(const picodeck_lua_array_t *a, lua_Integer i,
                              float *out) {
  const TValue *v = array_slot(a, i);
  if (v == NULL) return 0;
  if (ttisfloat(v)) {
    *out = fltvalue(v);
    return 1;
  }
  if (ttisinteger(v)) {
    *out = cast_num(ivalue(v));
    return 1;
  }
  return 0;
}

int picodeck_lua_array_getint(const picodeck_lua_array_t *a, lua_Integer i,
                              lua_Integer *out) {
  const TValue *v = array_slot(a, i);
  if (v == NULL || !ttisinteger(v)) return 0;
  *out = ivalue(v);
  return 1;
}

int picodeck_lua_array_setnum(const picodeck_lua_array_t *a, lua_Integer i,
                              float v) {
  TValue *s = array_slot(a, i);
  if (s == NULL) return 0;
  setfltvalue(s, v);
  return 1;
}

int picodeck_lua_array_setint(const picodeck_lua_array_t *a, lua_Integer i,
                              lua_Integer v) {
  TValue *s = array_slot(a, i);
  if (s == NULL) return 0;
  setivalue(s, v);
  return 1;
}

int picodeck_lua_array_setnil(const picodeck_lua_array_t *a, lua_Integer i) {
  TValue *s = array_slot(a, i);
  if (s == NULL) return 0;
  setnilvalue(s);
  return 1;
}
