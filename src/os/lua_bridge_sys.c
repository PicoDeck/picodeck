#include "lua_bridge_internal.h"
#include "app_identity.h"
#include "lua_psram_alloc.h"
#include "crashlog.h"
#include "idle_dim.h"
#include "ota_update.h"
#include "version.h"
#include "../dev_commands.h"
#include "../hardware.h"
#include "../drivers/keyboard.h"
#include "../drivers/pio_psram.h"
#include "perf.h"
#include "hardware/gpio.h"
#include <malloc.h>
#include <stdatomic.h>
#include <strings.h>

// ── picocalc.sys.* ───────────────────────────────────────────────────────────

static int l_sys_getTimeMs(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)to_ms_since_boot(get_absolute_time()));
  return 1;
}

// Microseconds since boot as a wrapping 32-bit integer (lua_Integer is
// 32-bit): subtract two readings as integers, which is exact for gaps under
// ~35 minutes even across the wrap.
static int l_sys_getTimeUs(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)(uint32_t)to_us_since_boot(get_absolute_time()));
  return 1;
}

static int l_sys_getBattery(lua_State *L) {
  // The keyboard bus engine reads the level every 5 s in the background;
  // this returns the latest reading without touching the bus.
  lua_pushinteger(L, kbd_get_battery_percent());
  return 1;
}

static int l_sys_log(lua_State *L) {
  const char *msg = luaL_checkstring(L, 1);
  printf("[APP] %s\n", msg);
#ifdef PICODECK_SIMULATOR
  extern void sim_log_append(const char *line);
  sim_log_append(msg);
#endif
  return 0;
}

static int l_sys_sleep(lua_State *L) {
  int ms = (int)lb_checkint(L, 1);
  // Every 10 ms: a background keyboard poll, so a physical Sym press is seen
  // during the sleep (it queues events and chars and accumulates press
  // edges without starting a new app-facing poll, so the app's next
  // input.update() still delivers them), then the same service pass as the
  // instruction hook: callbacks, dev commands, the menu, and the exit
  // request (raised from here, so an exit or the menu's Exit App ends a long
  // sleep at once).
  uint32_t start_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
  uint32_t span = ms > 0 ? (uint32_t)ms : 0;
  while (true) {
    kbd_poll_background();
    lua_bridge_service(L);
    uint32_t elapsed = (uint32_t)to_ms_since_boot(get_absolute_time()) - start_ms;
    if (elapsed >= span)
      break;
    uint32_t remaining = span - elapsed;
    sleep_ms(remaining < 10 ? remaining : 10);
  }
  return 0;
}

static int l_sys_reboot(lua_State *L) {
  (void)L;
  crashlog_clear_running(); // intentional — not an unclean exit
  kbd_prepare_reset();
  watchdog_enable(1, true);
  for (;;)
    tight_loop_contents();
  return 0;
}

// Pause Core 1 (WiFi, audio, HTTP) for safe batch SD writes.
// Matches the USB MSC pattern: caller must resumeBackground() when done.
extern _Atomic bool g_core1_pause;
extern _Atomic bool g_core1_paused;

static int l_sys_pauseBackground(lua_State *L) {
  (void)L;
  g_core1_pause = true;
  for (int i = 0; i < 500 && !g_core1_paused; i++)
    sleep_ms(1);
  lua_pushboolean(L, g_core1_paused);
  return 1;
}

static int l_sys_resumeBackground(lua_State *L) {
  (void)L;
  g_core1_pause = false;
  return 0;
}

// Deliberately trigger a HardFault for crash-logging testing.
// Writes to address 0 which is reserved on Cortex-M33 → immediate bus fault.
static int l_sys_trigger_fault(lua_State *L) {
  (void)L;
  *(volatile uint32_t *)0 = 0xDEADu;
  return 0; // never reached
}

static int l_sys_isUSBPowered(lua_State *L) {
  lua_pushboolean(L, gpio_get(USB_VBUS_PIN));
  return 1;
}

static int l_sys_getPowerStatus(lua_State *L) {
  lua_createtable(L, 0, 2);
  lua_pushboolean(L, gpio_get(USB_VBUS_PIN));
  lua_setfield(L, -2, "charging");
  lua_pushinteger(L, kbd_get_battery_percent());
  lua_setfield(L, -2, "percent");
  return 1;
}

// Exit the current app cleanly, returning to the launcher.
// Equivalent to `return` at the top level of main.lua, but works from
// any call depth. The launcher detects the sentinel and skips the error screen.
static int l_sys_exit(lua_State *L) {
  lua_bridge_raise_exit(L);
  return 0; // unreachable
}

// ── picocalc.sys.addMenuItem / clearMenuItems
// ───────────────────────────────── Lua-registered callbacks are stored here as
// Lua registry references. A C trampoline is passed to system_menu_add_item()
// so that calling the menu item invokes the original Lua function.

typedef struct {
  lua_State *L;
  int ref; // LUA_REGISTRYINDEX reference to the Lua function
} lua_callback_t;

static lua_callback_t s_lua_callbacks[SYSMENU_MAX_APP_ITEMS];
static int s_lua_callback_count = 0;

static void lua_menu_trampoline(void *user) {
  lua_callback_t *cb = (lua_callback_t *)user;
  lua_rawgeti(cb->L, LUA_REGISTRYINDEX, cb->ref);
  lua_call(cb->L, 0, 0); // propagates errors (including sys.exit() sentinel)
}

// picocalc.sys.getClock() → {synced=bool, hour=int, min=int, sec=int,
// epoch=int} epoch is UTC Unix seconds; hour/min/sec are UTC + tz_offset.
static int l_sys_getClock(lua_State *L) {
  int h = 0, m = 0, s = 0;
  bool synced = clock_get_time(&h, &m, &s);
  lua_createtable(L, 0, 5);
  lua_pushboolean(L, synced);
  lua_setfield(L, -2, "synced");
  lua_pushinteger(L, h);
  lua_setfield(L, -2, "hour");
  lua_pushinteger(L, m);
  lua_setfield(L, -2, "min");
  lua_pushinteger(L, s);
  lua_setfield(L, -2, "sec");
  lua_pushinteger(L, (lua_Integer)clock_get_epoch());
  lua_setfield(L, -2, "epoch");
  return 1;
}

static int l_sys_getMemInfo(lua_State *L) {
  size_t psram_free  = lua_psram_alloc_free_size();
  size_t psram_total = lua_psram_alloc_total_size();
  size_t psram_used  = psram_total - psram_free;

  struct mallinfo mi = mallinfo();

  lua_createtable(L, 0, 14);
  lua_pushinteger(L, (lua_Integer)psram_free);
  lua_setfield(L, -2, "psram_free");
  lua_pushinteger(L, (lua_Integer)psram_used);
  lua_setfield(L, -2, "psram_used");
  lua_pushinteger(L, (lua_Integer)psram_total);
  lua_setfield(L, -2, "psram_total");
  lua_pushinteger(L, (lua_Integer)lua_psram_alloc_largest_block());
  lua_setfield(L, -2, "psram_largest_block");
  lua_pushinteger(L, (lua_Integer)lua_psram_alloc_fragmentation());
  lua_setfield(L, -2, "psram_fragmentation");
  // Small-object pools (slabs are umm blocks, already counted above).
  small_stats_t sp;
  lua_psram_alloc_small_stats(&sp);
  lua_pushinteger(L, (lua_Integer)sp.slabs);
  lua_setfield(L, -2, "small_pool_slabs");
  lua_pushinteger(L, (lua_Integer)sp.slab_bytes);
  lua_setfield(L, -2, "small_pool_bytes");
  lua_pushinteger(L, (lua_Integer)sp.objects);
  lua_setfield(L, -2, "small_pool_objects");
  lua_pushinteger(L, (lua_Integer)sp.object_bytes);
  lua_setfield(L, -2, "small_pool_object_bytes");
  lua_pushinteger(L, (lua_Integer)mi.fordblks);
  lua_setfield(L, -2, "sram_free");
  lua_pushinteger(L, (lua_Integer)mi.uordblks);
  lua_setfield(L, -2, "sram_used");

  // PIO PSRAM (mainboard 8MB via PIO1)
  lua_pushboolean(L, pio_psram_available());
  lua_setfield(L, -2, "pio_psram_available");
  lua_pushinteger(L, (lua_Integer)pio_psram_size());
  lua_setfield(L, -2, "pio_psram_size");

  // XIP cache hit rate (0-100%, or -1 if no accesses)
  lua_pushinteger(L, (lua_Integer)perf_xip_cache_hit_rate());
  lua_setfield(L, -2, "xip_cache_hit_rate");

  return 1;
}

static int l_sys_addMenuItem(lua_State *L) {
  const char *label = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);

  if (s_lua_callback_count >= SYSMENU_MAX_APP_ITEMS)
    return luaL_error(L, "too many menu items (max %d)", SYSMENU_MAX_APP_ITEMS);

  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_callback_t *cb = &s_lua_callbacks[s_lua_callback_count++];
  cb->L = L;
  cb->ref = ref;

  system_menu_add_item(label, lua_menu_trampoline, cb);
  return 0;
}

static int l_sys_getVersion(lua_State *L) {
  lua_pushstring(L, PICODECK_VERSION);
  return 1;
}

// sys.applyUpdate(path) — flash firmware.  Registered only for OS apps
// (sys_update_allowed); validates the image, its checksum and its signature
// (ota_prepare_update → ota_verify.c), then asks
// the user before rebooting into the updater.
static int l_sys_applyUpdate(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "access denied");
    return 2;
  }
  const char *err = NULL;
  if (!ota_prepare_update(path, &err)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, err ? err : "Unknown error");
    return 2;
  }
  char msg[160];
  snprintf(msg, sizeof(msg), "Flash firmware from %s? The device reboots.",
           path);
  if (!ui_confirm(msg)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "cancelled");
    return 2;
  }
  bool ok = ota_trigger_update(path, &err);
  if (!ok) {
    lua_pushboolean(L, false);
    lua_pushstring(L, err ? err : "Unknown error");
    return 2;
  }
  // Unreachable — ota_trigger_update reboots on success
  return 0;
}

static int l_sys_clearMenuItems(lua_State *L) {
  for (int i = 0; i < s_lua_callback_count; i++)
    luaL_unref(L, LUA_REGISTRYINDEX, s_lua_callbacks[i].ref);
  s_lua_callback_count = 0;
  system_menu_clear_items();
  return 0;
}

// picocalc.sys.loadlib(name) — load /system/lib/<name>.lua and return its result
static int l_sys_loadlib(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);

  // Build path: /system/lib/<name>.lua
  char path[128];
  int n = snprintf(path, sizeof(path), "/system/lib/%s.lua", name);
  if (n < 0 || n >= (int)sizeof(path))
    return luaL_error(L, "library name too long");

  // Check for path traversal
  if (strstr(name, "..") || strchr(name, '/'))
    return luaL_error(L, "invalid library name");

  // Read file via SD card
  sdfile_t f = sdcard_fopen(path, "r");
  if (!f) {
    lua_pushnil(L);
    lua_pushfstring(L, "cannot open %s", path);
    return 2;
  }

  int size = sdcard_fsize(path);
  if (size <= 0) {
    sdcard_fclose(f);
    lua_pushnil(L);
    lua_pushfstring(L, "empty or unreadable: %s", path);
    return 2;
  }

  char *buf = (char *)umm_malloc(size + 1);
  if (!buf) {
    sdcard_fclose(f);
    return luaL_error(L, "out of memory loading %s (%d bytes)", path, size);
  }

  int read = sdcard_fread(f, buf, size);
  sdcard_fclose(f);
  if (read <= 0) {  // a failed read returns -1: buf[-1] was written here
    umm_free(buf);
    lua_pushnil(L);
    lua_pushfstring(L, "cannot read %s", path);
    return 2;
  }
  buf[read] = '\0';

  // Load and execute (source text only — never precompiled bytecode)
  int status = luaL_loadbufferx(L, buf, read, path, "t");
  umm_free(buf);

  if (status != LUA_OK)
    return lua_error(L);  // propagate compile error

  // Call the loaded chunk (no arguments, one result)
  if (lua_pcall(L, 0, 1, 0) != LUA_OK)
    return lua_error(L);  // propagate runtime error

  return 1;  // return the module's result
}

// ── PIO PSRAM (apps get [PIO_PSRAM_APP_BASE, chip end) only) ───────────────

// Raise a Lua error unless [addr, addr+len) is app-accessible PIO PSRAM.
// Checked even when the chip is absent (against its nominal size), so an
// app's mistake shows up on every device.
static void pio_check_app_range(lua_State *L, const char *fn,
                                lua_Integer addr, lua_Integer len) {
  uint32_t chip = pio_psram_available() ? pio_psram_size() : PIO_PSRAM_SIZE;
  pio_psram_range_t r =
      pio_psram_app_range_check((int64_t)addr, (int64_t)len, chip);
  if (r == PIO_PSRAM_RANGE_OK)
    return;
  char msg[96];  // lua_pushfstring has no %x
  if (r == PIO_PSRAM_RANGE_RESERVED)
    snprintf(msg, sizeof(msg),
             "%s: address range reserved by the OS (apps start at 0x%lx)",
             fn, (unsigned long)PIO_PSRAM_APP_BASE);
  else
    snprintf(msg, sizeof(msg),
             "%s: address/length out of range (apps may use 0x%lx..0x%lx)",
             fn, (unsigned long)PIO_PSRAM_APP_BASE, (unsigned long)chip);
  luaL_error(L, "%s", msg);
}

// picocalc.sys.pioPsramRead(addr, len) -> string, or nil without the chip
static int l_sys_pio_psram_read(lua_State *L) {
  lua_Integer addr = luaL_checkinteger(L, 1);
  lua_Integer len  = luaL_checkinteger(L, 2);
  pio_check_app_range(L, "pioPsramRead", addr, len);
  if (!pio_psram_available() || len == 0) { lua_pushnil(L); return 1; }
  picodeck_lua_strbuf_t sb;  // read straight into the result string
  char *p = picodeck_lua_strbuf_init(L, &sb, (size_t)len);
  pio_psram_read((uint32_t)addr, (uint8_t *)p, (uint32_t)len);
  picodeck_lua_strbuf_push(L, &sb, (size_t)len);
  return 1;
}

// picocalc.sys.pioPsramWrite(addr, data) -> bytes_written (0 without the chip)
static int l_sys_pio_psram_write(lua_State *L) {
  lua_Integer addr = luaL_checkinteger(L, 1);
  size_t len;
  const char *data = luaL_checklstring(L, 2, &len);
  if (len > (size_t)PIO_PSRAM_SIZE)
    return luaL_error(L, "pioPsramWrite: data too large");
  pio_check_app_range(L, "pioPsramWrite", addr, (lua_Integer)len);
  if (!pio_psram_available() || len == 0) { lua_pushinteger(L, 0); return 1; }
  pio_psram_write((uint32_t)addr, (const uint8_t *)data, (uint32_t)len);
  lua_pushinteger(L, (lua_Integer)len);
  return 1;
}

// picocalc.sys.pioPsramSize() -> size in bytes (0 if not available)
static int l_sys_pio_psram_size(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)pio_psram_size());
  return 1;
}

// ── QMI PSRAM buffers ────────────────────────────────────────────────────────
// sys.qmiPsramAlloc(size) returns a full userdata that owns a umm_malloc
// block and knows its size: reads and writes are bounds-checked, a freed
// buffer refuses access, and the block is released when the handle is
// collected (or the app's Lua state closes).  No raw pointers reach Lua.

// qmi_buf_t and QMI_BUF_MT live in lua_bridge_internal.h: graphics
// image.loadFromBuffer also reads these buffers.

static void qmi_buf_release(qmi_buf_t *b) {
  if (b->p) {
    umm_free(b->p);
    b->p = NULL;
    b->size = 0;
  }
}

// The buffer's bytes [off, off+len), or a Lua error.
static uint8_t *qmi_buf_span(lua_State *L, lua_Integer off, lua_Integer len) {
  qmi_buf_t *b = (qmi_buf_t *)lb_checkudata(L, 1, QMI_BUF_MT);
  if (!b->p)
    luaL_error(L, "qmiPsram: buffer freed");
  int64_t o = (int64_t)off, n = (int64_t)len, size = (int64_t)b->size;
  if (o < 0 || n < 0 || o > size || n > size - o)
    luaL_error(L, "qmiPsram: offset/length out of range (buffer is %d bytes)",
               (int)b->size);
  return b->p + off;
}

// sys.qmiPsramAlloc(size) -> handle, or nil when out of PSRAM
static int l_sys_qmi_psram_alloc(lua_State *L) {
  lua_Integer size = luaL_checkinteger(L, 1);
  luaL_argcheck(L, size > 0, 1, "size must be positive");
  qmi_buf_t *b = (qmi_buf_t *)lua_newuserdatauv(L, sizeof(*b), 0);
  b->p = NULL;
  b->size = 0;
  luaL_setmetatable(L, QMI_BUF_MT);
  b->p = (uint8_t *)umm_malloc((size_t)size);
  if (!b->p) { lua_pushnil(L); return 1; }
  b->size = (size_t)size;
  return 1;
}

// sys.qmiPsramFree(handle) — idempotent
static int l_sys_qmi_psram_free(lua_State *L) {
  qmi_buf_release((qmi_buf_t *)lb_checkudata(L, 1, QMI_BUF_MT));
  return 0;
}

// sys.qmiPsramWrite(handle, offset, data) -> bytes written
static int l_sys_qmi_psram_write(lua_State *L) {
  lua_Integer offset = luaL_checkinteger(L, 2);
  size_t len;
  const char *data = luaL_checklstring(L, 3, &len);
  uint8_t *dst = qmi_buf_span(L, offset, (lua_Integer)len);
  memcpy(dst, data, len);
  lua_pushinteger(L, (lua_Integer)len);
  return 1;
}

// sys.qmiPsramRead(handle, offset, len) -> string
static int l_sys_qmi_psram_read(lua_State *L) {
  lua_Integer offset = luaL_checkinteger(L, 2);
  lua_Integer len = luaL_checkinteger(L, 3);
  const uint8_t *src = qmi_buf_span(L, offset, len);
  lua_pushlstring(L, (const char *)src, (size_t)len);
  return 1;
}

static int l_qmi_buf_gc(lua_State *L) {
  qmi_buf_release((qmi_buf_t *)lb_checkudata(L, 1, QMI_BUF_MT));
  return 0;
}

// picocalc.sys.resetIdleTimer() — mark the device as active so the idle
// screen dimmer doesn't darken the backlight. Call periodically from apps
// that show moving content without receiving input (video, slideshows).
static int l_sys_resetIdleTimer(lua_State *L) {
  (void)L;
  idle_dim_note_activity();
  return 0;
}

static const luaL_Reg l_sys_lib[] = {{"getMemInfo", l_sys_getMemInfo},
                                     {"getTimeMs", l_sys_getTimeMs},
                                     {"getTimeUs", l_sys_getTimeUs},
                                     {"getBattery", l_sys_getBattery},
                                     {"log", l_sys_log},
                                     {"sleep", l_sys_sleep},
                                     {"exit", l_sys_exit},
                                     {"reboot", l_sys_reboot},
                                     {"isUSBPowered", l_sys_isUSBPowered},
                                     {"getPowerStatus", l_sys_getPowerStatus},
                                     {"getClock", l_sys_getClock},
                                     {"getVersion", l_sys_getVersion},
                                     {"addMenuItem", l_sys_addMenuItem},
                                     {"clearMenuItems", l_sys_clearMenuItems},
                                     {"pauseBackground", l_sys_pauseBackground},
                                     {"resumeBackground", l_sys_resumeBackground},
                                     {"triggerFault", l_sys_trigger_fault},
                                     {"resetIdleTimer", l_sys_resetIdleTimer},
                                     {"loadlib", l_sys_loadlib},
                                     {"pioPsramRead", l_sys_pio_psram_read},
                                     {"pioPsramWrite", l_sys_pio_psram_write},
                                     {"pioPsramSize", l_sys_pio_psram_size},
                                     {"qmiPsramAlloc", l_sys_qmi_psram_alloc},
                                     {"qmiPsramFree", l_sys_qmi_psram_free},
                                     {"qmiPsramWrite", l_sys_qmi_psram_write},
                                     {"qmiPsramRead", l_sys_qmi_psram_read},
                                     {NULL, NULL}};


// sys.applyUpdate is for the OS's own updaters: the "system-update"
// requirement AND an OS app (under /system/, or the updater/store id).
// app.json is written by the app itself, so the requirement alone is
// consent, not a boundary; the id/location check narrows it to OS apps.
static bool sys_update_allowed(void) {
  const app_identity_t *me = app_identity_current();
  if (!me || !app_identity_has_requirement("system-update"))
    return false;
  if (strncasecmp(me->dir, "/system/", 8) == 0)
    return true;
  return strcmp(me->id, "net.picodeck.updater") == 0 ||
         strcmp(me->id, "net.picodeck.store") == 0;
}

void lua_bridge_sys_init(lua_State *L) {
  s_lua_callback_count = 0;
  system_menu_clear_items();

  luaL_newmetatable(L, QMI_BUF_MT);
  lua_pushcfunction(L, l_qmi_buf_gc);
  lua_setfield(L, -2, "__gc");
  lua_pushliteral(L, "qmibuf");
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 1);

  // picocalc.sys (picocalc is at the top of the stack)
  lua_newtable(L);
  luaL_setfuncs(L, l_sys_lib, 0);
  if (sys_update_allowed()) {
    lua_pushcfunction(L, l_sys_applyUpdate);
    lua_setfield(L, -2, "applyUpdate");
  }
  lua_setfield(L, -2, "sys");
}
