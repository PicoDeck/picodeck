#pragma once

// =============================================================================
// HTTP/1.1 client over Mongoose for PicoDeck
//
// Non-blocking.  Core 0 (apps) owns the slot and queues work through the
// wifi.c request ring; Core 1 owns the Mongoose connection and runs
// http_ev_fn inside mg_mgr_poll().  Lua callbacks fire from Core 0's hook
// path (http_lua_fire_pending), never from Core 1.
//
// The response is parsed here, not by Mongoose's HTTP handler (connections
// are opened with plain mg_connect): headers, Content-Length, chunked and
// close-delimited bodies, and keep-alive reuse all run through http_ev_fn.
//
// Slot lifetime (the close protocol)
// ----------------------------------
//   IDLE/DONE/FAILED ─start_request→ QUEUED ─Core 1→ CONNECTING → SENDING
//     → HEADERS (request sent, read timeout armed) → BODY → DONE | FAILED
//   any in-use state ──http_free (Core 0)──→ CLOSING
//   CLOSING ──CONN_REQ_HTTP_CLOSE (Core 1, http_c1_release)──→ RELEASED
//   RELEASED ──http_reap (Core 0)──→ slot free (buffers freed, slot zeroed)
//
// * http_free() only marks the slot CLOSING and queues the close.  If the
//   request ring is full it stays CLOSING and http_reap() retries the push.
// * Core 1's close handler clears nc->fn_data, closes the connection, and
//   only then stores RELEASED (release order, under the pool lock); no Core 1
//   code touches the slot after that, and no Core 1 state change leaves
//   CLOSING/RELEASED.  Every path that drops pcb clears fn_data
//   (http_c1_detach): the close handler, timeouts, errors, a new request.
// * Core 0 frees the buffers and zeroes the slot only once it observes
//   RELEASED: http_reap(), from the Lua hook path, the native sys->poll,
//   http_alloc and http_close_all (app teardown).
//
// Locking: ONE spinlock for the whole pool (claimed once in http_init)
// guards state transitions, pending bits, the receive ring (+ spill) and
// in_use.  `state` and `headers_done` are also accessed atomically so Core 0
// can read them without the lock.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/sync.h"

#ifdef WIFI_ENABLED
// No-op for now, Mongoose handles its own includes
#endif

// ── Limits ────────────────────────────────────────────────────────────────────

#define HTTP_MAX_CONNECTIONS   8      // Simultaneous connections
#define HTTP_RECV_BUF_DEFAULT  4096   // Default receive ring buffer
#define HTTP_RECV_BUF_MAX      (2 * 1024 * 1024)  // Max allowed by setReadBufferSize
#define HTTP_HEADER_BUF_MAX    8192   // Raw response header block
#define HTTP_MAX_HDR_ENTRIES   24     // Max parsed header fields
#define HTTP_SERVER_MAX        128    // Hostname buffer
#define HTTP_ERR_MAX           128    // Error string buffer

// ── Pending callback bitmask ──────────────────────────────────────────────────
// Set on Core 1, consumed by http_lua_fire_pending on Core 0.

#define HTTP_CB_REQUEST   (1u << 0)   // Data arrived (fires setRequestCallback)
#define HTTP_CB_HEADERS   (1u << 1)   // Headers parsed (fires setHeadersReadCallback)
#define HTTP_CB_COMPLETE  (1u << 2)   // Body done (fires setRequestCompleteCallback)
#define HTTP_CB_CLOSED    (1u << 3)   // Connection closed (fires setConnectionClosedCallback)
#define HTTP_CB_FAILED    (1u << 4)   // Error occurred (fires setConnectionClosedCallback)

// ── State ─────────────────────────────────────────────────────────────────────

typedef enum {
    HTTP_STATE_IDLE = 0,
    HTTP_STATE_QUEUED,      // Request pushed to Core 1 queue, awaiting processing
    HTTP_STATE_DNS,         // DNS resolution (handled by Mongoose internally)
    HTTP_STATE_CONNECTING,  // TCP connect in progress
    HTTP_STATE_SENDING,     // Sending HTTP request
    HTTP_STATE_HEADERS,     // Receiving and parsing response headers
    HTTP_STATE_BODY,        // Receiving response body
    HTTP_STATE_DONE,        // Request complete
    HTTP_STATE_FAILED,      // Error
    HTTP_STATE_CLOSING,     // Core 0 released the slot; CLOSE queued/retrying
    HTTP_STATE_RELEASED,    // Core 1 let go of it; Core 0 may reclaim
} http_state_t;

// ── Connection struct ─────────────────────────────────────────────────────────

typedef struct {
    bool         in_use;       // pool lock
    http_state_t state;        // atomic (http_get_state); changes under the lock

    // Configuration (Core 0; set before a request is issued)
    char     server[HTTP_SERVER_MAX];
    char    *path;
    char     method[8];
    char    *extra_hdrs;
    uint16_t port;
    bool     use_ssl;
    bool     keep_alive;
    bool     insecure;           // TLS without certificate checks (opt-in)
    int32_t  range_from;         // -1 = not set; the next request sends it
    int32_t  range_to;           // < 0 = to the end
    uint32_t connect_timeout_ms; // 0 = off
    uint32_t read_timeout_ms;    // 0 = off
    uint32_t max_transfer_ms;    // 0 = disabled (default)

    // Snapshot of the configuration taken by start_request: the only copy
    // Core 1 reads, so the setters never race a request in flight.
    bool     req_keep_alive;
    uint32_t req_connect_timeout_ms;
    uint32_t req_read_timeout_ms;
    uint32_t req_max_transfer_ms;

    // Error string (non-empty on failure); written before the state store
    // that publishes it (http_get_error)
    char err[HTTP_ERR_MAX];

    // Pending Lua-callback flags (HTTP_CB_*), pool lock
    uint8_t pending;

    // Response metadata (Core 1 writes; Core 0 reads once headers_done)
    int      status_code;
    char    *hdr_buf;        // HTTP_HEADER_BUF_MAX bytes
    size_t   hdr_len;        // bytes written into hdr_buf
    bool     headers_done;   // atomic (http_headers_ready)
    int32_t  content_length; // -1 = unknown (chunked / close-delimited)
    uint32_t body_received;  // pool lock

    // Body framing (Core 1)
    bool     chunked;        // Transfer-Encoding: chunked
    uint8_t  chunk_state;    // decoder state (http.c)
    uint32_t chunk_left;     // bytes left in the current chunk

    // Parsed header fields (pointers into hdr_buf, null-terminated in place)
    const char *hdr_keys[HTTP_MAX_HDR_ENTRIES];
    const char *hdr_vals[HTTP_MAX_HDR_ENTRIES];
    int         hdr_count;

    // Receive ring buffer (body data), pool lock: Core 1 writes, Core 0 reads
    uint8_t *rx_buf;
    uint32_t rx_cap;   // allocated size
    uint32_t rx_head;  // write index
    uint32_t rx_tail;  // read index
    uint32_t rx_count; // bytes available

    // Body bytes that did not fit the ring when the server closed: kept so
    // the app can still read everything (pool lock; read after the ring)
    uint8_t *spill;
    uint32_t spill_len;
    uint32_t spill_off;

    // Request buffers: allocated and freed by Core 0, read by Core 1 while
    // it sends the request
    char    *tx_buf;
    uint32_t tx_len;

    // Deadline timestamps (ms since boot), pool lock
    uint32_t deadline_connect;
    uint32_t deadline_read;
    uint32_t deadline_transfer;  // hard ceiling for total transfer time

    // Core 0 only
    bool  close_queued;  // CONN_REQ_HTTP_CLOSE accepted by the ring
    void *lua_ud;        // Lua bridge userdata (NULL once released)

    // Internal mongoose connection pointer (Core 1 only)
    void *pcb;
} http_conn_t;

// ── Public API (Core 0) ───────────────────────────────────────────────────────

// Initialize the connection pool and claim its lock. Call once at boot.
void http_init(void);

// Release every connection (app teardown): on_free(lua_ud) is called for
// each one that still has a Lua userdata, then the slot is released and the
// call waits (bounded, 500 ms) for Core 1 to let go.  Slots Core 1 has not
// acknowledged stay CLOSING and are reclaimed later.
void http_close_all(void (*on_free)(void *lua_ud));

// Allocate a connection from the static pool. Returns NULL if pool is full.
// Caller must set conn->server, conn->port before issuing a request.
http_conn_t *http_alloc(void);

// Release a connection (asynchronous, see the close protocol above).  The
// handle must not be used afterwards.  Idempotent.
void http_free(http_conn_t *c);

// Same as http_free (kept for callers that close, then free).
void http_close(http_conn_t *c);

// Reclaim RELEASED slots and retry CLOSE requests the ring dropped.
void http_reap(void);

// Resize the receive ring buffer. Must be called before issuing a request.
// Returns false for 0, over HTTP_RECV_BUF_MAX or out of memory (the old
// buffer stays).
bool http_set_recv_buf(http_conn_t *c, uint32_t bytes);

// Issue an HTTP GET request. extra_hdr: optional "Key: Value\r\n..." string.
// Returns false (nothing queued) unless the slot is IDLE, DONE or FAILED,
// WiFi is present, and the request could be queued.
bool http_get(http_conn_t *c, const char *path, const char *extra_hdr);

// Issue an HTTP POST request with a body (binary safe: body_len bytes).
bool http_post(http_conn_t *c, const char *path, const char *extra_hdr,
               const char *body, size_t body_len);

// For a caller that cannot report http_get/http_post returning false (the native
// API's void get/post, polled with isComplete): a request that never started
// becomes a finished, failed one, with http_get_error saying why. A request
// already running (what the call was refused for) is left alone: it will finish.
void http_fail_unstarted(http_conn_t *c);

// Read up to `len` bytes of body into `out`.  Returns bytes copied.
uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len);

// Body bytes available to http_read.
uint32_t http_bytes_available(http_conn_t *c);

// Return a pool slot by index (0 .. HTTP_MAX_CONNECTIONS-1).
// Returns NULL if the slot is not in use.
http_conn_t *http_get_conn(int idx);

// Atomically read and clear the pending callback bitmask.
uint8_t http_take_pending(http_conn_t *c);

// Race-free views of fields Core 1 writes.
http_state_t http_get_state(http_conn_t *c);
const char  *http_get_error(http_conn_t *c);     // NULL if none
bool         http_headers_ready(http_conn_t *c); // hdr_* / status readable
int          http_get_status(http_conn_t *c);    // 0 until the headers
void         http_get_progress(http_conn_t *c, int *received, int *total);
bool         http_is_complete(http_conn_t *c);   // DONE or FAILED

// Poll Mongoose for network events.
void http_poll(void);

// Fire any pending C-language (non-Lua) HTTP callbacks.
// Called from Core 1 after wifi_poll() so native apps get network events
// without needing a Lua-style opcode hook.
void http_fire_c_pending(void);

// ── Core 1 only (wifi.c) ────────────────────────────────────────────────────

// Check and enforce connect/read/transfer timeouts. Called from wifi_poll().
void http_check_timeouts(void);

struct mg_connection;  // forward declaration to avoid pulling in mongoose.h
// Mongoose event handler for HTTP connections.
void http_ev_fn(struct mg_connection *nc, int ev, void *ev_data);
// QUEUED → `next`; false (do nothing) if the slot left QUEUED meanwhile.
bool http_c1_begin(http_conn_t *c, http_state_t next);
// Drop the slot's connection: clear fn_data, mark it closing, pcb = NULL.
void http_c1_detach(http_conn_t *c);
// Fail the request (FAILED + FAILED|CLOSED callbacks) and detach.
void http_c1_fail(http_conn_t *c, const char *msg);
// Build the request and send it on nc; then HEADERS with the read timeout.
void http_c1_send_request(struct mg_connection *nc, http_conn_t *c);
// CONN_REQ_HTTP_CLOSE: detach and close the connection, then RELEASED.
void http_c1_release(http_conn_t *c);
// Fail every request in flight (link down).
void http_c1_fail_all(const char *msg);
