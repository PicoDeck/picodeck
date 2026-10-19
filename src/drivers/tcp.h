#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/sync.h"

// =============================================================================
// TCP client over Mongoose for PicoDeck
//
// Non-blocking, cross-core: Core 0 (apps) owns the slot and requests work
// through the wifi.c request ring; Core 1 owns the Mongoose connection.
//
// Slot lifetime (the close protocol)
// ----------------------------------
//   IDLE ─connect→ QUEUED ─Core 1→ CONNECTING → CONNECTED ─→ CLOSED / FAILED
//     any in-use state ──tcp_free (Core 0)──→ CLOSING
//     CLOSING ──CONN_REQ_TCP_CLOSE (Core 1)──→ RELEASED
//     RELEASED ──tcp_reap (Core 0)──→ slot free (rx_buf freed, slot zeroed)
//
// * tcp_free() only marks the slot CLOSING and queues CONN_REQ_TCP_CLOSE; it
//   never frees memory.  If the request ring is full the slot stays CLOSING
//   and tcp_reap() retries the push.
// * Core 1's close handler (tcp_c1_release) clears nc->fn_data, marks the
//   Mongoose connection closing and only then stores RELEASED (release
//   order, under the pool lock).  From then on no Core 1 code touches the
//   slot: fn_data is gone, and every Core 1 state change refuses to leave
//   CLOSING/RELEASED.
// * Core 0 frees rx_buf and zeroes the slot only after it observes RELEASED:
//   tcp_reap(), called from the Lua hook path (tcp_lua_fire_pending), the
//   native sys->poll, tcp_alloc and tcp_close_all.
// * Every Core 1 path that drops pcb also clears fn_data (tcp_c1_detach).
//
// Locking: ONE spinlock for the whole pool (claimed once in tcp_init) guards
// state transitions, pending bits, the receive ring and in_use.  `state` is
// additionally accessed with atomic load/store so either core may read it
// without the lock.
// =============================================================================

#define TCP_MAX_CONNECTIONS   4
#define TCP_RECV_BUF_DEFAULT  8192
#define TCP_ERR_MAX           128

typedef enum {
    TCP_STATE_IDLE = 0,
    TCP_STATE_QUEUED,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_CLOSED,
    TCP_STATE_FAILED,
    TCP_STATE_CLOSING,    // Core 0 released the slot; CLOSE queued (or retrying)
    TCP_STATE_RELEASED,   // Core 1 detached the connection; Core 0 may reclaim
} tcp_conn_state_t;

typedef struct {
    bool             in_use;
    tcp_conn_state_t state;     // atomic access (tcp_get_state)

    char     host[128];
    uint16_t port;
    bool     use_ssl;
    bool     insecure;  // TLS without certificate checks (opt-in, set before connect)

    char err[TCP_ERR_MAX];      // written before the FAILED/CLOSED store
    // Lowercase hex SHA-256 of the TLS peer certificate (DER), "" when none.
    // Cleared by tcp_connect; written by Core 1 before the CONNECTED store,
    // so Core 0 reads it once tcp_get_state() returns CONNECTED or later.
    char fingerprint[65];
    uint32_t pending;  // TCP_CB_* flags (pool lock)

    uint8_t *rx_buf;
    uint32_t rx_cap;
    uint32_t rx_head;  // pool lock
    uint32_t rx_tail;  // pool lock
    uint32_t rx_count; // pool lock

    // What Mongoose still held when the connection closed and the ring
    // could not take (umm, through the cached alias: see tcp_ev_fn). Core 1 sets
    // it once, before the CLOSED store; tcp_read hands it out after the ring
    // and frees it once read; tcp_connect and tcp_reap free what is left.
    uint8_t *spill;     // pool lock
    uint32_t spill_len; // pool lock
    uint32_t spill_off; // pool lock

    void *pcb;  // mongoose mg_connection — Core 1 only

    uint32_t connect_timeout_ms;  // default 15000 (pool lock)
    uint32_t deadline_connect;    // ms since boot, 0 = not set (pool lock)

    uint32_t read_timeout_ms;    // 0 = disabled (default) (pool lock)
    uint32_t deadline_read;      // ms since boot, 0 = not set (pool lock)

    // Core 0 only
    bool     close_queued;  // CONN_REQ_TCP_CLOSE accepted by the ring
    void    *lua_ud;        // Lua bridge userdata (NULL once released)
} tcp_conn_t;

void tcp_init(void);
tcp_conn_t *tcp_alloc(void);
tcp_conn_t *tcp_get_conn(int idx);   // NULL unless the slot is in use

// Release the slot (Core 0): marks it CLOSING and queues the close; the
// handle must not be used afterwards.  Idempotent.
void tcp_free(tcp_conn_t *c);
// Same as tcp_free (kept for callers that close, then free).
void tcp_close(tcp_conn_t *c);
// Reclaim RELEASED slots and retry dropped CLOSE requests (Core 0).
void tcp_reap(void);
// Release every in-use slot and wait (bounded) for Core 1 to let them go
// (app teardown).  Slots Core 1 has not acknowledged stay CLOSING.
void tcp_close_all(void);

// Queue a connect.  Only from IDLE, CLOSED or FAILED.  `host` may be c->host.
bool tcp_connect(tcp_conn_t *c, const char *host, uint16_t port, bool use_ssl);
int  tcp_write(tcp_conn_t *c, const void *buf, int len);
int  tcp_read(tcp_conn_t *c, void *buf, int len);
uint32_t tcp_bytes_available(tcp_conn_t *c);
const char *tcp_get_error(tcp_conn_t *c);   // NULL if none
tcp_conn_state_t tcp_get_state(tcp_conn_t *c);
uint32_t tcp_take_pending(tcp_conn_t *c);
// Take and clear only the pending bits in `mask`.
uint32_t tcp_take_pending_bits(tcp_conn_t *c, uint32_t mask);
void tcp_set_connect_timeout(tcp_conn_t *c, uint32_t ms);
void tcp_set_read_timeout(tcp_conn_t *c, uint32_t ms);  // 0 = off

// Check and enforce TCP connect/read timeouts. Called from wifi_poll() on Core 1.
void tcp_check_timeouts(void);

// ── Core 1 only (wifi.c drain_requests) ─────────────────────────────────────
struct mg_connection;
// Mongoose event handler for TCP connections
void tcp_ev_fn(struct mg_connection *nc, int ev, void *ev_data);
// QUEUED → CONNECTING; false (do nothing) if the slot left QUEUED.  Detaches
// any previous connection of the slot first.
bool tcp_c1_begin_connect(tcp_conn_t *c);
// Record an error and move to FAILED (+TCP_CB_FAILED), detaching the
// connection.  No-op once the slot is CLOSING/RELEASED.
void tcp_c1_fail(tcp_conn_t *c, const char *msg);
// Send queued write data; fails the connection if Mongoose cannot buffer it.
void tcp_c1_send(tcp_conn_t *c, const void *data, uint32_t len);
// CONN_REQ_TCP_CLOSE: detach and close the connection, then RELEASED.
void tcp_c1_release(tcp_conn_t *c);
// Fail every live connection (link down).
void tcp_c1_fail_all(const char *msg);
