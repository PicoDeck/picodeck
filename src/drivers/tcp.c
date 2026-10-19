#include "tcp.h"
#include "wifi.h"
#include "mongoose.h"
#include "umm_malloc.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// RP2350 XIP cache coherency — see comment in http.c
#ifndef PSRAM_UNCACHED_OFFSET  // SIM_FIRMWARE_NET: 0 (host heap has no alias)
#define PSRAM_UNCACHED_OFFSET 0x04000000u
#endif
static inline uint8_t *rx_buf_uncached(const uint8_t *cached_ptr) {
  return (uint8_t *)((uintptr_t)cached_ptr + PSRAM_UNCACHED_OFFSET);
}

// TCP_CB_* come from os.h (tcp_event_t), shared with native apps.

static tcp_conn_t s_conns[TCP_MAX_CONNECTIONS];
static spin_lock_t *s_lock;  // the pool lock (see tcp.h)

// Unconsumed bytes in Mongoose past which reading pauses (tcp_drain).
#define TCP_RECV_PAUSE (16u * 1024u)

// How long tcp_alloc waits for a slot that is being released.
#define TCP_ALLOC_WAIT_MS 100

static inline tcp_conn_state_t st_get(const tcp_conn_t *c) {
    return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
}
static inline void st_put(tcp_conn_t *c, tcp_conn_state_t s) {
    __atomic_store_n(&c->state, s, __ATOMIC_RELEASE);
}
static inline bool st_released(tcp_conn_state_t s) {
    return s == TCP_STATE_CLOSING || s == TCP_STATE_RELEASED;
}
static inline uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}
static inline bool deadline_passed(uint32_t now, uint32_t deadline) {
    return deadline != 0 && (int32_t)(now - deadline) > 0;
}

// ── Pool (Core 0) ────────────────────────────────────────────────────────────

void tcp_init(void) {
    memset(s_conns, 0, sizeof(s_conns));
    if (!s_lock)
        s_lock = spin_lock_instance(spin_lock_claim_unused(true));
}

tcp_conn_t *tcp_get_conn(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNECTIONS) return NULL;
    tcp_conn_t *c = &s_conns[idx];
    uint32_t save = spin_lock_blocking(s_lock);
    bool used = c->in_use;
    spin_unlock(s_lock, save);
    return used ? c : NULL;
}

static bool any_releasing(void);

// Claim a free slot for buf (NULL if none).
static tcp_conn_t *claim_slot(uint8_t *buf) {
    tcp_conn_t *got = NULL;
    uint32_t save = spin_lock_blocking(s_lock);
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &s_conns[i];
        if (!c->in_use) {
            memset(c, 0, sizeof(*c));
            c->rx_buf = buf;
            c->rx_cap = TCP_RECV_BUF_DEFAULT;
            c->connect_timeout_ms = 15000;
            c->read_timeout_ms = 0;  // disabled by default
            c->in_use = true;
            got = c;
            break;
        }
    }
    spin_unlock(s_lock, save);
    return got;
}

tcp_conn_t *tcp_alloc(void) {
    uint8_t *buf = umm_malloc(TCP_RECV_BUF_DEFAULT);
    if (!buf) return NULL;
    tcp_reap();
    tcp_conn_t *got = claim_slot(buf);
    // Pool full but a socket is being released (closed just now): Core 1
    // acknowledges within a tick or two, so wait briefly rather than fail
    // a close-then-reopen.
    uint32_t start = now_ms();
    while (!got && any_releasing() && now_ms() - start < TCP_ALLOC_WAIT_MS) {
        sleep_ms(1);
        tcp_reap();
        got = claim_slot(buf);
    }
    if (!got) umm_free(buf);
    return got;
}

static void push_close(tcp_conn_t *c) {
    conn_req_t req = {.type = CONN_REQ_TCP_CLOSE, .conn = (http_conn_t *)c};
    c->close_queued = wifi_req_push(&req);
}

void tcp_free(tcp_conn_t *c) {
    if (!c) return;
    uint32_t save = spin_lock_blocking(s_lock);
    if (!c->in_use || st_released(st_get(c))) {
        spin_unlock(s_lock, save);
        return;
    }
    st_put(c, TCP_STATE_CLOSING);
    c->pending = 0;
    spin_unlock(s_lock, save);
    c->lua_ud = NULL;
    push_close(c);  // on failure the slot stays CLOSING; tcp_reap retries
}

void tcp_close(tcp_conn_t *c) { tcp_free(c); }

void tcp_reap(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &s_conns[i];
        uint8_t *buf = NULL;
        bool retry = false;
        uint32_t save = spin_lock_blocking(s_lock);
        if (c->in_use) {
            tcp_conn_state_t st = st_get(c);
            if (st == TCP_STATE_RELEASED) {
                buf = c->rx_buf;
                memset(c, 0, sizeof(*c));  // in_use = false
            } else if (st == TCP_STATE_CLOSING && !c->close_queued) {
                retry = true;
            }
        }
        spin_unlock(s_lock, save);
        if (buf) umm_free(buf);
        if (retry) push_close(c);
    }
}

static bool any_releasing(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &s_conns[i];
        uint32_t save = spin_lock_blocking(s_lock);
        bool r = c->in_use && st_released(st_get(c));
        spin_unlock(s_lock, save);
        if (r) return true;
    }
    return false;
}

void tcp_close_all(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = tcp_get_conn(i);
        if (c) tcp_free(c);
    }
    uint32_t start = now_ms();
    for (;;) {
        tcp_reap();
        if (!any_releasing() || now_ms() - start >= 500) break;
        sleep_ms(5);
    }
    if (any_releasing())
        printf("[TCP] close_all: Core 1 has not released every slot yet\n");
}

// ── Requests (Core 0) ────────────────────────────────────────────────────────

bool tcp_connect(tcp_conn_t *c, const char *host, uint16_t port, bool use_ssl) {
    if (!c || !host || !wifi_is_available()) return false;
    tcp_conn_state_t st = st_get(c);
    if (st != TCP_STATE_IDLE && st != TCP_STATE_CLOSED &&
        st != TCP_STATE_FAILED)
        return false;  // in progress, connected, or released
    if (host != c->host) {  // Lua passes the slot's own buffer
        strncpy(c->host, host, sizeof(c->host) - 1);
        c->host[sizeof(c->host) - 1] = '\0';
    }
    c->port = port;
    c->use_ssl = use_ssl;
    c->err[0] = '\0';
    c->fingerprint[0] = '\0';

    uint32_t save = spin_lock_blocking(s_lock);
    c->pending = 0;
    c->rx_head = c->rx_tail = c->rx_count = 0;
    c->deadline_connect = now_ms() + c->connect_timeout_ms;
    c->deadline_read = 0;
    st_put(c, TCP_STATE_QUEUED);
    spin_unlock(s_lock, save);

    conn_req_t req = {.type = CONN_REQ_TCP_CONNECT, .conn = (http_conn_t *)c};
    if (wifi_req_push(&req)) return true;
    // Core 1 never saw it.
    snprintf(c->err, sizeof(c->err), "request queue full");
    save = spin_lock_blocking(s_lock);
    if (st_get(c) == TCP_STATE_QUEUED) st_put(c, TCP_STATE_FAILED);
    spin_unlock(s_lock, save);
    return false;
}

int tcp_write(tcp_conn_t *c, const void *buf, int len) {
    if (!c || !buf || len < 0 || st_get(c) != TCP_STATE_CONNECTED) return -1;
    if (len == 0) return 0;

    void *copy = umm_malloc(len);
    if (!copy) return -1;
    memcpy(copy, buf, len);

    conn_req_t req = {
        .type = CONN_REQ_TCP_WRITE,
        .conn = (http_conn_t*)c,
        .data = copy,
        .data_len = (uint32_t)len
    };

    if (!wifi_req_push(&req)) {
        umm_free(copy);
        return -1;
    }
    return len;
}

int tcp_read(tcp_conn_t *c, void *buf, int len) {
    if (!c || !buf || len <= 0) return 0;

    uint32_t save = spin_lock_blocking(s_lock);
    if (c->rx_count == 0) {
        spin_unlock(s_lock, save);
        return 0;
    }
    uint32_t n = ((uint32_t)len < c->rx_count) ? (uint32_t)len : c->rx_count;

    // Read through uncached alias to see Core 1's writes to physical PSRAM
    const uint8_t *uc = rx_buf_uncached(c->rx_buf);
    uint32_t till_end = c->rx_cap - c->rx_tail;
    if (n <= till_end) {
        memcpy(buf, &uc[c->rx_tail], n);
        c->rx_tail += n;
        if (c->rx_tail == c->rx_cap) c->rx_tail = 0;
    } else {
        memcpy(buf, &uc[c->rx_tail], till_end);
        memcpy((uint8_t*)buf + till_end, uc, n - till_end);
        c->rx_tail = n - till_end;
    }
    c->rx_count -= n;
    spin_unlock(s_lock, save);
    return (int)n;
}

uint32_t tcp_bytes_available(tcp_conn_t *c) {
    if (!c) return 0;
    uint32_t save = spin_lock_blocking(s_lock);
    uint32_t n = c->rx_count;
    spin_unlock(s_lock, save);
    return n;
}

const char *tcp_get_error(tcp_conn_t *c) {
    if (!c) return NULL;
    // err is written before the state store that publishes it, and never
    // while the slot is in one of these states.
    tcp_conn_state_t st = st_get(c);
    if (st != TCP_STATE_FAILED && st != TCP_STATE_CLOSED &&
        st != TCP_STATE_IDLE)
        return NULL;
    return c->err[0] ? c->err : NULL;
}

tcp_conn_state_t tcp_get_state(tcp_conn_t *c) {
    return c ? st_get(c) : TCP_STATE_IDLE;
}

uint32_t tcp_take_pending_bits(tcp_conn_t *c, uint32_t mask) {
    if (!c) return 0;
    uint32_t save = spin_lock_blocking(s_lock);
    uint32_t p = c->pending & mask;
    c->pending &= ~mask;
    spin_unlock(s_lock, save);
    return p;
}

uint32_t tcp_take_pending(tcp_conn_t *c) {
    return tcp_take_pending_bits(c, 0xFFFFFFFFu);
}

void tcp_set_connect_timeout(tcp_conn_t *c, uint32_t ms) {
    if (!c) return;
    uint32_t save = spin_lock_blocking(s_lock);  // Core 1 prints it on timeout
    c->connect_timeout_ms = ms;
    spin_unlock(s_lock, save);
}

void tcp_set_read_timeout(tcp_conn_t *c, uint32_t ms) {
    if (!c) return;
    uint32_t save = spin_lock_blocking(s_lock);
    c->read_timeout_ms = ms;
    // Already connected: arm (or disarm) it now rather than at the next byte.
    if (st_get(c) == TCP_STATE_CONNECTED)
        c->deadline_read = ms ? now_ms() + ms : 0;
    spin_unlock(s_lock, save);
}

// ── Core 1 ───────────────────────────────────────────────────────────────────

// The one place a connection is dropped: clear fn_data so no later event
// reaches the slot, and let Mongoose close it.
static void tcp_c1_detach(tcp_conn_t *c) {
    struct mg_connection *nc = (struct mg_connection *)c->pcb;
    if (!nc) return;
    nc->fn_data = NULL;
    nc->is_closing = 1;
    c->pcb = NULL;
}

bool tcp_c1_begin_connect(tcp_conn_t *c) {
    bool ok = false;
    uint32_t save = spin_lock_blocking(s_lock);
    if (st_get(c) == TCP_STATE_QUEUED) {
        st_put(c, TCP_STATE_CONNECTING);
        ok = true;
    }
    spin_unlock(s_lock, save);
    if (ok) tcp_c1_detach(c);  // a previous connect's socket, if any
    return ok;
}

// Move to FAILED under the lock unless the slot is being released.  The
// error text is written first; the state store publishes it.
static bool c1_fail_locked(tcp_conn_t *c, const char *msg) {
    bool changed = false;
    uint32_t save = spin_lock_blocking(s_lock);
    tcp_conn_state_t st = st_get(c);
    if (!st_released(st) && st != TCP_STATE_FAILED) {
        snprintf(c->err, sizeof(c->err), "%s", msg);
        c->deadline_connect = 0;
        c->deadline_read = 0;
        st_put(c, TCP_STATE_FAILED);
        c->pending |= TCP_CB_FAILED;
        changed = true;
    }
    spin_unlock(s_lock, save);
    return changed;
}

void tcp_c1_fail(tcp_conn_t *c, const char *msg) {
    if (c1_fail_locked(c, msg))
        printf("[TCP] %s\n", msg);
    tcp_c1_detach(c);
}

void tcp_c1_send(tcp_conn_t *c, const void *data, uint32_t len) {
    struct mg_connection *nc = (struct mg_connection *)c->pcb;
    if (!nc || st_get(c) != TCP_STATE_CONNECTED) return;  // closed meanwhile
    if (!mg_send(nc, data, len))
        tcp_c1_fail(c, "send failed (out of memory)");
}

void tcp_c1_release(tcp_conn_t *c) {
    if (st_get(c) != TCP_STATE_CLOSING) return;  // not a release request
    tcp_c1_detach(c);
    uint32_t save = spin_lock_blocking(s_lock);
    st_put(c, TCP_STATE_RELEASED);  // Core 1 never touches the slot again
    spin_unlock(s_lock, save);
}

void tcp_c1_fail_all(const char *msg) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &s_conns[i];
        uint32_t save = spin_lock_blocking(s_lock);
        tcp_conn_state_t st = st_get(c);
        bool live = c->in_use &&
                    (st == TCP_STATE_QUEUED || st == TCP_STATE_CONNECTING ||
                     st == TCP_STATE_CONNECTED);
        spin_unlock(s_lock, save);
        if (live) tcp_c1_fail(c, msg);
    }
}

// Move what fits from nc->recv into the ring.  Runs on MG_EV_READ and on
// every MG_EV_POLL, so data left behind while the ring was full moves once
// the app has read.
static void tcp_drain(struct mg_connection *nc, tcp_conn_t *c) {
    struct mg_iobuf *io = &nc->recv;
    if (io->len == 0) { nc->is_full = 0; return; }

    uint32_t save = spin_lock_blocking(s_lock);
    if (st_released(st_get(c))) {
        spin_unlock(s_lock, save);
        return;
    }
    uint32_t space = c->rx_cap - c->rx_count;
    uint32_t len = (uint32_t)io->len;
    if (len > space) len = space;

    if (len > 0) {
        // Write through uncached alias to bypass Core 1's XIP cache
        uint8_t *uc = rx_buf_uncached(c->rx_buf);
        uint32_t till_end = c->rx_cap - c->rx_head;
        if (len <= till_end) {
            memcpy(&uc[c->rx_head], io->buf, len);
            c->rx_head += len;
            if (c->rx_head == c->rx_cap) c->rx_head = 0;
        } else {
            memcpy(&uc[c->rx_head], io->buf, till_end);
            memcpy(uc, io->buf + till_end, len - till_end);
            c->rx_head = len - till_end;
        }
        c->rx_count += len;
        c->pending |= TCP_CB_READ;
    }
    // Data is arriving (even if the ring is full): not idle.
    if (c->read_timeout_ms > 0 && st_get(c) == TCP_STATE_CONNECTED)
        c->deadline_read = now_ms() + c->read_timeout_ms;
    spin_unlock(s_lock, save);

    if (len > 0) mg_iobuf_del(io, 0, len);
    // Stop reading while the app lags (honoured by the socket build only;
    // see HTTP_RECV_PAUSE in http.c).
    nc->is_full = io->len >= TCP_RECV_PAUSE;
}

void tcp_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
    tcp_conn_t *c = (tcp_conn_t *)nc->fn_data;
    if (!c) return;
    // An older connection of this slot (it was detached, so this should not
    // happen; belt and braces).
    if (c->pcb != NULL && c->pcb != nc) return;

    // A TLS socket is CONNECTED (writable, TCP_CB_CONNECT) only after the
    // handshake and certificate check (MG_EV_TLS_HS); until then the connect
    // timeout keeps running, so a stalled handshake fails like a stalled
    // connect.  Plain TCP is CONNECTED at MG_EV_CONNECT.
    if ((ev == MG_EV_CONNECT && !nc->is_tls) || ev == MG_EV_TLS_HS) {
        // The peer certificate's fingerprint, before the CONNECTED store
        // publishes it (lua_bridge_tcp.c reads it only after that store).
        if (ev == MG_EV_TLS_HS && st_get(c) == TCP_STATE_CONNECTING)
            wifi_tls_peer_fingerprint(nc, c->fingerprint, sizeof(c->fingerprint));
        uint32_t save = spin_lock_blocking(s_lock);
        if (st_get(c) == TCP_STATE_CONNECTING) {
            c->deadline_connect = 0;
            c->deadline_read = c->read_timeout_ms
                                   ? now_ms() + c->read_timeout_ms : 0;
            st_put(c, TCP_STATE_CONNECTED);
            c->pending |= TCP_CB_CONNECT;
        }
        spin_unlock(s_lock, save);
    } else if (ev == MG_EV_READ || ev == MG_EV_POLL) {
        tcp_drain(nc, c);
    } else if (ev == MG_EV_ERROR) {
        char msg[TCP_ERR_MAX];
        if (!wifi_tls_verify_error(nc, msg, sizeof(msg)))
            snprintf(msg, sizeof(msg), "Mongoose error: %s", (char *)ev_data);
        // Mongoose closes nc itself after an error; keep pcb until
        // MG_EV_CLOSE so that event still reaches this slot.
        if (c1_fail_locked(c, msg)) printf("[TCP] %s\n", msg);
    } else if (ev == MG_EV_CLOSE) {
        uint32_t save = spin_lock_blocking(s_lock);
        tcp_conn_state_t st = st_get(c);
        if (!st_released(st)) {
            if (st != TCP_STATE_FAILED) st_put(c, TCP_STATE_CLOSED);
            c->pending |= TCP_CB_CLOSED;
            c->deadline_connect = c->deadline_read = 0;
        }
        spin_unlock(s_lock, save);
        c->pcb = NULL;
    }
}

void tcp_check_timeouts(void) {
    uint32_t now = now_ms();
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &s_conns[i];
        char msg[TCP_ERR_MAX];
        msg[0] = '\0';
        uint32_t save = spin_lock_blocking(s_lock);
        if (c->in_use) {
            tcp_conn_state_t st = st_get(c);
            if ((st == TCP_STATE_QUEUED || st == TCP_STATE_CONNECTING) &&
                deadline_passed(now, c->deadline_connect)) {
                snprintf(msg, sizeof(msg), "connect timeout (%ums)",
                         (unsigned)c->connect_timeout_ms);
            } else if (st == TCP_STATE_CONNECTED &&
                       deadline_passed(now, c->deadline_read)) {
                snprintf(msg, sizeof(msg), "read timeout (%ums)",
                         (unsigned)c->read_timeout_ms);
            }
        }
        spin_unlock(s_lock, save);
        if (msg[0]) tcp_c1_fail(c, msg);
    }
}
