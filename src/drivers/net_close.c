#include "net_close.h"

#include <stdint.h>

#include "mongoose.h"

// In nc->data: a connection let go has no other use for it.
typedef struct {
  uint32_t deadline_ms;  // low bits of mg_millis(), compared modulo 2^32
  bool cleaned;          // the buffers below have gone
} closing_t;

static void closing_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
  closing_t *cl = (closing_t *)nc->data;
  if (ev == MG_EV_READ) {
    // Nobody will read it: without this it piles up in Core 1's pool for as
    // long as the peer keeps sending.
    mg_iobuf_del(&nc->recv, 0, nc->recv.len);
  } else if (ev == MG_EV_POLL) {
    if ((int32_t)((uint32_t)*(uint64_t *)ev_data - cl->deadline_ms) > 0) {
      nc->is_closing = 1;
      return;
    }
    // Everything has gone down to TCP, where the retransmission queue holds
    // it as ciphertext, so the ~30 KB of mbedTLS state and these buffers are
    // of no more use. It is what lets TLS sockets open and close quickly: the
    // connection's remaining wait costs about 2 KB. In a poll, not in
    // net_close_conn, as that can run deep inside a handler still using them.
    if (!cl->cleaned && nc->send.len == 0 && !nc->is_tls_throttled) {
      cl->cleaned = true;
#if MG_TLS == MG_TLS_MBED
      // The close_notify a TLS peer expects before the FIN, or it sees the
      // stream as cut short. Best effort: it needs room in the queue.
      if (nc->tls != NULL)
        mbedtls_ssl_close_notify(&((struct mg_tls *)nc->tls)->ssl);
#endif
      mg_tls_free(nc);
      nc->is_tls = 0;
      mg_iobuf_free(&nc->send);
      mg_iobuf_free(&nc->rtls);
      mg_iobuf_free(&nc->recv);
    }
  }
}

void net_close_conn(struct mg_connection *nc) {
  nc->fn_data = NULL;
  if (nc->is_closing)
    return;
  // No FIN to send before the connection is up. During a TLS handshake one
  // would leave the handshake running: it has no use, and its key exchange
  // would hold Core 1 for a second.
  if (nc->is_connecting || nc->is_resolving || nc->is_tls_hs ||
      nc->mgr->ifp == NULL || nc->mgr->ifp->state != MG_TCPIP_STATE_READY) {
    nc->is_closing = 1;
    return;
  }
  closing_t *cl = (closing_t *)nc->data;
  cl->deadline_ms = (uint32_t)mg_millis() + NET_CLOSE_LIMIT_MS;
  cl->cleaned = false;
  nc->is_full = 0;  // TLS stops decrypting while this is set
  nc->is_draining = 1;
  nc->fn = closing_ev_fn;
}

void net_close_drop_all(struct mg_mgr *mgr) {
  for (struct mg_connection *c = mgr->conns; c != NULL; c = c->next)
    if (c->fn == closing_ev_fn)
      c->is_closing = 1;
}

unsigned net_close_count(const struct mg_mgr *mgr) {
  unsigned n = 0;
  for (const struct mg_connection *c = mgr->conns; c != NULL; c = c->next)
    if (c->fn == closing_ev_fn)
      n++;
  return n;
}
