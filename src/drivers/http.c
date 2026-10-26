#include "http.h"
#include "wifi.h"
#include "display.h"
#include "mbedtls/platform.h"
#include "mongoose.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os/core1_alloc.h"
#include "umm_malloc.h"

// RP2350 XIP cache is per-core, no PSRAM coherency: Core 1 writes rx_buf and
// Core 0 reads it through the uncached alias (0x15xxxxxx), bypassing stale lines.
#ifndef PSRAM_UNCACHED_OFFSET  // SIM_FIRMWARE_NET: 0 (host heap has no alias)
#define PSRAM_UNCACHED_OFFSET 0x04000000u
#endif
static inline uint8_t *rx_buf_uncached(const uint8_t *cached_ptr) {
  return (uint8_t *)((uintptr_t)cached_ptr + PSRAM_UNCACHED_OFFSET);
}

static char *http_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s) + 1;
  char *d = umm_malloc(len);
  if (d)
    memcpy(d, s, len);
  return d;
}

// ── Static pool
// ───────────────────────────────────────────────────────────────

static http_conn_t s_conns[HTTP_MAX_CONNECTIONS];
static spin_lock_t *s_lock;  // the pool lock (see http.h)

// Chunked-body decoder states (http_conn_t.chunk_state)
enum { CH_SIZE = 0, CH_DATA, CH_DATA_CRLF, CH_TRAILER, CH_DONE };

// A response head this large without its blank line is refused.
#define HTTP_HEAD_MAX (HTTP_HEADER_BUF_MAX + 1024)
// Body bytes Mongoose may hold beyond the ring before the transfer is failed
// (there is no TCP backpressure on the device stack; see the report).
// Under Core 1's 128 KB Mongoose pool: one HTTPS connection uses ~35-40 KB
// and growing nc->recv reallocates (old + new live at once), so the guard
// fires before the pool runs out and the transfer fails with a clear error.
#define HTTP_RECV_OVERFLOW (32u * 1024u)
// Ask Mongoose to stop reading (nc->is_full) past this much unconsumed
// data.  Only the socket build (MG_ARCH_UNIX, the simulator) honours it;
// the device's built-in TCP stack has no receive window, so there the
// overflow guard is what stops a reader that falls behind.
#define HTTP_RECV_PAUSE (16u * 1024u)
// How long http_alloc waits for a slot that is being released.
#define HTTP_ALLOC_WAIT_MS 100

// ── Internal helpers
// ──────────────────────────────────────────────────────────

static inline http_state_t st_get(const http_conn_t *c) {
  return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
}
static inline void st_put(http_conn_t *c, http_state_t s) {
  __atomic_store_n(&c->state, s, __ATOMIC_RELEASE);
}
static inline bool st_released(http_state_t s) {
  return s == HTTP_STATE_CLOSING || s == HTTP_STATE_RELEASED;
}
static inline bool st_active(http_state_t s) {  // Core 1 has work to do
  return s == HTTP_STATE_SENDING || s == HTTP_STATE_HEADERS ||
         s == HTTP_STATE_BODY;
}
static inline uint32_t now_ms(void) {
  return to_ms_since_boot(get_absolute_time());
}
static inline bool deadline_passed(uint32_t now, uint32_t deadline) {
  return deadline != 0 && (int32_t)(now - deadline) > 0;
}

// Atomically set pending callback bits (Core 1), unless being released
static inline void pending_set(http_conn_t *c, uint8_t bits) {
  uint32_t irq = spin_lock_blocking(s_lock);
  if (!st_released(st_get(c)))
    c->pending |= bits;
  spin_unlock(s_lock, irq);
}

// FAILED with an error text, FAILED|CLOSED callbacks.  Ignored once the
// request finished or failed, or the slot is being released.  The text is
// written before the state store that publishes it.
static void conn_fail(http_conn_t *c, const char *fmt, ...) {
  char msg[HTTP_ERR_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  uint32_t irq = spin_lock_blocking(s_lock);
  http_state_t st = st_get(c);
  if (st_released(st) || st == HTTP_STATE_DONE || st == HTTP_STATE_FAILED) {
    spin_unlock(s_lock, irq);
    return;
  }
  memcpy(c->err, msg, sizeof(c->err));
  c->deadline_connect = c->deadline_read = c->deadline_transfer = 0;
  st_put(c, HTTP_STATE_FAILED);
  c->pending |= HTTP_CB_FAILED | HTTP_CB_CLOSED;
  spin_unlock(s_lock, irq);
  printf("[HTTP] Error (state %d): %s\n", (int)st, msg);
}

// Returns actual bytes written (may be less than len if the ring is full)
static uint32_t rx_write(http_conn_t *c, const uint8_t *data, uint32_t len) {
  uint32_t irq = spin_lock_blocking(s_lock);

  uint32_t space = c->rx_cap - c->rx_count;
  if (len > space)
    len = space;
  if (len == 0 || !c->rx_buf) {
    spin_unlock(s_lock, irq);
    return 0;
  }

  // Write through uncached alias — bypasses Core 1's XIP cache so data
  // reaches physical PSRAM immediately, visible to Core 0's uncached reads.
  uint8_t *uc = rx_buf_uncached(c->rx_buf);
  uint32_t till_end = c->rx_cap - c->rx_head;
  if (len <= till_end) {
    memcpy(&uc[c->rx_head], data, len);
    c->rx_head += len;
    if (c->rx_head == c->rx_cap)
      c->rx_head = 0;
  } else {
    memcpy(&uc[c->rx_head], data, till_end);
    memcpy(uc, data + till_end, len - till_end);
    c->rx_head = len - till_end;
  }
  c->rx_count += len;

  spin_unlock(s_lock, irq);
  return len;
}

// Where decoded body bytes go: the ring (sink == NULL) or, when the server
// closes with more than the ring can take, a spill buffer.
typedef struct {
  uint8_t *buf;
  uint32_t len, cap;
} spill_sink_t;

static uint32_t sink_put(http_conn_t *c, spill_sink_t *sp, const uint8_t *d,
                         uint32_t n) {
  if (!sp)
    return rx_write(c, d, n);
  uint32_t room = sp->cap - sp->len;
  if (n > room)
    n = room;
  // Through the cached alias: umm's header shares the buffer's first cache
  // line, and a write-back of it would overwrite bytes written uncached
  // (tcp.c's spill lost its first 4 bytes that way on the device).
  memcpy(sp->buf + sp->len, d, n);
  sp->len += n;
  return n;
}

static int hexval(uint8_t ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

// ── Core 1: connection handling ─────────────────────────────────────────────

// pcb is non-NULL only while Mongoose still owns that connection: every
// MG_EV_CLOSE that reaches the slot clears it (http_ev_fn), and a detached
// connection no longer reaches the slot.  So pcb here is never freed memory.
void http_c1_detach(http_conn_t *c) {
  struct mg_connection *nc = (struct mg_connection *)c->pcb;
  if (!nc)
    return;
  nc->fn_data = NULL;  // no later event reaches the slot
  nc->is_closing = 1;
  c->pcb = NULL;
}

void http_c1_fail(http_conn_t *c, const char *msg) {
  conn_fail(c, "%s", msg);
  http_c1_detach(c);
}

bool http_c1_begin(http_conn_t *c, http_state_t next) {
  bool ok = false;
  uint32_t irq = spin_lock_blocking(s_lock);
  if (st_get(c) == HTTP_STATE_QUEUED) {
    st_put(c, next);
    ok = true;
  }
  spin_unlock(s_lock, irq);
  return ok;
}

void http_c1_release(http_conn_t *c) {
  if (st_get(c) != HTTP_STATE_CLOSING)
    return;  // not a release request
  http_c1_detach(c);
  uint32_t irq = spin_lock_blocking(s_lock);
  st_put(c, HTTP_STATE_RELEASED);  // Core 1 never touches the slot again
  spin_unlock(s_lock, irq);
}

void http_c1_fail_all(const char *msg) {
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    http_conn_t *c = &s_conns[i];
    uint32_t irq = spin_lock_blocking(s_lock);
    http_state_t st = st_get(c);
    bool live = c->in_use && st >= HTTP_STATE_QUEUED && st <= HTTP_STATE_BODY;
    spin_unlock(s_lock, irq);
    if (live)
      http_c1_fail(c, msg);
  }
}

// Build the full request in one PSRAM buffer and mg_send() it; then the
// request waits for the response head with the read timeout armed.
void http_c1_send_request(struct mg_connection *nc, http_conn_t *c) {
  size_t path_len = c->path ? strlen(c->path) : 1;
  size_t hdrs_len = c->extra_hdrs ? strlen(c->extra_hdrs) : 0;
  size_t need = path_len + strlen(c->server) + hdrs_len + 256;
  if (c->tx_buf && c->tx_len > 0)
    need += 32 + c->tx_len;

  char *buf = umm_malloc(need);
  if (!buf) {
    http_c1_fail(c, "request build OOM");
    return;
  }

  // Skip default User-Agent / Accept-Encoding if extra_hdrs already provides one
  bool has_ua = c->extra_hdrs &&
      (strstr(c->extra_hdrs, "User-Agent:") != NULL ||
       strstr(c->extra_hdrs, "user-agent:") != NULL);
  bool has_ae = c->extra_hdrs &&
      (strstr(c->extra_hdrs, "Accept-Encoding:") != NULL ||
       strstr(c->extra_hdrs, "accept-encoding:") != NULL);

  int off = snprintf(buf, need,
      "%s %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "%s"
      "%s"
      "Connection: %s\r\n",
      c->method, c->path ? c->path : "/", c->server,
      has_ua ? "" : "User-Agent: PicoDeck/1.0\r\n",
      has_ae ? "" : "Accept-Encoding: identity\r\n",
      c->req_keep_alive ? "keep-alive" : "close");

  if (c->extra_hdrs)
    off += snprintf(buf + off, need - off, "%s", c->extra_hdrs);

  if (c->tx_buf && c->tx_len > 0) {
    off += snprintf(buf + off, need - off,
        "Content-Length: %u\r\n\r\n", (unsigned)c->tx_len);
    memcpy(buf + off, c->tx_buf, c->tx_len);  // binary safe
    off += c->tx_len;
  } else {
    off += snprintf(buf + off, need - off, "\r\n");
  }

  bool sent = mg_send(nc, buf, off);
  umm_free(buf);
  if (!sent) {
    http_c1_fail(c, "send failed (out of memory)");
    return;
  }

  uint32_t irq = spin_lock_blocking(s_lock);
  http_state_t st = st_get(c);
  if (st == HTTP_STATE_CONNECTING || st == HTTP_STATE_SENDING) {
    c->deadline_connect = 0;
    c->deadline_read = now_ms() + c->req_read_timeout_ms;
    st_put(c, HTTP_STATE_HEADERS);
  }
  spin_unlock(s_lock, irq);
}

// Copy the parsed header fields into hdr_buf (lower-case names).
static void copy_headers(http_conn_t *c, const struct mg_http_message *hm) {
  c->hdr_count = 0;
  size_t hdr_off = 0;
  for (int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0;
       i++) {
    const struct mg_http_header *h = &hm->headers[i];
    size_t need = h->name.len + 1 + h->value.len + 1;
    if (hdr_off + need > HTTP_HEADER_BUF_MAX) {
      printf("[HTTP] Header buf overflow: need %u, have %u/%u\n",
             (unsigned)need, (unsigned)hdr_off,
             (unsigned)HTTP_HEADER_BUF_MAX);
      break;
    }
    if (c->hdr_count >= HTTP_MAX_HDR_ENTRIES)
      break;

    c->hdr_keys[c->hdr_count] = &c->hdr_buf[hdr_off];
    for (size_t j = 0; j < h->name.len; j++)
      c->hdr_buf[hdr_off++] =
          (h->name.buf[j] >= 'A' && h->name.buf[j] <= 'Z')
              ? h->name.buf[j] + 32
              : h->name.buf[j];
    c->hdr_buf[hdr_off++] = '\0';

    c->hdr_vals[c->hdr_count] = &c->hdr_buf[hdr_off];
    memcpy(&c->hdr_buf[hdr_off], h->value.buf, h->value.len);
    hdr_off += h->value.len;
    c->hdr_buf[hdr_off++] = '\0';

    c->hdr_count++;
  }
  c->hdr_len = hdr_off;
}

// Parse the response head once it is complete in nc->recv.  True once the
// headers are in (state BODY); false if not yet (or the response is bad).
static bool c1_parse_head(struct mg_connection *nc, http_conn_t *c) {
  for (;;) {
    int n = mg_http_get_request_len(nc->recv.buf, nc->recv.len);
    if (n < 0) {
      http_c1_fail(c, "malformed response headers");
      return false;
    }
    if (n == 0) {
      if (nc->recv.len > HTTP_HEAD_MAX) {
        http_c1_fail(c, "response headers too large");
      }
      return false;
    }
    struct mg_http_message hm;
    if (mg_http_parse((const char *)nc->recv.buf, nc->recv.len, &hm) <= 0) {
      http_c1_fail(c, "malformed response headers");
      return false;
    }
    int status = mg_http_status(&hm);
    if (status >= 100 && status < 200) {  // interim (100 Continue): skip it
      mg_iobuf_del(&nc->recv, 0, (size_t)n);
      continue;
    }

    copy_headers(c, &hm);
    c->status_code = status;
    c->chunked = false;
    c->chunk_state = CH_SIZE;
    c->chunk_left = 0;
    c->content_length = -1;
    struct mg_str *te = mg_http_get_header(&hm, "Transfer-Encoding");
    struct mg_str *cl = mg_http_get_header(&hm, "Content-Length");
    if (te && mg_strcasecmp(*te, mg_str("chunked")) == 0) {
      c->chunked = true;
    } else if (cl) {
      c->content_length = atoi(cl->buf);
      if (c->content_length < 0)
        c->content_length = 0;
    }
    // No body, whatever the headers say
    if (status == 204 || status == 304 || strcmp(c->method, "HEAD") == 0) {
      c->chunked = false;
      c->content_length = 0;
    }
    mg_iobuf_del(&nc->recv, 0, (size_t)n);
    // The response has begun, so the whole request has been sent: give
    // nc->send's memory back to Core 1's pool. Mongoose keeps an iobuf at
    // its high-water size, so a chat POST's ~20 KB body held 24 KB for the
    // whole response, and the pool, fragmented around it, then couldn't
    // grow another buffer.
    if (nc->send.len == 0)
      mg_iobuf_free(&nc->send);
    printf("[HTTP] Headers received, status %d, %s %d\n", status,
           c->chunked ? "chunked" : "Content-Length", (int)c->content_length);

    uint32_t irq = spin_lock_blocking(s_lock);
    bool ok = !st_released(st_get(c));
    if (ok) {
      __atomic_store_n(&c->headers_done, true, __ATOMIC_RELEASE);
      c->deadline_connect = 0;
      c->deadline_read = now_ms() + c->req_read_timeout_ms;
      st_put(c, HTTP_STATE_BODY);
      c->pending |= HTTP_CB_HEADERS;
    }
    spin_unlock(s_lock, irq);
    return ok;
  }
}

static bool body_complete(const http_conn_t *c) {
  if (c->chunked)
    return c->chunk_state == CH_DONE;
  return c->content_length >= 0 &&
         c->body_received >= (uint32_t)c->content_length;
}

// Move body bytes from nc->recv into the sink (ring or spill), decoding
// chunked framing.  Consumes from nc->recv only what the sink took.
// Returns false if the body is malformed (the request has been failed).
static bool c1_feed_body(struct mg_connection *nc, http_conn_t *c,
                         spill_sink_t *sp) {
  const uint8_t *p = nc->recv.buf;
  size_t len = nc->recv.len;
  size_t used = 0;
  uint32_t wrote = 0;
  bool bad = false;

  if (!c->chunked) {
    size_t want = len;
    if (c->content_length >= 0) {
      uint32_t left = (uint32_t)c->content_length - c->body_received;
      if (want > left)
        want = left;
    }
    wrote = sink_put(c, sp, p, (uint32_t)want);
    used = wrote;
  } else {
    while (used < len && c->chunk_state != CH_DONE) {
      const uint8_t *q = p + used;
      size_t avail = len - used;
      if (c->chunk_state == CH_SIZE || c->chunk_state == CH_TRAILER) {
        const uint8_t *nl = memchr(q, '\n', avail);
        if (!nl) {
          if (avail > 1024) bad = true;  // no line end in sight
          break;
        }
        size_t line = (size_t)(nl - q) + 1;
        if (c->chunk_state == CH_SIZE) {
          uint32_t v = 0;
          size_t k = 0;
          int h;
          while (k < line && (h = hexval(q[k])) >= 0) {
            if (v > 0x07FFFFFFu) { bad = true; break; }
            v = v * 16 + (uint32_t)h;
            k++;
          }
          if (bad || k == 0) { bad = true; break; }
          used += line;  // size line (chunk extensions ignored)
          if (v == 0) {
            c->chunk_state = CH_TRAILER;
          } else {
            c->chunk_left = v;
            c->chunk_state = CH_DATA;
          }
        } else {  // trailer lines until the empty one
          used += line;
          if (line == 1 || (line == 2 && q[0] == '\r'))
            c->chunk_state = CH_DONE;
        }
      } else if (c->chunk_state == CH_DATA) {
        uint32_t n = avail < c->chunk_left ? (uint32_t)avail : c->chunk_left;
        uint32_t w = sink_put(c, sp, q, n);
        used += w;
        wrote += w;
        c->chunk_left -= w;
        if (c->chunk_left == 0)
          c->chunk_state = CH_DATA_CRLF;
        if (w < n)
          break;  // sink full
      } else {  // CH_DATA_CRLF
        if (q[0] == '\n') {
          used += 1;
        } else if (q[0] == '\r') {
          if (avail < 2) break;
          if (q[1] != '\n') { bad = true; break; }
          used += 2;
        } else {
          bad = true;
          break;
        }
        c->chunk_state = CH_SIZE;
      }
    }
  }

  if (used)
    mg_iobuf_del(&nc->recv, 0, used);

  uint32_t irq = spin_lock_blocking(s_lock);
  c->body_received += wrote;
  if (!st_released(st_get(c))) {
    if (wrote)
      c->pending |= HTTP_CB_REQUEST;
    // Bytes arrived or are waiting for ring space: the transfer is not idle.
    if (used || nc->recv.len)
      c->deadline_read = now_ms() + c->req_read_timeout_ms;
  }
  spin_unlock(s_lock, irq);

  if (bad) {
    http_c1_fail(c, "malformed chunked body");
    return false;
  }
  return true;
}

static void c1_complete(struct mg_connection *nc, http_conn_t *c,
                        bool closing) {
  uint32_t irq = spin_lock_blocking(s_lock);
  bool done = st_get(c) == HTTP_STATE_BODY;
  if (done) {
    c->deadline_read = c->deadline_transfer = 0;
    st_put(c, HTTP_STATE_DONE);
    c->pending |= HTTP_CB_COMPLETE;
  }
  spin_unlock(s_lock, irq);
  if (done && !closing && !c->req_keep_alive)
    nc->is_closing = 1;
}

// Body bytes still in nc->recv when the connection goes away: decode the
// rest into a spill buffer so the app can read all of it.
static bool c1_spill_rest(struct mg_connection *nc, http_conn_t *c) {
  if (nc->recv.len == 0)
    return true;
  spill_sink_t sp = {umm_malloc(nc->recv.len), 0, (uint32_t)nc->recv.len};
  if (!sp.buf) {
    http_c1_fail(c, "out of memory keeping the end of the response");
    return false;
  }
  if (!c1_feed_body(nc, c, &sp)) {
    umm_free(sp.buf);
    return false;
  }
  if (sp.len == 0) {
    umm_free(sp.buf);
    return true;
  }
  uint32_t irq = spin_lock_blocking(s_lock);
  uint8_t *old = c->spill;  // never set twice for one response
  c->spill = sp.buf;
  c->spill_len = sp.len;
  c->spill_off = 0;
  spin_unlock(s_lock, irq);
  if (old)
    umm_free(old);
  return true;
}

// Parse and move whatever nc->recv holds.  `closing`: the connection is
// going away (MG_EV_CLOSE), so a close-delimited body ends here and nothing
// may be left behind in nc->recv.
static void c1_process(struct mg_connection *nc, http_conn_t *c,
                       bool closing) {
  if (!st_active(st_get(c)))
    return;
  if (!__atomic_load_n(&c->headers_done, __ATOMIC_RELAXED) &&
      !c1_parse_head(nc, c)) {
    if (closing)
      conn_fail(c, "connection closed before the response headers");
    return;
  }
  if (st_get(c) != HTTP_STATE_BODY)
    return;
  if (!c1_feed_body(nc, c, NULL))
    return;

  if (closing && !body_complete(c)) {
    if (!c1_spill_rest(nc, c))
      return;
    if (!c->chunked && c->content_length < 0) {
      c1_complete(nc, c, true);  // close-delimited: the close ends the body
      return;
    }
    if (!body_complete(c)) {
      if (c->chunked)
        conn_fail(c, "connection closed inside the chunked body");
      else
        conn_fail(c, "connection closed after %u of %d bytes",
                  (unsigned)c->body_received, (int)c->content_length);
      return;
    }
  }
  if (body_complete(c)) {
    c1_complete(nc, c, closing);
  } else if (nc->recv.len > HTTP_RECV_OVERFLOW) {
    // The app is not reading and Mongoose keeps buffering: give up rather
    // than exhaust Core 1's pool (and never report this as complete).
    http_c1_fail(c, "receive buffer overflow (the app is not reading)");
  }
}

// ── Mongoose Event Handler ───────────────────────────────────────────────────
// Runs exclusively on Core 1 inside mg_mgr_poll().  Streaming: fires
// HTTP_CB_REQUEST incrementally as body data arrives.

static void c1_event(struct mg_connection *nc, http_conn_t *c, int ev,
                     void *ev_data);

void http_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
  http_conn_t *c = (http_conn_t *)nc->fn_data;
  if (!c)
    return;
  // A stale connection of this slot (detached connections have no fn_data;
  // belt and braces).  pcb is still NULL during mg_connect itself.
  if (c->pcb != NULL && c->pcb != nc)
    return;
  c1_event(nc, c, ev, ev_data);
  if ((ev == MG_EV_READ || ev == MG_EV_POLL) && nc->fn_data == c)
    nc->is_full = nc->recv.len >= HTTP_RECV_PAUSE;
  // Invariant: pcb never outlives its connection.  Mongoose frees nc right
  // after MG_EV_CLOSE, so drop it here whatever the handler did — also for
  // a CLOSING slot, whose close handler (http_c1_release) would otherwise
  // detach a freed connection.
  if (ev == MG_EV_CLOSE && c->pcb == nc)
    c->pcb = NULL;
}

static void c1_event(struct mg_connection *nc, http_conn_t *c, int ev,
                     void *ev_data) {
  http_state_t st = st_get(c);
  if (st_released(st))
    return;  // Core 0 let go; the close request is on its way

  if (ev == MG_EV_CONNECT) {
    if (st == HTTP_STATE_CONNECTING) {
      printf("[HTTP] Connected, sending %s %s\n", c->method,
             c->path ? c->path : "/");
      http_c1_send_request(nc, c);
    }
  } else if (ev == MG_EV_READ || ev == MG_EV_POLL) {
    // POLL too: data left in nc->recv while the ring was full moves once the
    // app has read (MG_EV_READ fires only for new bytes).
    if (st_active(st) && nc->recv.len > 0)
      c1_process(nc, c, false);
  } else if (ev == MG_EV_ERROR) {
    if (st == HTTP_STATE_DONE || st == HTTP_STATE_FAILED)
      return;  // late error after the response was complete
    char tls_err[HTTP_ERR_MAX];
    if (wifi_tls_verify_error(nc, tls_err, sizeof(tls_err)))
      conn_fail(c, "%s", tls_err);
    else if (c->body_received > 0)
      conn_fail(c, "partial: %s", (char *)ev_data);  // truncated, not done
    else
      conn_fail(c, "Mongoose error: %s", (char *)ev_data);
    // Mongoose closes nc itself; MG_EV_CLOSE clears pcb.
  } else if (ev == MG_EV_CLOSE) {
    printf("[HTTP] Connection closed (slot %ld, state %d)\n",
           (long)(c - s_conns), (int)st);
    if (st_active(st))
      c1_process(nc, c, true);
    else if (st == HTTP_STATE_QUEUED || st == HTTP_STATE_CONNECTING)
      conn_fail(c, "connection closed");
    // pcb is cleared by http_ev_fn after this returns.
  }
}

// ── Timeout enforcement (called from wifi_poll on Core 1) ───────────────────

void http_check_timeouts(void) {
  uint32_t now = now_ms();
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    http_conn_t *c = &s_conns[i];
    char msg[48];
    msg[0] = '\0';
    uint32_t irq = spin_lock_blocking(s_lock);
    if (c->in_use) {
      http_state_t st = st_get(c);
      bool connecting = st == HTTP_STATE_QUEUED ||
                        st == HTTP_STATE_CONNECTING ||
                        st == HTTP_STATE_SENDING;
      bool receiving = st == HTTP_STATE_HEADERS || st == HTTP_STATE_BODY;
      // Connect timeout: QUEUED → CONNECTING → SENDING
      if (connecting && deadline_passed(now, c->deadline_connect))
        snprintf(msg, sizeof(msg), "connect timeout (%ums)",
                 (unsigned)c->req_connect_timeout_ms);
      // Read timeout: waiting for the head or the body, nothing arriving
      else if (receiving && deadline_passed(now, c->deadline_read))
        snprintf(msg, sizeof(msg), "read timeout (%ums)",
                 (unsigned)c->req_read_timeout_ms);
      // Transfer deadline: hard ceiling regardless of data trickle
      else if (receiving && deadline_passed(now, c->deadline_transfer))
        snprintf(msg, sizeof(msg), "transfer timeout (%ums)",
                 (unsigned)c->req_max_transfer_ms);
    }
    spin_unlock(s_lock, irq);
    if (msg[0])
      http_c1_fail(c, msg);  // clears fn_data: no event reaches the slot
  }
}

// ── Public API (Core 0)
// ────────────────────────────────────────────────────────────────

void http_init(void) {
  memset(s_conns, 0, sizeof(s_conns));
  if (!s_lock)
    s_lock = spin_lock_instance(spin_lock_claim_unused(true));
}

static void push_close(http_conn_t *c) {
  conn_req_t req = {.type = CONN_REQ_HTTP_CLOSE, .conn = c};
  c->close_queued = wifi_req_push(&req);
}

static bool any_releasing(void);

// Claim a free slot for the two buffers (NULL if none).
static http_conn_t *claim_slot(char *hdr, uint8_t *rx) {
  http_conn_t *got = NULL;
  uint32_t irq = spin_lock_blocking(s_lock);
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    http_conn_t *c = &s_conns[i];
    if (!c->in_use) {
      memset(c, 0, sizeof(*c));
      c->range_from = -1;
      c->range_to = -1;
      c->connect_timeout_ms = 10000;
      c->read_timeout_ms = 30000;
      c->content_length = -1;
      c->hdr_buf = hdr;
      c->rx_buf = rx;
      c->rx_cap = HTTP_RECV_BUF_DEFAULT;
      c->in_use = true;
      got = c;
      break;
    }
  }
  spin_unlock(s_lock, irq);
  return got;
}

http_conn_t *http_alloc(void) {
  http_reap();
  char *hdr = umm_malloc(HTTP_HEADER_BUF_MAX);
  uint8_t *rx = umm_malloc(HTTP_RECV_BUF_DEFAULT);
  if (!hdr || !rx) {
    printf("[HTTP] Failed to allocate connection buffers (OOM)\n");
    umm_free(hdr);
    umm_free(rx);
    return NULL;
  }
  http_conn_t *got = claim_slot(hdr, rx);
  // Pool full but a connection is being released (closed just now): Core 1
  // acknowledges within a tick or two, so wait briefly rather than fail a
  // close-then-reopen.
  uint32_t start = now_ms();
  while (!got && any_releasing() && now_ms() - start < HTTP_ALLOC_WAIT_MS) {
    sleep_ms(1);
    http_reap();
    got = claim_slot(hdr, rx);
  }
  if (!got) {
    umm_free(hdr);
    umm_free(rx);
    printf("[HTTP] Failed to allocate connection: all %d slots in use\n",
           HTTP_MAX_CONNECTIONS);
    return NULL;
  }
  printf("[HTTP] Allocated connection %d\n", (int)(got - s_conns));
  return got;
}

void http_free(http_conn_t *c) {
  if (!c)
    return;
  uint32_t irq = spin_lock_blocking(s_lock);
  if (!c->in_use || st_released(st_get(c))) {
    spin_unlock(s_lock, irq);
    return;
  }
  st_put(c, HTTP_STATE_CLOSING);
  c->pending = 0;
  spin_unlock(s_lock, irq);
  c->lua_ud = NULL;
  push_close(c);  // on failure the slot stays CLOSING; http_reap retries
}

void http_close(http_conn_t *c) { http_free(c); }

void http_reap(void) {
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    http_conn_t *c = &s_conns[i];
    void *bufs[6] = {0};
    bool retry = false;
    uint32_t irq = spin_lock_blocking(s_lock);
    if (c->in_use) {
      http_state_t st = st_get(c);
      if (st == HTTP_STATE_RELEASED) {
        bufs[0] = c->path;
        bufs[1] = c->extra_hdrs;
        bufs[2] = c->tx_buf;
        bufs[3] = c->rx_buf;
        bufs[4] = c->hdr_buf;
        bufs[5] = c->spill;
        memset(c, 0, sizeof(*c));  // in_use = false
      } else if (st == HTTP_STATE_CLOSING && !c->close_queued) {
        retry = true;
      }
    }
    spin_unlock(s_lock, irq);
    for (int k = 0; k < 6; k++)
      umm_free(bufs[k]);
    if (retry)
      push_close(c);
  }
}

static bool any_releasing(void) {
  bool r = false;
  uint32_t irq = spin_lock_blocking(s_lock);
  for (int i = 0; i < HTTP_MAX_CONNECTIONS && !r; i++)
    r = s_conns[i].in_use && st_released(st_get(&s_conns[i]));
  spin_unlock(s_lock, irq);
  return r;
}

void http_close_all(void (*on_free)(void *lua_ud)) {
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    http_conn_t *c = http_get_conn(i);
    if (!c || st_released(st_get(c)))
      continue;
    if (on_free && c->lua_ud)
      on_free(c->lua_ud);
    http_free(c);
  }
  // Wait (bounded) for Core 1 to release them, reclaiming as they come.
  uint32_t start_ms = now_ms();
  for (;;) {
    http_reap();
    if (!any_releasing() || now_ms() - start_ms >= 500)
      break;
    sleep_ms(5);
  }
  if (any_releasing())
    printf("[HTTP] close_all: Core 1 has not released every slot yet\n");
}

bool http_set_recv_buf(http_conn_t *c, uint32_t bytes) {
  if (!c || bytes == 0 || bytes > HTTP_RECV_BUF_MAX)
    return false;
  // A fresh buffer rather than realloc: a realloc memcpy would leave stale
  // XIP cache lines on Core 0 for a buffer Core 1 writes (RP2350 has
  // per-core XIP caches, no hardware coherency for PSRAM).
  uint8_t *nb = umm_malloc(bytes);
  if (!nb)
    return false;  // the old buffer stays
  uint32_t irq = spin_lock_blocking(s_lock);
  uint8_t *old = c->rx_buf;
  c->rx_buf = nb;
  c->rx_cap = bytes;
  c->rx_head = 0;
  c->rx_tail = 0;
  c->rx_count = 0;
  spin_unlock(s_lock, irq);
  umm_free(old);
  return true;
}

static bool start_request(http_conn_t *c, const char *method, const char *path,
                          const char *extra_hdr, const char *body,
                          size_t body_len) {
  if (!c || !path)
    return false;

  if (!wifi_is_available())
    return false;

  // Only between requests: never while Core 1 may still read the buffers
  // below or run a connection for this slot.
  http_state_t st = st_get(c);
  if (st != HTTP_STATE_IDLE && st != HTTP_STATE_DONE &&
      st != HTTP_STATE_FAILED)
    return false;

  // Request buffers: Core 0 allocates and frees them; Core 1 only reads
  // them between CONN_REQ_HTTP_START and sending.  The body is binary.
  char *new_path = http_strdup(path);
  char *new_hdrs = extra_hdr ? http_strdup(extra_hdr) : NULL;
  char *new_tx = NULL;
  if (body && body_len > 0) {
    new_tx = umm_malloc(body_len);
    if (new_tx)
      memcpy(new_tx, body, body_len);
  }
  if (!new_path || (extra_hdr && !new_hdrs) ||
      (body && body_len > 0 && !new_tx)) {
    umm_free(new_path);
    umm_free(new_hdrs);
    umm_free(new_tx);
    snprintf(c->err, sizeof(c->err), "request alloc failed (OOM)");
    return false;
  }

  uint32_t now = now_ms();
  uint32_t irq = spin_lock_blocking(s_lock);
  char *old_path = c->path, *old_hdrs = c->extra_hdrs, *old_tx = c->tx_buf;
  uint8_t *old_spill = c->spill;
  c->path = new_path;
  c->extra_hdrs = new_hdrs;
  c->tx_buf = new_tx;
  c->tx_len = new_tx ? (uint32_t)body_len : 0;
  strncpy(c->method, method, sizeof(c->method) - 1);
  c->method[sizeof(c->method) - 1] = '\0';

  c->status_code = 0;
  c->content_length = -1;
  c->body_received = 0;
  __atomic_store_n(&c->headers_done, false, __ATOMIC_RELAXED);
  c->chunked = false;
  c->chunk_state = CH_SIZE;
  c->chunk_left = 0;
  c->hdr_count = 0;
  c->hdr_len = 0;
  c->err[0] = '\0';
  c->spill = NULL;
  c->spill_len = c->spill_off = 0;
  c->rx_head = c->rx_tail = c->rx_count = 0;
  c->pending = 0;

  c->req_keep_alive = c->keep_alive;
  c->req_connect_timeout_ms = c->connect_timeout_ms;
  c->req_read_timeout_ms = c->read_timeout_ms;
  c->req_max_transfer_ms = c->max_transfer_ms;
  c->deadline_connect = now + c->req_connect_timeout_ms;
  c->deadline_read = 0;
  c->deadline_transfer =
      c->req_max_transfer_ms > 0 ? now + c->req_max_transfer_ms : 0;
  st_put(c, HTTP_STATE_QUEUED);
  spin_unlock(s_lock, irq);

  umm_free(old_path);
  umm_free(old_hdrs);
  umm_free(old_tx);
  umm_free(old_spill);

  // Core 1 runs mg_connect / the keep-alive reuse from drain_requests().
  conn_req_t req = {.type = CONN_REQ_HTTP_START, .conn = c};
  if (!wifi_req_push(&req)) {
    conn_fail(c, "request queue full");  // Core 1 never saw it
    return false;
  }
  return true;
}

bool http_get(http_conn_t *c, const char *path, const char *extra_hdr) {
  return start_request(c, "GET", path, extra_hdr, NULL, 0);
}

bool http_post(http_conn_t *c, const char *path, const char *extra_hdr,
               const char *body, size_t body_len) {
  return start_request(c, "POST", path, extra_hdr, body, body_len);
}

uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len) {
  if (!c || !out || len == 0)
    return 0;

  uint32_t irq = spin_lock_blocking(s_lock);
  uint32_t n = (len < c->rx_count) ? len : c->rx_count;
  if (n > 0) {
    // Read through uncached alias — bypasses Core 0's XIP cache to see
    // fresh data written by Core 1 to physical PSRAM.
    const uint8_t *uc = rx_buf_uncached(c->rx_buf);
    uint32_t till_end = c->rx_cap - c->rx_tail;
    if (n <= till_end) {
      memcpy(out, &uc[c->rx_tail], n);
      c->rx_tail += n;
      if (c->rx_tail == c->rx_cap)
        c->rx_tail = 0;
    } else {
      memcpy(out, &uc[c->rx_tail], till_end);
      memcpy(out + till_end, uc, n - till_end);
      c->rx_tail = n - till_end;
    }
    c->rx_count -= n;
  }
  // The ring first, then what was spilled when the server closed.
  if (n < len && c->spill && c->spill_off < c->spill_len) {
    uint32_t m = c->spill_len - c->spill_off;
    if (m > len - n)
      m = len - n;
    memcpy(out + n, c->spill + c->spill_off, m);  // cached: see sink_put
    c->spill_off += m;
    n += m;
  }
  spin_unlock(s_lock, irq);
  return n;
}

uint32_t http_bytes_available(http_conn_t *c) {
  if (!c) return 0;
  uint32_t irq = spin_lock_blocking(s_lock);
  uint32_t n = c->rx_count + (c->spill ? c->spill_len - c->spill_off : 0);
  spin_unlock(s_lock, irq);
  return n;
}

http_conn_t *http_get_conn(int idx) {
  if (idx < 0 || idx >= HTTP_MAX_CONNECTIONS)
    return NULL;
  uint32_t irq = spin_lock_blocking(s_lock);
  bool used = s_conns[idx].in_use;
  spin_unlock(s_lock, irq);
  return used ? &s_conns[idx] : NULL;
}

uint8_t http_take_pending(http_conn_t *c) {
  if (!c)
    return 0;
  uint32_t irq = spin_lock_blocking(s_lock);
  uint8_t p = c->pending;
  c->pending = 0;
  spin_unlock(s_lock, irq);
  return p;
}

http_state_t http_get_state(http_conn_t *c) {
  return c ? st_get(c) : HTTP_STATE_IDLE;
}

const char *http_get_error(http_conn_t *c) {
  if (!c)
    return NULL;
  // Core 1 writes err only before publishing FAILED (or a DONE it reached);
  // in these states nobody is writing it.
  http_state_t st = st_get(c);
  if (st != HTTP_STATE_IDLE && st != HTTP_STATE_DONE &&
      st != HTTP_STATE_FAILED)
    return NULL;
  return c->err[0] ? c->err : NULL;
}

bool http_headers_ready(http_conn_t *c) {
  return c && __atomic_load_n(&c->headers_done, __ATOMIC_ACQUIRE);
}

int http_get_status(http_conn_t *c) {
  return http_headers_ready(c) ? c->status_code : 0;
}

void http_get_progress(http_conn_t *c, int *received, int *total) {
  int r = 0, t = -1;
  if (c) {
    uint32_t irq = spin_lock_blocking(s_lock);
    r = (int)c->body_received;
    spin_unlock(s_lock, irq);
    if (http_headers_ready(c))
      t = (int)c->content_length;
  }
  if (received) *received = r;
  if (total) *total = t;
}

bool http_is_complete(http_conn_t *c) {
  http_state_t st = http_get_state(c);
  return st == HTTP_STATE_DONE || st == HTTP_STATE_FAILED;
}

void http_poll(void) {}

// Fire C-language (non-Lua) HTTP callbacks.
// Native apps store their callback function pointers directly in http_conn_t
// extension fields (to be added when native HTTP is fully implemented).
// For now this is a no-op stub; the infrastructure is wired up so Core 1
// can call it safely every poll cycle.
void http_fire_c_pending(void) {
  // Future: iterate s_conns, check pending flags, call C callbacks.
}

// ── Custom Mongoose Allocator ────────────────────────────────────────────────
void *mg_calloc(size_t count, size_t size) { return core1_calloc(count, size); }

void mg_free(void *ptr) { core1_free(ptr); }
