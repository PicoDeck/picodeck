#include "lua_bridge_internal.h"
#include "app_identity.h"
#include "appconfig.h"

// Per-app store: always the running app's own /data/<id>/config.json.  The id
// comes from app_identity (C), never from the APP_ID global, and the store is
// re-bound whenever it holds another id (app_identity unbinds it at every app
// start and exit), so app B never reads or saves app A's config.
static bool bind_current_app(void) {
    const app_identity_t *me = app_identity_current();
    if (!me)
        return false;
    const char *bound = g_api.appconfig->getAppId();
    if (!bound || strcmp(bound, me->id) != 0)
        g_api.appconfig->load(me->id);  // false = no file yet; still bound
    return true;
}

static int l_config_get(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    const char *fallback = lua_isnoneornil(L, 2) ? NULL : luaL_optstring(L, 2, NULL);

    if (!bind_current_app()) {
        lua_pushnil(L);
        return 1;
    }

    const char *val = g_api.appconfig->get(key, fallback);
    if (val)
        lua_pushstring(L, val);
    else
        lua_pushnil(L);
    return 1;
}

// set(key, value) -> true, or false and why (nothing stored; an existing key
// keeps its old value). nil or "" deletes the key.
static int l_config_set(lua_State *L) {
    size_t klen = 0, vlen = 0;
    const char *key = luaL_checklstring(L, 1, &klen);
    const char *val = lua_isnoneornil(L, 2) ? "" : luaL_checklstring(L, 2, &vlen);

    const char *why = NULL;
    if (!bind_current_app())
        why = "no app is running";
    else if (strlen(key) != klen)
        why = "key contains a NUL byte";
    else if (strlen(val) != vlen)
        why = "value contains a NUL byte";
    else {
        appconfig_status_t st = appconfig_put(key, val);
        if (st != APPCONFIG_OK)
            why = appconfig_status_text(st);
    }
    if (why) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, why);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_config_save(lua_State *L) {
    lua_pushboolean(L, bind_current_app() && g_api.appconfig->save());
    return 1;
}

static int l_config_clear(lua_State *L) {
    (void)L;
    if (bind_current_app())
        g_api.appconfig->clear();
    return 0;
}

static int l_config_reset(lua_State *L) {
    lua_pushboolean(L, bind_current_app() && g_api.appconfig->reset());
    return 1;
}

// Explicit reload from disk (drops unsaved changes).
static int l_config_load(lua_State *L) {
    const app_identity_t *me = app_identity_current();
    lua_pushboolean(L, me && g_api.appconfig->load(me->id));
    return 1;
}

static const luaL_Reg l_config_lib[] = {
    {"get",    l_config_get},
    {"set",    l_config_set},
    {"save",   l_config_save},
    {"load",   l_config_load},
    {"clear",  l_config_clear},
    {"reset",  l_config_reset},
    {NULL, NULL}
};

void lua_bridge_appconfig_init(lua_State *L) {
    // Per-app config (/data/<APP_ID>/config.json). Registered twice:
    // "config" is the historical name, "appconfig" matches the C API
    // (g_api.appconfig) and the docs. Both names alias the same store.
    register_subtable(L, "config", l_config_lib);
    register_subtable(L, "appconfig", l_config_lib);
}
