// Web (Emscripten) network stubs.
//
// The browser has no raw sockets and libcurl does not exist there, so the web
// demo ships without networking: WiFi reports "not available" and every HTTP /
// TCP allocation fails, which the Lua bridges already surface as a nil + error
// ("connection pool full or out of memory"). Replaces sim_http.c + sim_tcp.c;
// sim_wifi.c is kept and calls into the sim_* hooks below.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os.h"
#include "drivers/http.h"
#include "drivers/tcp.h"

// ── Hooks sim_wifi.c expects ─────────────────────────────────────────────────

bool sim_wifi_is_available(void) { return false; }
bool sim_network_blocked(void) { return true; }
int sim_wifi_forced_status(void) { return -1; }

void sim_http_start(http_conn_t *c) { (void)c; }
void sim_http_close_handle(http_conn_t *c) { (void)c; }

void sim_tcp_start_connect(tcp_conn_t *c) { (void)c; }
void sim_tcp_do_write(tcp_conn_t *c, void *data, uint32_t len) { (void)c; (void)data; (void)len; }
void sim_tcp_do_close(tcp_conn_t *c) { (void)c; }
void sim_tcp_poll(void) {}

// ── HTTP (src/drivers/http.h) ────────────────────────────────────────────────

void http_init(void) {}
http_conn_t *http_alloc(void) { return NULL; }
void http_free(http_conn_t *c) { (void)c; }
void http_close(http_conn_t *c) { (void)c; }
void http_close_all(void (*on_free)(void *lua_ud)) { (void)on_free; }
bool http_set_recv_buf(http_conn_t *c, uint32_t bytes) { (void)c; (void)bytes; return false; }
bool http_get(http_conn_t *c, const char *path, const char *extra_hdr) {
    (void)c; (void)path; (void)extra_hdr;
    return false;
}
bool http_post(http_conn_t *c, const char *path, const char *extra_hdr,
               const char *body, size_t body_len) {
    (void)c; (void)path; (void)extra_hdr; (void)body; (void)body_len;
    return false;
}
uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len) { (void)c; (void)out; (void)len; return 0; }
uint32_t http_bytes_available(http_conn_t *c) { (void)c; return 0; }
http_conn_t *http_get_conn(int idx) { (void)idx; return NULL; }
uint8_t http_take_pending(http_conn_t *c) { (void)c; return 0; }
void http_poll(void) {}
void http_check_timeouts(void) {}
void http_fire_c_pending(void) {}
void http_reap(void) {}
http_state_t http_get_state(http_conn_t *c) { (void)c; return HTTP_STATE_FAILED; }
const char *http_get_error(http_conn_t *c) { (void)c; return "Networking is not available in the web demo"; }
bool http_headers_ready(http_conn_t *c) { (void)c; return false; }
int http_get_status(http_conn_t *c) { (void)c; return 0; }
void http_get_progress(http_conn_t *c, int *received, int *total) {
    (void)c;
    if (received) *received = 0;
    if (total) *total = 0;
}
bool http_is_complete(http_conn_t *c) { (void)c; return true; }

// ── TCP (src/drivers/tcp.h) ──────────────────────────────────────────────────

void tcp_init(void) {}
tcp_conn_t *tcp_alloc(void) { return NULL; }
tcp_conn_t *tcp_get_conn(int idx) { (void)idx; return NULL; }
void tcp_free(tcp_conn_t *c) { (void)c; }
bool tcp_connect(tcp_conn_t *c, const char *host, uint16_t port, bool use_ssl) {
    (void)c; (void)host; (void)port; (void)use_ssl;
    return false;
}
int tcp_write(tcp_conn_t *c, const void *buf, int len) { (void)c; (void)buf; (void)len; return -1; }
int tcp_read(tcp_conn_t *c, void *buf, int len) { (void)c; (void)buf; (void)len; return 0; }
void tcp_close(tcp_conn_t *c) { (void)c; }
uint32_t tcp_bytes_available(tcp_conn_t *c) { (void)c; return 0; }
const char *tcp_get_error(tcp_conn_t *c) { (void)c; return "Networking is not available in the web demo"; }
uint32_t tcp_take_pending(tcp_conn_t *c) { (void)c; return 0; }
void tcp_check_timeouts(void) {}
void tcp_reap(void) {}
void tcp_close_all(void) {}
tcp_conn_state_t tcp_get_state(tcp_conn_t *c) { (void)c; return TCP_STATE_FAILED; }
uint32_t tcp_take_pending_bits(tcp_conn_t *c, uint32_t mask) { (void)c; (void)mask; return 0; }
void tcp_set_connect_timeout(tcp_conn_t *c, uint32_t ms) { (void)c; (void)ms; }
void tcp_set_read_timeout(tcp_conn_t *c, uint32_t ms) { (void)c; (void)ms; }
