// apps/ssh's key exchange on the real pieces: the app's Lua (lib/ssh_wire.lua,
// ssh_transport.lua, ssh_auth.lua) in the device's Lua configuration, over the
// real picocalc.crypto bridge, crypto.c and the firmware's mbedTLS. Nothing in
// the crypto is stubbed, so a wrong argument at a call site fails here as it
// would on the device. The checks are in test_ssh_transport.lua.
//
//   test_ssh_transport          recorded OpenSSH handshakes (ssh_handshake_vectors.h)
//   test_ssh_transport --live   the client against a real sshd, if there is one:
//                               sshd -i over a socketpair with throwaway host keys,
//                               no port and no system configuration. Exits 77 (ctest:
//                               skipped) when sshd or ssh-keygen is missing or unusable.
//                               SSHD_LOG=<file> appends sshd's debug log to a file.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "crypto.h"
#include "os.h"
#include "rng.h"
#include "ssh_handshake_vectors.h"

#define EXIT_SKIP 77

// ── What the bridge links against ───────────────────────────────────────────

PicoCalcAPI g_api;

bool rng_ready(void) { return true; }
bool rng_bytes(void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {  // getentropy takes at most 256 bytes at a time
        size_t n = len < 256 ? len : 256;
        if (getentropy(p, n) != 0) abort();
        p += n;
        len -= n;
    }
    return true;
}
int rng_mbedtls_random(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    return rng_bytes(out, len) ? 0 : -1;
}

void *lb_checkudata(lua_State *L, int idx, const char *tname) {
    return luaL_checkudata(L, idx, tname);
}

void register_subtable(lua_State *L, const char *name, const luaL_Reg *funcs) {
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_setfield(L, -2, name);
}

// As in lua_bridge.c, without what only the device's other types use.
void lb_register_type(lua_State *L, const char *mtname, const luaL_Reg *methods,
                      const luaL_Reg *meta) {
    luaL_newmetatable(L, mtname);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, meta, 0);
    lua_pop(L, 1);
}

// Only crypto.sha256File reaches this, and nothing here calls it.
bool fs_sandbox_check(lua_State *L, const char *path, bool write);
bool fs_sandbox_check(lua_State *L, const char *path, bool write) {
    (void)L; (void)path; (void)write;
    return false;
}

void lua_bridge_crypto_init(lua_State *L);

// The same wiring as s_crypto_impl in src/main.c.
static bool random_bytes_w(uint8_t *buf, uint32_t len) { return rng_bytes(buf, len); }
static pccrypto_aes_t aes_new_w(const uint8_t *key, uint32_t klen, const uint8_t *nonce) {
    return (pccrypto_aes_t)crypto_aes_new(key, klen, nonce);
}
static int aes_update_w(pccrypto_aes_t ctx, const uint8_t *in, uint8_t *out, uint32_t len) {
    return crypto_aes_update((crypto_aes_t *)ctx, in, out, len);
}
static void aes_free_w(pccrypto_aes_t ctx) { crypto_aes_free((crypto_aes_t *)ctx); }
static pccrypto_ecdh_t ecdh_x25519_w(void) { return (pccrypto_ecdh_t)crypto_ecdh_x25519(); }
static pccrypto_ecdh_t ecdh_p256_w(void) { return (pccrypto_ecdh_t)crypto_ecdh_p256(); }
static void ecdh_pub_w(pccrypto_ecdh_t ctx, uint8_t *out, uint32_t *out_len) {
    crypto_ecdh_get_public_key((crypto_ecdh_t *)ctx, out, out_len);
}
static int ecdh_shared_w(pccrypto_ecdh_t ctx, const uint8_t *remote, uint32_t rlen,
                         uint8_t *out, uint32_t *out_len) {
    return crypto_ecdh_compute_shared((crypto_ecdh_t *)ctx, remote, rlen, out, out_len);
}
static void ecdh_free_w(pccrypto_ecdh_t ctx) { crypto_ecdh_free((crypto_ecdh_t *)ctx); }

static const picocalc_crypto_t s_crypto = {
    .sha256 = crypto_sha256,
    .sha1 = crypto_sha1,
    .hmacSha256 = crypto_hmac_sha256,
    .hmacSha1 = crypto_hmac_sha1,
    .randomBytes = random_bytes_w,
    .deriveKey = crypto_derive_key,
    .aesNew = aes_new_w,
    .aesUpdate = aes_update_w,
    .aesFree = aes_free_w,
    .ecdhX25519 = ecdh_x25519_w,
    .ecdhP256 = ecdh_p256_w,
    .ecdhGetPublicKey = ecdh_pub_w,
    .ecdhComputeShared = ecdh_shared_w,
    .ecdhFree = ecdh_free_w,
    .rsaVerify = crypto_rsa_verify,
    .ecdsaP256Verify = crypto_ecdsa_p256_verify,
};

// ── picocalc.sys and the test's own functions ───────────────────────────────

static int l_get_time_ms(lua_State *L) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushinteger(L, (lua_Integer)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
    return 1;
}

static int l_sleep(lua_State *L) {
    int ms = (int)luaL_checkinteger(L, 1);
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    return 0;
}

// check(cond, what): one CHECK whose failure names what the Lua side was testing.
static int l_check(lua_State *L) {
    s_check_count++;
    if (!lua_toboolean(L, 1)) {
        luaL_where(L, 1);
        printf("FAIL %s%s\n", lua_tostring(L, -1), luaL_optstring(L, 2, "check failed"));
        s_check_fails++;
    }
    return 0;
}

static void push_hex(lua_State *L, const char *hex) {
    size_t n = strlen(hex) / 2;
    uint8_t *bytes = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        bytes[i] = (uint8_t)v;
    }
    lua_pushlstring(L, (const char *)bytes, n);
    free(bytes);
}

#define SET_STR(st, f) (lua_pushstring(L, (st)->f), lua_setfield(L, -2, #f))
#define SET_HEX(st, f) (push_hex(L, (st)->f), lua_setfield(L, -2, #f))

static void push_vectors(lua_State *L) {
    lua_newtable(L);
    for (size_t i = 0; i < sizeof SSH_HANDSHAKES / sizeof SSH_HANDSHAKES[0]; i++) {
        const ssh_handshake_t *v = &SSH_HANDSHAKES[i];
        lua_newtable(L);
        SET_STR(v, name); SET_STR(v, kex); SET_STR(v, hostkey_algo);
        SET_HEX(v, V_C); SET_HEX(v, V_S); SET_HEX(v, I_C); SET_HEX(v, I_S);
        SET_HEX(v, K_S); SET_HEX(v, e); SET_HEX(v, f); SET_HEX(v, shared);
        SET_HEX(v, K); SET_HEX(v, H); SET_HEX(v, sig_blob);
        SET_HEX(v, iv_c2s); SET_HEX(v, iv_s2c); SET_HEX(v, key_c2s); SET_HEX(v, key_s2c);
        SET_HEX(v, mac_c2s); SET_HEX(v, mac_s2c);
        SET_HEX(v, accept_wire); SET_HEX(v, accept_payload);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setglobal(L, "HANDSHAKES");

    lua_newtable(L);
    for (size_t i = 0; i < sizeof SSH_SIGNATURES / sizeof SSH_SIGNATURES[0]; i++) {
        const ssh_signature_t *v = &SSH_SIGNATURES[i];
        lua_newtable(L);
        SET_STR(v, name); SET_STR(v, hostkey_algo);
        SET_HEX(v, K_S); SET_HEX(v, H); SET_HEX(v, sig_blob);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setglobal(L, "SIGNATURES");
}

// ── A socket as the app sees one (lua_bridge_tcp.c) ─────────────────────────
// The bridge's semantics, kept deliberately: read returns nil when nothing has
// arrived, data the peer sent before closing stays readable, and isConnected
// goes false as soon as the peer has closed, even with that data still unread.

#define CONN_MT "test.conn"

typedef struct {
    int fd;
    pid_t pid;
    bool peer_closed;
    char *rx;
    size_t rx_len;
} conn_t;

static void conn_pump(conn_t *c) {
    while (!c->peer_closed && c->fd >= 0) {
        char chunk[4096];
        ssize_t n = recv(c->fd, chunk, sizeof chunk, MSG_DONTWAIT);
        if (n > 0) {
            c->rx = realloc(c->rx, c->rx_len + (size_t)n);
            memcpy(c->rx + c->rx_len, chunk, (size_t)n);
            c->rx_len += (size_t)n;
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            c->peer_closed = true;
        } else {
            return;
        }
    }
}

static int l_conn_is_connected(lua_State *L) {
    conn_t *c = luaL_checkudata(L, 1, CONN_MT);
    conn_pump(c);
    lua_pushboolean(L, !c->peer_closed);
    return 1;
}

static int l_conn_read(lua_State *L) {
    conn_t *c = luaL_checkudata(L, 1, CONN_MT);
    size_t max = (size_t)luaL_optinteger(L, 2, 4096);
    conn_pump(c);
    if (c->rx_len == 0) {
        lua_pushnil(L);
        return 1;
    }
    size_t n = c->rx_len < max ? c->rx_len : max;
    lua_pushlstring(L, c->rx, n);
    memmove(c->rx, c->rx + n, c->rx_len - n);
    c->rx_len -= n;
    return 1;
}

static int l_conn_write(lua_State *L) {
    conn_t *c = luaL_checkudata(L, 1, CONN_MT);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(c->fd, data + done, len - done, MSG_NOSIGNAL);
        if (n < 0) {
            lua_pushinteger(L, -1);
            return 1;
        }
        done += (size_t)n;
    }
    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

static int l_conn_error(lua_State *L) {
    lua_pushnil(L);
    return 1;
}

static void reap(pid_t pid) {
    // sshd exits on EOF; give it a moment, then make sure.
    for (int i = 0; i < 200; i++) {
        if (waitpid(pid, NULL, WNOHANG) != 0) return;
        struct timespec ts = {0, 10000000L};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void conn_close(conn_t *c) {
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    if (c->pid > 0) {
        reap(c->pid);
        c->pid = 0;
    }
    free(c->rx);
    c->rx = NULL;
    c->rx_len = 0;
}

static int l_conn_close(lua_State *L) {
    conn_close(luaL_checkudata(L, 1, CONN_MT));
    return 0;
}

static const luaL_Reg s_conn_methods[] = {
    {"isConnected", l_conn_is_connected},
    {"read", l_conn_read},
    {"write", l_conn_write},
    {"error", l_conn_error},
    {"close", l_conn_close},
    {NULL, NULL},
};

// ── The real sshd ───────────────────────────────────────────────────────────

static char s_tmpdir[256];
static char s_sshd[256];
static char s_keygen[256];

// sshd wants an absolute path, and is usually outside a user's PATH.
static bool find_tool(const char *name, char *out, size_t cap) {
    static const char *const dirs[] = {"/usr/sbin", "/usr/bin", "/sbin", "/bin", "/usr/local/sbin"};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(out, cap, "%s/%s", dirs[i], name);
        if (access(out, X_OK) == 0) return true;
    }
    return false;
}

// Run argv with stdin, stdout and stderr on the given descriptors. Returns its pid.
static pid_t spawn(char *const argv[], int in, int out, int err) {
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in, 0);
        dup2(out, 1);
        dup2(err, 2);
        for (int fd = 3; fd < 64; fd++) close(fd);
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static int s_devnull = -1;

static bool run_quiet(char *const argv[]) {
    pid_t pid = spawn(argv, s_devnull, s_devnull, s_devnull);
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// argv for sshd with one host key, one key exchange and one host key algorithm, and
// no configuration file. `extra` is one more option, or NULL.
static void sshd_argv(char **argv, char opts[][128], const char *mode, const char *key,
                      const char *kex, const char *algo, const char *extra) {
    snprintf(opts[0], sizeof opts[0], "KexAlgorithms=%s", kex);
    snprintf(opts[1], sizeof opts[1], "HostKeyAlgorithms=%s", algo);
    int n = 0;
    argv[n++] = s_sshd;
    argv[n++] = (char *)mode;
    argv[n++] = "-f";
    argv[n++] = "/dev/null";
    argv[n++] = "-h";
    argv[n++] = (char *)key;
    argv[n++] = "-o";
    argv[n++] = "PidFile=none";
    argv[n++] = "-o";
    argv[n++] = "UsePAM=no";
    argv[n++] = "-o";
    argv[n++] = opts[0];
    argv[n++] = "-o";
    argv[n++] = opts[1];
    if (extra) {
        argv[n++] = "-o";
        argv[n++] = (char *)extra;
    }
    if (getenv("SSHD_LOG")) {
        argv[n++] = "-e";
        argv[n++] = "-o";
        argv[n++] = "LogLevel=DEBUG2";
    }
    argv[n] = NULL;
}

// keygen(type, bits) -> path of a new throwaway host key
static int l_keygen(lua_State *L) {
    const char *type = luaL_checkstring(L, 1);
    const char *bits = luaL_checkstring(L, 2);
    char path[300];
    snprintf(path, sizeof path, "%s/%s_key", s_tmpdir, type);
    char *argv[] = {s_keygen, "-q", "-N", "", "-t", (char *)type, "-b", (char *)bits, "-f", path, NULL};
    if (!run_quiet(argv)) return luaL_error(L, "ssh-keygen -t %s failed", type);
    lua_pushstring(L, path);
    return 1;
}

// sshd_usable(key, kex, hostkey_algo): does sshd accept this configuration as this user?
static int l_sshd_usable(lua_State *L) {
    char *argv[24];
    char opts[2][128];
    sshd_argv(argv, opts, "-t", luaL_checkstring(L, 1), luaL_checkstring(L, 2),
              luaL_checkstring(L, 3), NULL);
    lua_pushboolean(L, run_quiet(argv));
    return 1;
}

// sshd_connect(key, kex, hostkey_algo [, option]) -> a connection to a new sshd -i
static int l_sshd_connect(lua_State *L) {
    char *argv[24];
    char opts[2][128];
    sshd_argv(argv, opts, "-i", luaL_checkstring(L, 1), luaL_checkstring(L, 2),
              luaL_checkstring(L, 3), luaL_optstring(L, 4, NULL));

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return luaL_error(L, "socketpair failed");
    conn_t *c = lua_newuserdatauv(L, sizeof *c, 0);
    memset(c, 0, sizeof *c);
    c->fd = sv[0];
    luaL_setmetatable(L, CONN_MT);
    const char *log_path = getenv("SSHD_LOG");
    int log_fd = log_path ? open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    c->pid = spawn(argv, sv[1], sv[1], log_fd >= 0 ? log_fd : s_devnull);
    close(sv[1]);
    if (log_fd >= 0) close(log_fd);
    if (c->pid < 0) return luaL_error(L, "fork failed");
    return 1;
}

static const luaL_Reg s_live_funcs[] = {
    {"keygen", l_keygen},
    {"sshd_usable", l_sshd_usable},
    {"sshd_connect", l_sshd_connect},
    {NULL, NULL},
};

// ── Running it ──────────────────────────────────────────────────────────────

static lua_State *new_state(void) {
    g_api.crypto = &s_crypto;

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    lua_newtable(L);                  // picocalc
    lua_bridge_crypto_init(L);        // picocalc.crypto: the real bridge
    lua_newtable(L);
    lua_pushcfunction(L, l_get_time_ms);
    lua_setfield(L, -2, "getTimeMs");
    lua_pushcfunction(L, l_sleep);
    lua_setfield(L, -2, "sleep");
    lua_setfield(L, -2, "sys");
    lua_newtable(L);                  // only main.lua opens sockets
    lua_setfield(L, -2, "tcp");
    lua_setglobal(L, "picocalc");

    lua_pushcfunction(L, l_check);
    lua_setglobal(L, "check");

    lua_getglobal(L, "package");
    lua_pushstring(L, PICODECK_ROOT "/apps/ssh/?.lua");
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
    return L;
}

static void run_script(lua_State *L) {
    if (luaL_dofile(L, PICODECK_ROOT "/tests/unit/test_ssh_transport.lua") != 0) {
        printf("FAIL test_ssh_transport.lua: %s\n", lua_tostring(L, -1));
        s_check_fails++;
    }
}

// Skip (77) rather than fail when this machine cannot run the live test at all.
static int run_live(lua_State *L) {
    if (!find_tool("sshd", s_sshd, sizeof s_sshd) ||
        !find_tool("ssh-keygen", s_keygen, sizeof s_keygen)) {
        printf("SKIP test_ssh_transport --live: no sshd or ssh-keygen\n");
        return EXIT_SKIP;
    }
    const char *base = getenv("TMPDIR");
    snprintf(s_tmpdir, sizeof s_tmpdir, "%s/picodeck-ssh-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(s_tmpdir)) {
        printf("SKIP test_ssh_transport --live: no temporary directory\n");
        return EXIT_SKIP;
    }

    luaL_newmetatable(L, CONN_MT);
    lua_newtable(L);
    luaL_setfuncs(L, s_conn_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, s_live_funcs, 0);
    lua_setglobal(L, "LIVE");
    run_script(L);

    // Whether the script ran or skipped, the throwaway keys go.
    const char *sub[] = {"ecdsa_key", "ecdsa_key.pub", "rsa_key", "rsa_key.pub"};
    for (size_t i = 0; i < sizeof sub / sizeof sub[0]; i++) {
        char path[300];
        snprintf(path, sizeof path, "%s/%s", s_tmpdir, sub[i]);
        unlink(path);
    }
    rmdir(s_tmpdir);

    lua_getglobal(L, "LIVE_SKIPPED");
    bool skipped = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (skipped) {
        printf("SKIP test_ssh_transport --live: this sshd will not run as this user\n");
        return EXIT_SKIP;
    }
    return check_report("test_ssh_transport --live");
}

int main(int argc, char **argv) {
    bool live = argc > 1 && strcmp(argv[1], "--live") == 0;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    s_devnull = open("/dev/null", O_RDWR);

    lua_State *L = new_state();
    int rc;
    if (live) {
        rc = run_live(L);
    } else {
        push_vectors(L);
        run_script(L);
        rc = check_report("test_ssh_transport");
    }
    lua_close(L);
    return rc;
}
