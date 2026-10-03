#include "lua_bridge_internal.h"

// ── picocalc.fs.* ────────────────────────────────────────────────────────────
// Thin wrapper over sdcard_ functions, exposed to Lua

#include "../drivers/sdcard.h"
#include "file_browser.h"
#include "app_identity.h"
#include "fs_path.h"

#include <limits.h>
#include <string.h>

// ── Filesystem sandbox ──────────────────────────────────────────────────────
// Apps may touch only (unless "root-filesystem" is granted):
//   /apps/<dir>/   — read-only (their own bundle)
//   /data/<id>/    — read + write (their own data directory)
//   /system/lib/   — read-only (shared libraries)
// Relative paths and any path containing ".." are always rejected.
//
// The identity and grants come from app_identity (set in C by the runner),
// never from the APP_DIR / APP_ID / APP_REQUIREMENTS globals, which the app
// can overwrite.  The rules themselves are fs_path_allowed() (fs_path.c,
// host-tested).

bool fs_sandbox_check(lua_State *L, const char *path, bool write) {
  (void)L;
  const app_identity_t *me = app_identity_current();
  if (!me)
    return fs_path_allowed(path, write, NULL, NULL, false);
  return fs_path_allowed(path, write, me->dir, me->data_dir, me->root_fs);
}

// ── File handles ────────────────────────────────────────────────────────────
// fs.open returns a full userdata (FS_FILE_MT) that owns the sdcard_fopen
// handle; it is never a raw pointer in Lua's hands:
//   - every handle function checks the type with lb_checkudata, so nil, a
//     light userdata or another module's userdata is a Lua error, not a FIL;
//   - close sets f to NULL, so a second close is a no-op and any other use
//     of a closed handle is a Lua error ("attempt to use a closed file");
//   - __gc and __close close a handle the app dropped or scoped with
//     `local f <close> = ...`;
//   - each open file is also on the running app's open-file list
//     (app_files_*), which lua_run sweeps after lua_close, so no handle
//     outlives its app (FatFS has only FF_FS_LOCK = 16 slots).
// The metatable is hidden (__metatable = false) and methods come from a
// separate table through __index, so h:read(n) works and h:__gc() does not
// exist.  fs.read(h, n) and h:read(n) are the same function.

#define FS_FILE_MT "picocalc.fs.file"

typedef struct {
  sdfile_t f;  // NULL once closed
} lua_fs_file_t;

// The open handle at idx, or a Lua error (wrong type, or closed).
static lua_fs_file_t *check_file(lua_State *L, int idx) {
  lua_fs_file_t *h = (lua_fs_file_t *)lb_checkudata(L, idx, FS_FILE_MT);
  if (!h->f)
    luaL_error(L, "attempt to use a closed file");
  return h;
}

// Close h once.  The fclose belongs to whoever untracks the handle: if the
// app-exit sweep (app_files_close_all) already closed it, do nothing.
static void fs_file_release(lua_fs_file_t *h) {
  sdfile_t f = h->f;
  if (!f)
    return;
  h->f = NULL;
  if (app_files_untrack(f))
    sdcard_fclose(f);
}

// fs.open(path [, mode]) -> handle | nil, err
static int l_fs_open(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  const char *mode = luaL_optstring(L, 2, "r");
  bool needs_write = (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL ||
                      strchr(mode, '+') != NULL);
  if (!fs_sandbox_check(L, path, needs_write)) {
    lua_pushnil(L);
    lua_pushliteral(L, "permission denied");
    return 2;
  }
  // The userdata first: if it raises (out of memory) nothing is open yet.
  lua_fs_file_t *h = (lua_fs_file_t *)lua_newuserdatauv(L, sizeof(*h), 0);
  h->f = NULL;
  luaL_setmetatable(L, FS_FILE_MT);
  sdfile_t f = sdcard_fopen(path, mode);
  if (!f) {
    lua_pushnil(L);
    lua_pushliteral(L, "cannot open file");
    return 2;
  }
  if (!app_files_track(f)) {
    sdcard_fclose(f);
    lua_pushnil(L);
    lua_pushliteral(L, "too many open files");
    return 2;
  }
  h->f = f;
  return 1;
}

// fs.read(h, len) / h:read(len) -> string, or nil at end of file / on error.
// len must be >= 0; it is clamped to the bytes left in the file, so an
// absurd length never becomes an allocation.  Reads straight into the string
// it returns (picodeck_lua_strbuf: no temporary umm_malloc to leak if an
// error unwinds, and no second copy).
static int l_fs_read(lua_State *L) {
  lua_fs_file_t *h = check_file(L, 1);
  lua_Integer want = luaL_checkinteger(L, 2);
  luaL_argcheck(L, want >= 0, 2, "negative length");
  int size = sdcard_fsize_handle(h->f);
  if (size >= 0) {
    uint32_t pos = sdcard_ftell(h->f);
    lua_Integer left = (uint32_t)size > pos ? (lua_Integer)((uint32_t)size - pos) : 0;
    if (want > left)
      want = left;
  }
  if (want == 0 || want > INT_MAX) {
    lua_pushnil(L);
    return 1;
  }
  picodeck_lua_strbuf_t sb;  // read straight into the result string
  char *buf = picodeck_lua_strbuf_init(L, &sb, (size_t)want);
  int n = sdcard_fread(h->f, buf, (int)want);
  if (n <= 0) {
    lua_pushnil(L);
    return 1;
  }
  picodeck_lua_strbuf_push(L, &sb, (size_t)n);
  return 1;
}

// fs.write(h, data) / h:write(data) -> bytes written (-1 on error)
static int l_fs_write(lua_State *L) {
  lua_fs_file_t *h = check_file(L, 1);
  size_t len;
  const char *data = luaL_checklstring(L, 2, &len);
  luaL_argcheck(L, len <= INT_MAX, 2, "data too large");
  lua_pushinteger(L, sdcard_fwrite(h->f, data, (int)len));
  return 1;
}

// fs.close(h) / h:close() — idempotent; fs.close(nil) is a no-op.
static int l_fs_close(lua_State *L) {
  if (lua_isnoneornil(L, 1))
    return 0;
  fs_file_release((lua_fs_file_t *)lb_checkudata(L, 1, FS_FILE_MT));
  return 0;
}

// fs.seek(h, offset [, whence]) / h:seek(...) -> ok.  whence is "set" (the
// default: offset from the start, must be >= 0), "cur" or "end" (offset from
// the current position / the end of the file, negative allowed).  The
// resulting position is computed in 64 bits and must lie in [0, INT_MAX];
// anything else is an argument error, never a wrapped seek.  Returns true on
// success (unchanged), false if the seek failed.  A position past the end is
// the SD driver's business: FatFS clamps a read handle to the end and extends
// a write handle.
static int l_fs_seek(lua_State *L) {
  static const char *const whences[] = {"set", "cur", "end", NULL};
  lua_fs_file_t *h = check_file(L, 1);
  lua_Integer offset = luaL_checkinteger(L, 2);
  int whence = luaL_checkoption(L, 3, "set", whences);
  int64_t base = 0;
  if (whence == 0) {
    luaL_argcheck(L, offset >= 0, 2, "negative offset");
  } else if (whence == 1) {
    base = (int64_t)sdcard_ftell(h->f);
  } else {
    int size = sdcard_fsize_handle(h->f);
    if (size < 0) {
      lua_pushboolean(L, false);
      return 1;
    }
    base = size;
  }
  int64_t target = base + (int64_t)offset;
  luaL_argcheck(L, target >= 0, 2, "position before start of file");
  luaL_argcheck(L, target <= INT_MAX, 2, "position out of range");
  lua_pushboolean(L, sdcard_fseek(h->f, (uint32_t)target));
  return 1;
}

static int l_fs_tell(lua_State *L) {
  lua_fs_file_t *h = check_file(L, 1);
  lua_pushinteger(L, (lua_Integer)sdcard_ftell(h->f));
  return 1;
}

// __gc and __close: close a handle the app dropped or scoped.
static int l_fs_file_gc(lua_State *L) {
  fs_file_release((lua_fs_file_t *)lb_checkudata(L, 1, FS_FILE_MT));
  return 0;
}

static int l_fs_file_tostring(lua_State *L) {
  lua_fs_file_t *h = (lua_fs_file_t *)lb_checkudata(L, 1, FS_FILE_MT);
  if (h->f)
    lua_pushfstring(L, "file (%p)", (void *)h);
  else
    lua_pushliteral(L, "file (closed)");
  return 1;
}

// ── file:fsize() / fs.fsize(f) → bytes | nil, err ────────────────────────────
// The size of an open handle, without moving the read position: :tell() after
// seek(0, "end") gets the same number but leaves the handle at EOF.
static int l_fs_fsize(lua_State *L) {
  lua_fs_file_t *h = check_file(L, 1);
  int n = sdcard_fsize_handle(h->f);
  if (n < 0) {
    lua_pushnil(L);
    lua_pushstring(L, "fsize failed");
    return 2;
  }
  lua_pushinteger(L, (lua_Integer)n);
  return 1;
}

static const luaL_Reg l_fs_file_methods[] = {
    {"read", l_fs_read},   {"write", l_fs_write}, {"close", l_fs_close},
    {"seek", l_fs_seek},   {"tell", l_fs_tell},   {"fsize", l_fs_fsize},
    {NULL, NULL}};

static const luaL_Reg l_fs_file_meta[] = {
    {"__gc", l_fs_file_gc},
    {"__close", l_fs_file_gc},
    {"__tostring", l_fs_file_tostring},
    {NULL, NULL}};

static void fs_file_register_mt(lua_State *L) {
  lb_register_type(L, FS_FILE_MT, l_fs_file_methods, l_fs_file_meta);
}

// The SD volume root ("/", "//", "/.", "/./"...: slashes and whole "."
// segments only). FatFS f_stat refuses it (the root has no
// directory entry) while the simulator's host stat() succeeds, so exists /
// stat / isDir answer for it here, identically on both: an existing, empty-
// sized directory. Only a root-filesystem app gets past fs_sandbox_check.
static bool path_is_root(const char *path) {
  if (path[0] != '/') return false;
  for (const char *c = path; *c;) {
    if (*c == '/') { c++; continue; }
    if (c[0] == '.' && (c[1] == '/' || c[1] == '\0')) { c++; continue; }
    return false;
  }
  return true;
}

static int l_fs_exists(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushboolean(L, false);
    return 1;
  }
  lua_pushboolean(L, path_is_root(path) || sdcard_fexists(path));
  return 1;
}

// fs.readFile(path) -> string | nil.  The Lua buffer is sized before the
// file is opened, so an out-of-memory error cannot leave the file open, and
// the bytes are read straight into it (no umm_malloc copy to leak).
static int l_fs_readFile(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushnil(L);
    return 1;
  }
  int size = sdcard_fsize(path);
  if (size < 0) {
    lua_pushnil(L);
    return 1;
  }
  picodeck_lua_strbuf_t sb;
  char *buf = picodeck_lua_strbuf_init(L, &sb, (size_t)size);
  sdfile_t f = sdcard_fopen(path, "rb");
  if (!f) {
    lua_pushnil(L);
    return 1;
  }
  int n = size > 0 ? sdcard_fread(f, buf, size) : 0;
  sdcard_fclose(f);
  if (n < 0) {
    lua_pushnil(L);
    return 1;
  }
  picodeck_lua_strbuf_push(L, &sb, (size_t)n);
  return 1;
}

static int l_fs_size(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushinteger(L, -1);
    return 1;
  }
  lua_pushinteger(L, sdcard_fsize(path));
  return 1;
}

// Context passed through sdcard_list_dir's void* user pointer
typedef struct {
  lua_State *L;
  int tidx;
  int n;
} listdir_ctx_t;

// Decode FatFS packed date/time fields into separate Lua table entries.
// fdate bits: [15:9]=year-1980, [8:5]=month, [4:0]=day
// ftime bits: [15:11]=hour, [10:5]=min, [4:0]=sec/2
static void push_mtime_fields(lua_State *L, uint16_t fdate, uint16_t ftime) {
  if (fdate == 0) return; // no timestamp stored
  lua_pushinteger(L, (fdate >> 9) + 1980); lua_setfield(L, -2, "year");
  lua_pushinteger(L, (fdate >> 5) & 0xF);  lua_setfield(L, -2, "month");
  lua_pushinteger(L, fdate & 0x1F);         lua_setfield(L, -2, "day");
  lua_pushinteger(L, ftime >> 11);           lua_setfield(L, -2, "hour");
  lua_pushinteger(L, (ftime >> 5) & 0x3F);  lua_setfield(L, -2, "min");
  lua_pushinteger(L, (ftime & 0x1F) * 2);   lua_setfield(L, -2, "sec");
}

static void listdir_cb(const sdcard_entry_t *e, void *user) {
  listdir_ctx_t *ctx = (listdir_ctx_t *)user;
  lua_State *L = ctx->L;
  lua_newtable(L);
  lua_pushstring(L, e->name);
  lua_setfield(L, -2, "name");
  lua_pushboolean(L, e->is_dir);
  lua_setfield(L, -2, "is_dir");
  lua_pushinteger(L, e->size);
  lua_setfield(L, -2, "size");
  push_mtime_fields(L, e->fdate, e->ftime);
  lua_rawseti(L, ctx->tidx, ++ctx->n);
}

// Returns an array of {name, is_dir, size} tables, or an empty table on error.
static int l_fs_listDir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  lua_newtable(L);
  if (!fs_sandbox_check(L, path, false))
    return 1; // return empty table
  listdir_ctx_t ctx = {L, lua_gettop(L), 0};
  sdcard_list_dir(path, listdir_cb, &ctx);
  return 1;
}

static int l_fs_mkdir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, true)) {
    lua_pushboolean(L, false);
    return 1;
  }
  lua_pushboolean(L, sdcard_mkdir(path));
  return 1;
}

// Convenience: return the path /data/<dirname>/<name>, auto-creating the
// data directory if it does not already exist.
// Usage: local path = picocalc.fs.appPath("save.json")
static int l_fs_appPath(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);

  const app_identity_t *me = app_identity_current();
  if (!me) {
    lua_pushnil(L);
    return 1;
  }

  // Auto-create /data/<id>/ on first call
  sdcard_mkdir(me->data_dir);

  char full_path[192];
  snprintf(full_path, sizeof(full_path), "%s/%s", me->data_dir, name);
  lua_pushstring(L, full_path);
  return 1;
}

// Open a file-browser panel overlay.
// Optional arg: start directory (defaults to the app's /data/<id>/ dir).
// Returns the selected file path as a string, or nil if cancelled.
static int l_fs_browse(lua_State *L) {
  // The app's data root is the browser's root boundary.
  const app_identity_t *me = app_identity_current();
  const char *root_path = "/data";
  if (me) {
    sdcard_mkdir(me->data_dir);
    root_path = me->data_dir;
  }

  // The start directory is listed, so it needs read access: a sandboxed
  // app asking for /system or another app's /data starts at its own root.
  const char *start_path =
      lua_isnoneornil(L, 1) ? root_path : luaL_checkstring(L, 1);
  if (start_path != root_path && !fs_sandbox_check(L, start_path, false))
    start_path = root_path;

  char selected[192];
  if (file_browser_show(start_path, root_path, selected, sizeof(selected))) {
    lua_pushstring(L, selected);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

// ── picocalc.fs.delete(path) → ok [, err] ────────────────────────────────────
static int l_fs_delete(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied");
    return 2;
  }
  if (sdcard_delete(path)) {
    lua_pushboolean(L, true);
    return 1;
  }
  lua_pushboolean(L, false);
  lua_pushstring(L, "delete failed");
  return 2;
}

// ── picocalc.fs.deleteRecursive(path) → ok [, err] ──────────────────────────
static int l_fs_delete_recursive(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied");
    return 2;
  }
  if (sdcard_delete_recursive(path)) {
    lua_pushboolean(L, true);
    return 1;
  }
  lua_pushboolean(L, false);
  lua_pushstring(L, "delete failed");
  return 2;
}

// ── picocalc.fs.rename(src, dst) → ok [, err] ────────────────────────────────
static int l_fs_rename(lua_State *L) {
  const char *src = luaL_checkstring(L, 1);
  const char *dst = luaL_checkstring(L, 2);
  if (!fs_sandbox_check(L, src, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (source)");
    return 2;
  }
  if (!fs_sandbox_check(L, dst, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (destination)");
    return 2;
  }
  if (sdcard_rename(src, dst)) {
    lua_pushboolean(L, true);
    return 1;
  }
  lua_pushboolean(L, false);
  lua_pushstring(L, "rename failed");
  return 2;
}

// ── picocalc.fs.copy(src, dst [, progress_fn]) → ok [, err] ─────────────────
typedef struct { lua_State *L; int fn_ref; } copy_progress_ctx_t;

static void copy_progress_trampoline(uint32_t done, uint32_t total, void *user) {
  copy_progress_ctx_t *ctx = (copy_progress_ctx_t *)user;
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->fn_ref);
  lua_pushinteger(ctx->L, (lua_Integer)done);
  lua_pushinteger(ctx->L, (lua_Integer)total);
  if (lua_pcall(ctx->L, 2, 0, 0) != LUA_OK)
    lua_pop(ctx->L, 1);  // errors in the callback are ignored (one per chunk
                         // would otherwise pile up on the stack)
}

static int l_fs_copy(lua_State *L) {
  const char *src = luaL_checkstring(L, 1);
  const char *dst = luaL_checkstring(L, 2);
  if (!fs_sandbox_check(L, src, false)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (source)");
    return 2;
  }
  if (!fs_sandbox_check(L, dst, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (destination)");
    return 2;
  }

  copy_progress_ctx_t ctx = {NULL, LUA_NOREF};
  if (lua_isfunction(L, 3)) {
    lua_pushvalue(L, 3);
    ctx.L      = L;
    ctx.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  bool ok = sdcard_copy(src, dst,
                        ctx.fn_ref != LUA_NOREF ? copy_progress_trampoline : NULL,
                        &ctx);
  if (ctx.fn_ref != LUA_NOREF)
    luaL_unref(L, LUA_REGISTRYINDEX, ctx.fn_ref);

  if (ok) {
    lua_pushboolean(L, true);
    return 1;
  }
  lua_pushboolean(L, false);
  lua_pushstring(L, "copy failed");
  return 2;
}

// ── picocalc.fs.stat(path) → {size, is_dir, year, month, day, hour, min, sec} ─
static int l_fs_stat(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushnil(L);
    lua_pushstring(L, "permission denied");
    return 2;
  }
  sdcard_stat_t st;
  if (path_is_root(path)) {
    memset(&st, 0, sizeof st);  // no size, no timestamp (FAT keeps none)
    st.is_dir = true;
  } else if (!sdcard_stat(path, &st)) {
    lua_pushnil(L);
    lua_pushstring(L, "not found");
    return 2;
  }
  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)st.size); lua_setfield(L, -2, "size");
  lua_pushboolean(L, st.is_dir);            lua_setfield(L, -2, "is_dir");
  push_mtime_fields(L, st.fdate, st.ftime);
  return 1;
}

// ── picocalc.fs.ensureReady() → true/false ───────────────────────────────────
// Probes the SD card and attempts recovery if unresponsive.  Call before
// critical multi-write operations after long network activity.
static int l_fs_ensureReady(lua_State *L) {
  lua_pushboolean(L, sdcard_ensure_ready());
  return 1;
}

// ── picocalc.fs.diskInfo() → {free, total}  (values in KB) ──────────────────
static int l_fs_diskInfo(lua_State *L) {
  uint32_t free_kb = 0, total_kb = 0;
  if (!sdcard_disk_info(&free_kb, &total_kb)) {
    lua_pushnil(L);
    lua_pushstring(L, "disk info unavailable");
    return 2;
  }
  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)free_kb);  lua_setfield(L, -2, "free");
  lua_pushinteger(L, (lua_Integer)total_kb); lua_setfield(L, -2, "total");
  return 1;
}

// ── picocalc.fs.glob(path, pattern) → [{name, is_dir, size, year, …}] ────────
// Pattern supports * (any sequence) and ? (any single char); case-insensitive.
static bool glob_match(const char *pat, const char *str) {
  const char *star = NULL, *ss = str;
  while (*str) {
    char p = (*pat >= 'A' && *pat <= 'Z') ? (*pat + 32) : *pat;
    char s = (*str >= 'A' && *str <= 'Z') ? (*str + 32) : *str;
    if (p == '?' || p == s) {
      pat++; str++;
    } else if (*pat == '*') {
      star = pat++; ss = str;
    } else if (star) {
      pat = star + 1; str = ++ss;
    } else {
      return false;
    }
  }
  while (*pat == '*') pat++;
  return !*pat;
}

typedef struct { lua_State *L; int tidx; int n; const char *pattern; } glob_ctx_t;

static void glob_cb(const sdcard_entry_t *e, void *user) {
  glob_ctx_t *ctx = (glob_ctx_t *)user;
  if (!glob_match(ctx->pattern, e->name)) return;
  lua_State *L = ctx->L;
  lua_newtable(L);
  lua_pushstring(L, e->name);           lua_setfield(L, -2, "name");
  lua_pushboolean(L, e->is_dir);        lua_setfield(L, -2, "is_dir");
  lua_pushinteger(L, (lua_Integer)e->size); lua_setfield(L, -2, "size");
  push_mtime_fields(L, e->fdate, e->ftime);
  lua_rawseti(L, ctx->tidx, ++ctx->n);
}

static int l_fs_glob(lua_State *L) {
  const char *path    = luaL_checkstring(L, 1);
  const char *pattern = luaL_checkstring(L, 2);
  lua_newtable(L);
  if (!fs_sandbox_check(L, path, false)) return 1;
  glob_ctx_t ctx = {L, lua_gettop(L), 0, pattern};
  sdcard_list_dir(path, glob_cb, &ctx);
  return 1;
}

static int l_fs_setSlowMode(lua_State *L) {
  sd_set_slow_mode(lua_toboolean(L, 1));
  return 0;
}

// ── fs.isDir(path) → bool ─────────────────────────────────────────────────────
// True if the path exists and is a directory. False for a missing path, a
// file, and a path the sandbox refuses — same "not usable" answer as exists().
static int l_fs_is_dir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushboolean(L, false);
    return 1;
  }
  sdcard_stat_t st;
  lua_pushboolean(L, path_is_root(path) ||
                         (sdcard_stat(path, &st) && st.is_dir));
  return 1;
}

static const luaL_Reg l_fs_lib[] = {
    {"open",     l_fs_open},     {"read",     l_fs_read},
    {"write",    l_fs_write},    {"close",    l_fs_close},
    {"seek",     l_fs_seek},     {"tell",     l_fs_tell},
    {"fsize",    l_fs_fsize},
    {"exists",   l_fs_exists},   {"readFile", l_fs_readFile},
    {"size",     l_fs_size},     {"isDir",    l_fs_is_dir},
    {"listDir",  l_fs_listDir},
    {"mkdir",    l_fs_mkdir},    {"appPath",  l_fs_appPath},
    {"browse",   l_fs_browse},
    {"delete",   l_fs_delete},   {"deleteRecursive", l_fs_delete_recursive},
    {"rename",   l_fs_rename},
    {"copy",     l_fs_copy},     {"stat",     l_fs_stat},
    {"diskInfo", l_fs_diskInfo}, {"ensureReady", l_fs_ensureReady},
    {"glob",     l_fs_glob},     {"setSlowMode", l_fs_setSlowMode},
    {NULL, NULL}};


void lua_bridge_fs_init(lua_State *L) {
  fs_file_register_mt(L);
  register_subtable(L, "fs", l_fs_lib);
}
