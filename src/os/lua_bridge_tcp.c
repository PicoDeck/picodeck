#include "lua_bridge_internal.h"
#include "../drivers/tcp.h"
#include "../drivers/wifi.h"
#include "umm_malloc.h"
#include "pico/time.h"
#include "hardware/timer.h"

#define TCP_MT "picocalc.tcp"
// Registry key of a weak-valued table: light userdata (the tcp_ud_t) → the
// socket object, so tcp_lua_fire_pending can pass the object to callbacks
// and keep it alive while they run, without keeping sockets from GC.
#define TCP_OBJS "picocalc.tcp.objs"

typedef struct {
    tcp_conn_t *conn;   // NULL once closed/GC'd (the slot is then released)
    int cb_connect;
    int cb_read;
    int cb_closed;
} tcp_ud_t;

static tcp_ud_t *check_tcp(lua_State *L, int idx) {
    tcp_ud_t *ud = (tcp_ud_t *)lb_checkudata(L, idx, TCP_MT);
    return ud;
}

static tcp_ud_t *check_tcp_open(lua_State *L, int idx) {
    tcp_ud_t *ud = check_tcp(L, idx);
    if (!ud->conn)
        luaL_error(L, "tcp: connection is closed");
    return ud;
}

static int set_tcp_cb(lua_State *L, int idx, int *ref_ptr) {
    if (lua_isnoneornil(L, idx)) {
        *ref_ptr = LUA_NOREF;
        return 0;
    }
    luaL_checktype(L, idx, LUA_TFUNCTION);
    lua_settop(L, idx);
    *ref_ptr = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static void tcp_ud_unref_all(lua_State *L, tcp_ud_t *ud) {
    if (!ud) return;
    if (ud->cb_connect != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_connect);
        ud->cb_connect = LUA_NOREF;
    }
    if (ud->cb_read != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_read);
        ud->cb_read = LUA_NOREF;
    }
    if (ud->cb_closed != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_closed);
        ud->cb_closed = LUA_NOREF;
    }
}

// Release the C slot (asynchronously: tcp_free only queues the close and the
// slot is reclaimed once Core 1 has let go of it, see tcp.h).
static void tcp_ud_release(tcp_ud_t *ud) {
    if (ud->conn) {
        tcp_free(ud->conn);
        ud->conn = NULL;
    }
}

static int l_tcp_gc(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    tcp_ud_unref_all(L, ud);
    tcp_ud_release(ud);
    return 0;
}

static int l_tcp_new(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    uint16_t port = (uint16_t)luaL_optinteger(L, 2, 80);
    bool use_ssl = lua_gettop(L) >= 3 && lua_toboolean(L, 3);

    tcp_conn_t *conn = tcp_alloc();
    if (!conn) {
        lua_pushnil(L);
        lua_pushstring(L, "TCP connection pool full or out of memory");
        return 2;
    }

    strncpy(conn->host, host, sizeof(conn->host) - 1);
    conn->port = port;
    conn->use_ssl = use_ssl;

    tcp_ud_t *ud = (tcp_ud_t *)lua_newuserdata(L, sizeof(tcp_ud_t));
    ud->conn = conn;
    ud->cb_connect = LUA_NOREF;
    ud->cb_read = LUA_NOREF;
    ud->cb_closed = LUA_NOREF;

    luaL_getmetatable(L, TCP_MT);
    lua_setmetatable(L, -2);

    lua_getfield(L, LUA_REGISTRYINDEX, TCP_OBJS);
    lua_pushvalue(L, -2);
    lua_rawsetp(L, -2, ud);
    lua_pop(L, 1);
    conn->lua_ud = ud;
    return 1;
}

static int l_tcp_connect(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    if (!wifi_is_available()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "WiFi not available");
        return 2;
    }
    bool ok = tcp_connect(ud->conn, ud->conn->host, ud->conn->port, ud->conn->use_ssl);
    lua_pushboolean(L, ok);
    if (!ok) {
        const char *err = tcp_get_error(ud->conn);
        lua_pushstring(L, err ? err
                              : "cannot connect now (already connecting or connected)");
    }
    return ok ? 1 : 2;
}

static int l_tcp_write(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    int written = tcp_write(ud->conn, data, (int)len);
    if (written < 0) {
        lua_pushinteger(L, -1);
        return 1;
    }
    lua_pushinteger(L, written);
    return 1;
}

static int l_tcp_read(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    int max_len = (int)luaL_optinteger(L, 2, 4096);

    // Data the peer sent before closing stays readable.
    if (max_len <= 0 || tcp_bytes_available(ud->conn) == 0) {
        lua_pushnil(L);
        return 1;
    }

    uint8_t *buf = umm_malloc((size_t)max_len + 1);
    if (!buf) {
        lua_pushnil(L);
        return 1;
    }

    int n = tcp_read(ud->conn, buf, max_len);
    if (n <= 0) {
        umm_free(buf);
        lua_pushnil(L);
        return 1;
    }

    buf[n] = '\0';
    lua_pushlstring(L, (const char *)buf, (size_t)n);
    umm_free(buf);
    return 1;
}

static int l_tcp_available(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    if (!ud->conn) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)tcp_bytes_available(ud->conn));
    return 1;
}

static int l_tcp_close(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    tcp_ud_release(ud);
    tcp_ud_unref_all(L, ud);
    return 0;
}

static int l_tcp_error(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    const char *err = ud->conn ? tcp_get_error(ud->conn) : NULL;
    if (!err) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, err);
    return 1;
}

static int l_tcp_isConnected(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    bool connected = ud->conn &&
                     tcp_get_state(ud->conn) == TCP_STATE_CONNECTED;
    lua_pushboolean(L, connected);
    return 1;
}

// Seconds (fractions allowed) → ms; negative counts as 0.
static uint32_t tcp_secs_to_ms(lua_State *L, int idx) {
    lua_Number s = luaL_checknumber(L, idx);
    return s > 0 ? (uint32_t)(s * 1000.0) : 0;
}

// sock:setConnectTimeout(seconds) — applies to the next connect().
static int l_tcp_setConnectTimeout(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    uint32_t ms = tcp_secs_to_ms(L, 2);
    if (ud->conn) tcp_set_connect_timeout(ud->conn, ms);
    return 0;
}

// tcp:setInsecure(bool) — before connect(): TLS without certificate
// verification or the SNTP clock gate (self-signed dev servers only).
static int l_tcp_setInsecure(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    ud->conn->insecure = lua_toboolean(L, 2);
    return 0;
}

// sock:setReadTimeout(seconds) — fail a connected socket that receives
// nothing for that long; 0 turns it off (the default).
static int l_tcp_setReadTimeout(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    uint32_t ms = tcp_secs_to_ms(L, 2);
    if (ud->conn) tcp_set_read_timeout(ud->conn, ms);
    return 0;
}

static int l_tcp_setConnectCallback(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    return set_tcp_cb(L, 2, &ud->cb_connect);
}

static int l_tcp_setReadCallback(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    return set_tcp_cb(L, 2, &ud->cb_read);
}

static int l_tcp_setCloseCallback(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    return set_tcp_cb(L, 2, &ud->cb_closed);
}

static int l_tcp_getEvents(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    if (!ud->conn) {
        lua_pushinteger(L, 0);
        return 1;
    }
    uint32_t events = tcp_take_pending(ud->conn);
    lua_pushinteger(L, (lua_Integer)events);
    return 1;
}

static int l_tcp_waitConnected(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    uint32_t timeout_ms = (uint32_t)(luaL_optnumber(L, 2, 10.0) * 1000.0);
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (ud->conn && tcp_get_state(ud->conn) != TCP_STATE_CONNECTED) {
        tcp_conn_state_t st = tcp_get_state(ud->conn);
        if (st == TCP_STATE_FAILED || st == TCP_STATE_CLOSED) {
            break;
        }
        if (to_ms_since_boot(get_absolute_time()) - start > timeout_ms) {
            break;
        }
        sleep_ms(10);
    }

    lua_pushboolean(L, ud->conn &&
                           tcp_get_state(ud->conn) == TCP_STATE_CONNECTED);
    return 1;
}

static int l_tcp_waitData(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    uint32_t timeout_ms = (uint32_t)(luaL_optnumber(L, 2, 10.0) * 1000.0);
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (ud->conn && tcp_get_state(ud->conn) == TCP_STATE_CONNECTED) {
        if (tcp_bytes_available(ud->conn) > 0) {
            break;
        }
        if (to_ms_since_boot(get_absolute_time()) - start > timeout_ms) {
            break;
        }
        sleep_ms(10);
    }

    lua_pushboolean(L, tcp_bytes_available(ud->conn) > 0);
    return 1;
}

static const luaL_Reg l_tcp_methods[] = {
    {"connect", l_tcp_connect},
    {"write", l_tcp_write},
    {"read", l_tcp_read},
    {"available", l_tcp_available},
    {"close", l_tcp_close},
    {"error", l_tcp_error},
    {"isConnected", l_tcp_isConnected},
    {"setConnectTimeout", l_tcp_setConnectTimeout},
    {"setInsecure", l_tcp_setInsecure},
    {"setReadTimeout", l_tcp_setReadTimeout},
    {"setConnectCallback", l_tcp_setConnectCallback},
    {"setReadCallback", l_tcp_setReadCallback},
    {"setCloseCallback", l_tcp_setCloseCallback},
    {"getEvents", l_tcp_getEvents},
    {"waitConnected", l_tcp_waitConnected},
    {"waitData", l_tcp_waitData},
    {NULL, NULL}
};

static const luaL_Reg l_tcp_lib[] = {
    {"new", l_tcp_new},
    {NULL, NULL}
};

// Push the socket object for ud (false, nothing pushed, if it was collected).
static bool push_tcp_obj(lua_State *L, tcp_ud_t *ud) {
    lua_getfield(L, LUA_REGISTRYINDEX, TCP_OBJS);
    if (lua_istable(L, -1) && lua_rawgetp(L, -1, ud) == LUA_TUSERDATA) {
        lua_remove(L, -2);
        return true;
    }
    lua_pop(L, lua_istable(L, -1) ? 2 : 1);
    return false;
}

// True while tcp_lua_fire_pending runs (it is never re-entered).  Reset by
// lua_bridge_tcp_init: an error unwinding out of a callback must not leave
// it set for the next app.
static bool s_firing = false;

static void tcp_fire(lua_State *L, tcp_ud_t *ud, tcp_conn_t *c, int obj,
                     int ref, const char *what) {
    // A callback that ran before this one may have closed the socket.
    if (ud->conn != c || ref == LUA_NOREF)
        return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushvalue(L, obj);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        if (lua_bridge_is_exit_sentinel(L, -1)) {
            s_firing = false;  // the app is leaving: unwind to it
            lua_error(L);
        }
        printf("[TCP-LUA] %s callback error: %s\n", what, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

// Reclaim released slots, then fire the callbacks for pending events.
// Called from the instruction hook, sys.sleep and the terminal's wait loop;
// never re-entered (a callback that sleeps or runs long enough for the hook
// leaves later events for the outer call).  Only the events that have a
// callback are taken, so sock:getEvents() still sees the others.
void tcp_lua_fire_pending(lua_State *L) {
    if (s_firing)
        return;
    s_firing = true;
    tcp_reap();
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = tcp_get_conn(i);
        if (!c || !c->lua_ud) continue;
        tcp_ud_t *ud = (tcp_ud_t *)c->lua_ud;

        uint32_t mask = 0;
        if (ud->cb_connect != LUA_NOREF) mask |= TCP_CB_CONNECT;
        if (ud->cb_read != LUA_NOREF) mask |= TCP_CB_READ;
        if (ud->cb_closed != LUA_NOREF) mask |= TCP_CB_CLOSED | TCP_CB_FAILED;
        if (!mask) continue;
        uint32_t events = tcp_take_pending_bits(c, mask);
        if (!events) continue;
        if (!push_tcp_obj(L, ud)) continue;  // collected; __gc releases it
        int obj = lua_gettop(L);             // keeps ud alive meanwhile

        if (events & TCP_CB_CONNECT)
            tcp_fire(L, ud, c, obj, ud->cb_connect, "connect");
        if (events & TCP_CB_READ)
            tcp_fire(L, ud, c, obj, ud->cb_read, "read");
        if (events & (TCP_CB_CLOSED | TCP_CB_FAILED))
            tcp_fire(L, ud, c, obj, ud->cb_closed, "close");
        lua_settop(L, obj - 1);
    }
    s_firing = false;
}

void lua_bridge_tcp_init(lua_State *L) {
    s_firing = false;
    // Sockets a previous app (Lua or native) left behind.
    tcp_close_all();

    lua_newtable(L);                       // objs
    lua_newtable(L);                       // its metatable
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, TCP_OBJS);

    static const luaL_Reg tcp_meta[] = {{"__gc", l_tcp_gc}, {NULL, NULL}};
    lb_register_type(L, TCP_MT, l_tcp_methods, tcp_meta);

    lua_newtable(L);
    luaL_setfuncs(L, l_tcp_lib, 0);

    lua_pushinteger(L, 1); lua_setfield(L, -2, "CB_CONNECT");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "CB_READ");
    lua_pushinteger(L, 4); lua_setfield(L, -2, "CB_WRITE");
    lua_pushinteger(L, 8); lua_setfield(L, -2, "CB_CLOSED");
    lua_pushinteger(L, 16); lua_setfield(L, -2, "CB_FAILED");

    lua_setfield(L, -2, "tcp");
}
