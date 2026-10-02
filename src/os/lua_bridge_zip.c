// lua_bridge_zip.c — picocalc.zip.extract() and picocalc.zip.list()
// Thin wrappers over the shared hardened ZIP engine (zip_util.c). Sandbox
// checks stay here; all miniz access, streaming and path hardening live in
// the engine.

#include "lua_bridge_zip.h"
#include "lua_bridge_internal.h"  // fs_sandbox_check, lb_register_type
#include "zip_util.h"

#include <string.h>

#ifndef PICODECK_SIMULATOR
#include "hardware/watchdog.h"
#endif

// ── Progress trampoline: pump the watchdog, then fire the Lua callback ───────

typedef struct {
    lua_State *L;
    bool       has_cb;  // Lua callback sitting at stack index 3
} lua_zip_progress_t;

static bool lua_zip_progress(int done, int total, const char *name, void *user) {
    (void)name;
    lua_zip_progress_t *p = (lua_zip_progress_t *)user;
#ifndef PICODECK_SIMULATOR
    // Extraction can far outlast the 10s watchdog window; the only other
    // feeder (sys->poll) never runs while we're inside this call.
    watchdog_update();
#endif
    if (p->has_cb) {
        lua_pushvalue(p->L, 3);  // the callback
        lua_pushinteger(p->L, done);
        lua_pushinteger(p->L, total);
        if (lua_pcall(p->L, 2, 0, 0) != LUA_OK)
            lua_pop(p->L, 1);  // errors in the callback are ignored
    }
    // An exit request (sys.exit() in the callback, exit_app) stops the
    // extraction; the app then exits from the next instruction.
    return !lua_bridge_exit_requested();
}

// ── picocalc.zip.extract(zip_path, dest_dir [, progress_fn]) → ok [, err] ───
static int l_zip_extract(lua_State *L) {
    const char *zip_path = luaL_checkstring(L, 1);
    const char *dest_dir = luaL_checkstring(L, 2);
    bool has_progress = lua_isfunction(L, 3);

    if (!fs_sandbox_check(L, zip_path, false)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "permission denied (source)");
        return 2;
    }
    if (!fs_sandbox_check(L, dest_dir, true)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "permission denied (destination)");
        return 2;
    }

    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err);
        return 2;
    }

    lua_zip_progress_t prog = { .L = L, .has_cb = has_progress };
    zip_extract_result_t result;
    bool ok = zip_reader_extract_all(&zr, dest_dir, NULL,
                                     lua_zip_progress, &prog, &result, err);
    zip_reader_close(&zr);

    if (!ok) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err);
        return 2;
    }

    lua_pushboolean(L, true);
    return 1;
}

// ── Read-in-place archive handles: picocalc.zip.open(path) ───────────────────
// Full userdata with a metatable (__gc + __close + closed-handle checks), so
// an archive frees itself when an app errors out mid-file.

#define ZIP_MT "picocalc.zip.archive"
#define ZIP_LUA_MAX_OPEN 4

typedef struct {
    zip_reader_t zr;    // inline: Lua never moves userdata memory
    bool         open;
} lua_zip_archive_t;

// Open-archive budget for the running app; reset in lua_bridge_zip_init.
static int s_lua_zip_open_count = 0;

static lua_zip_archive_t *check_archive(lua_State *L) {
    lua_zip_archive_t *ar =
        (lua_zip_archive_t *)luaL_checkudata(L, 1, ZIP_MT);
    if (!ar->open)
        luaL_error(L, "archive is closed");
    return ar;
}

static void archive_do_close(lua_zip_archive_t *ar) {
    if (ar->open) {
        zip_reader_close(&ar->zr);
        ar->open = false;
        s_lua_zip_open_count--;
    }
}

// Pushes an archive userdata (metatable set, not open yet): the reader lives
// inside it, so an error that unwinds past an open reader leaves it to __gc.
static lua_zip_archive_t *push_archive(lua_State *L) {
    lua_zip_archive_t *ar =
        (lua_zip_archive_t *)lua_newuserdatauv(L, sizeof(*ar), 0);
    ar->open = false;
    luaL_setmetatable(L, ZIP_MT);
    return ar;
}

// Pushes the {name, size, compressed_size} array of the archive's files.
static void push_entry_list(lua_State *L, zip_reader_t *zr) {
    int n = zip_reader_num_entries(zr);
    lua_createtable(L, n > 0 ? n : 0, 0);
    for (int i = 0; i < n; i++) {
        zip_entry_info_t info;
        if (!zip_reader_stat_index(zr, i, &info) || info.is_dir)
            continue;
        lua_createtable(L, 0, 3);
        lua_pushstring(L, info.name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)info.size);
        lua_setfield(L, -2, "size");
        lua_pushinteger(L, (lua_Integer)info.comp_size);
        lua_setfield(L, -2, "compressed_size");
        lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
    }
}

// ── picocalc.zip.list(zip_path) → array of {name, size, compressed_size} ─────
// The reader is held in a (transient) archive userdata: the table building
// below can raise (out of memory), and the archive's __gc then closes it.
static int l_zip_list(lua_State *L) {
    const char *zip_path = luaL_checkstring(L, 1);

    if (!fs_sandbox_check(L, zip_path, false)) {
        lua_pushnil(L);
        lua_pushstring(L, "permission denied");
        return 2;
    }

    lua_zip_archive_t *ar = push_archive(L);
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&ar->zr, zip_path, err)) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    ar->open = true;
    s_lua_zip_open_count++;  // transient; not limited by ZIP_LUA_MAX_OPEN

    push_entry_list(L, &ar->zr);
    archive_do_close(ar);
    return 1;
}

// ── zip.open(path) → archive [, err] ─────────────────────────────────────────
static int l_zip_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    if (!fs_sandbox_check(L, path, false)) {
        lua_pushnil(L);
        lua_pushstring(L, "permission denied");
        return 2;
    }
    if (s_lua_zip_open_count >= ZIP_LUA_MAX_OPEN) {
        lua_pushnil(L);
        lua_pushstring(L, "too many open archives (max 4)");
        return 2;
    }

    lua_zip_archive_t *ar = push_archive(L);

    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&ar->zr, path, err)) {
        lua_pop(L, 1);  // the userdata (never opened, __gc is a no-op)
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    ar->open = true;
    s_lua_zip_open_count++;
    return 1;
}

// ── ar:list() → array of {name, size, compressed_size} ───────────────────────
static int l_ar_list(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    push_entry_list(L, &ar->zr);
    return 1;
}

// ── ar:exists(name) → bool ───────────────────────────────────────────────────
static int l_ar_exists(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *name = luaL_checkstring(L, 2);
    lua_pushboolean(L, zip_reader_locate(&ar->zr, name) >= 0);
    return 1;
}

// ── ar:size(name) → bytes | nil ──────────────────────────────────────────────
static int l_ar_size(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *name = luaL_checkstring(L, 2);
    int idx = zip_reader_locate(&ar->zr, name);
    zip_entry_info_t info;
    if (idx < 0 || !zip_reader_stat_index(&ar->zr, idx, &info)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)info.size);
    return 1;
}

// ── ar:numEntries() → integer (files + directories) ──────────────────────────
static int l_ar_num_entries(lua_State *L) {
  lua_zip_archive_t *ar = check_archive(L);
  lua_pushinteger(L, zip_reader_num_entries(&ar->zr));
  return 1;
}

// ── ar:locate(name) → index | nil ────────────────────────────────────────────
// The name → index mapping the index-addressed calls below take.
static int l_ar_locate(lua_State *L) {
  lua_zip_archive_t *ar = check_archive(L);
  const char *name = luaL_checkstring(L, 2);
  int idx = zip_reader_locate(&ar->zr, name);
  if (idx < 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, idx);
  return 1;
}

// ── ar:statIndex(idx) → {name, size, compressed_size, is_dir} | nil ──────────
// The metadata of one entry, keyed the way :list() reports it.
static int l_ar_stat_index(lua_State *L) {
  lua_zip_archive_t *ar = check_archive(L);
  lua_Integer idx = lb_checkint(L, 2);
  zip_entry_info_t info;
  if (!zip_reader_stat_index(&ar->zr, (int)idx, &info)) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 4);
  lua_pushstring(L, info.name);
  lua_setfield(L, -2, "name");
  lua_pushinteger(L, (lua_Integer)info.size);
  lua_setfield(L, -2, "size");
  lua_pushinteger(L, (lua_Integer)info.comp_size);
  lua_setfield(L, -2, "compressed_size");
  lua_pushboolean(L, info.is_dir);
  lua_setfield(L, -2, "is_dir");
  return 1;
}

// ── ar:extractEntry(idx, dest_path) → ok [, err] ──────────────────────────────
// :extract() is this, addressed by entry name.
static int l_ar_extract_entry(lua_State *L) {
  lua_zip_archive_t *ar = check_archive(L);
  lua_Integer idx = lb_checkint(L, 2);
  const char *dest = luaL_checkstring(L, 3);

  if (!fs_sandbox_check(L, dest, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (destination)");
    return 2;
  }
  char err[ZIP_ERR_MAX];
  if (!zip_reader_extract_entry(&ar->zr, (int)idx, dest, err)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, err);
    return 2;
  }
  lua_pushboolean(L, true);
  return 1;
}

// ── ar:read(name [, max_len]) → string | nil, err ────────────────────────────
static int l_ar_read(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *name = luaL_checkstring(L, 2);
    lua_Integer max_len = luaL_optinteger(L, 3, 0);

    int idx = zip_reader_locate(&ar->zr, name);
    if (idx < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no such entry");
        return 2;
    }

    // Decompressed straight into the result string's buffer (no heap copy
    // to leak if an error unwinds). Same cap as
    // zip_reader_read_to_heap: ZIP_MAX_READ_MEM, or max_len when smaller.
    zip_entry_info_t info;
    if (!zip_reader_stat_index(&ar->zr, idx, &info)) {
        lua_pushnil(L);
        lua_pushstring(L, "bad zip");
        return 2;
    }
    size_t cap = ZIP_MAX_READ_MEM;
    if (max_len > 0 && (size_t)max_len < cap)
        cap = (size_t)max_len;
    if ((uint64_t)info.size > cap) {
        lua_pushnil(L);
        lua_pushstring(L, "size cap");
        return 2;
    }
    luaL_Buffer b;
    void *buf = luaL_buffinitsize(L, &b, (size_t)info.size);
    char err[ZIP_ERR_MAX];
    int n = zip_reader_read_to_buf(&ar->zr, idx, buf, (size_t)info.size, err);
    if (n < 0) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}

// ── ar:extract(name, dest_path) → ok [, err] ─────────────────────────────────
static int l_ar_extract(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *name = luaL_checkstring(L, 2);
    const char *dest = luaL_checkstring(L, 3);

    if (!fs_sandbox_check(L, dest, true)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "permission denied (destination)");
        return 2;
    }
    // Single-entry extraction to a caller-chosen path: the entry name only
    // selects the data, it never becomes a filesystem path, so no name
    // validation is needed here.
    int idx = zip_reader_locate(&ar->zr, name);
    if (idx < 0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "no such entry");
        return 2;
    }
    char err[ZIP_ERR_MAX];
    if (!zip_reader_extract_entry(&ar->zr, idx, dest, err)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

// ── ar:extractAll(dest_dir [, progress_fn]) → ok [, err] ─────────────────────
static int l_ar_extract_all(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *dest = luaL_checkstring(L, 2);
    bool has_progress = lua_isfunction(L, 3);

    if (!fs_sandbox_check(L, dest, true)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "permission denied (destination)");
        return 2;
    }

    lua_zip_progress_t prog = { .L = L, .has_cb = has_progress };
    zip_extract_result_t result;
    char err[ZIP_ERR_MAX];
    bool ok = zip_reader_extract_all(&ar->zr, dest, NULL,
                                     lua_zip_progress, &prog, &result, err);
    if (!ok) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

// ── ar:close() ───────────────────────────────────────────────────────────────
static int l_ar_close(lua_State *L) {
    lua_zip_archive_t *ar =
        (lua_zip_archive_t *)luaL_checkudata(L, 1, ZIP_MT);
    archive_do_close(ar);  // double-close is a no-op, not an error
    return 0;
}

// __gc / __close — release the SD file handle when the archive is collected
// or leaves a <close> scope.
static int l_ar_gc(lua_State *L) {
    lua_zip_archive_t *ar =
        (lua_zip_archive_t *)luaL_checkudata(L, 1, ZIP_MT);
    archive_do_close(ar);
    return 0;
}

static const luaL_Reg archive_methods[] = {
    {"list",        l_ar_list},
    {"exists",      l_ar_exists},
    {"size",        l_ar_size},
    {"numEntries",  l_ar_num_entries},
    {"locate",      l_ar_locate},
    {"statIndex",   l_ar_stat_index},
    {"extractEntry",l_ar_extract_entry},
    {"read",        l_ar_read},
    {"extract",    l_ar_extract},
    {"extractAll", l_ar_extract_all},
    {"close",      l_ar_close},
    {NULL, NULL}
};

// ── Module registration ──────────────────────────────────────────────────────

static const luaL_Reg zip_funcs[] = {
    {"extract", l_zip_extract},
    {"list",    l_zip_list},
    {"open",    l_zip_open},
    {NULL, NULL}
};

void lua_bridge_zip_init(lua_State *L) {
    // Fresh per-app budget. Archives from the previous app were closed by
    // lua_close() running their __gc; this is the safety net.
    s_lua_zip_open_count = 0;

    static const luaL_Reg archive_meta[] = {
        {"__gc", l_ar_gc}, {"__close", l_ar_gc}, {NULL, NULL}};
    lb_register_type(L, ZIP_MT, archive_methods, archive_meta);

    // Assumes the `picocalc` table is on top of the stack
    lua_newtable(L);
    luaL_setfuncs(L, zip_funcs, 0);
    lua_setfield(L, -2, "zip");
}
