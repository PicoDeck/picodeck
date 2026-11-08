// Mongoose's built-in TCP and mbedTLS together, as on the device: a TLS client
// on the firmware's mbedTLS configuration and Core 1's allocator, against a TLS
// server on a second stack, over a wire that can lose frames, on a virtual
// clock. No simulator build has this: the firmware-net build has no TLS and no
// built-in TCP. A peer on the same stack keeps no out-of-order segments, so one
// lost frame costs a resend per frame in flight; that is the peer's limit, and
// time here is virtual. test_mg_tcp_resend.c has a peer that keeps them.
//
// The memory figures are this 64-bit host's: pointers make Mongoose's structures
// larger than on the device. What they are compared with is what counts.
//
// The receive window (MG_TCPIP_RCVBUF) is tested from the other end: the server
// streams to a client that reads as the HTTP driver does, at the SD card's pace,
// in trickles, or not at all.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "mongoose.c"
#pragma GCC diagnostic pop

#include "core1_alloc.h"
#include "net_close.h"

#include "check.h"

#define SERVER_IP 0x0a01a8c0u  // 192.168.1.10
#define CLIENT_IP 0x0201a8c0u  // 192.168.1.2
#define GW_IP 0x0101a8c0u      // 192.168.1.1
#define MASK 0x00ffffffu
#define POOL_SIZE (128u * 1024u)  // Core 1's pool on the device
#define MAX_STREAM (256u * 1024u)

static uint64_t s_now = 1000;
uint64_t mg_millis(void) { return s_now; }
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t) s_now; }

// ── The two heaps, as the client sees them ──────────────────────────────────
// Mongoose: mg_calloc, Core 1's first-fit pool. mbedTLS: umm_*, here counted.
// The server side has neither limit: it is only there to be talked to.

static bool s_client_side = true;
static size_t s_umm_live, s_umm_peak;

void *mg_calloc(size_t count, size_t size) {
  return s_client_side ? core1_calloc(count, size) : calloc(count, size);
}

void mg_free(void *ptr) {
  if (ptr == NULL) return;
  if (core1_owns(ptr)) core1_free(ptr);
  else free(ptr);
}

void *umm_malloc(size_t n) {
  size_t *p = (size_t *) malloc(n + 2 * sizeof(size_t));
  if (p == NULL) return NULL;
  p[0] = s_client_side ? n : 0;
  s_umm_live += p[0];
  if (s_umm_live > s_umm_peak) s_umm_peak = s_umm_live;
  return p + 2;
}

void *umm_calloc(size_t num, size_t size) {
  void *p = umm_malloc(num * size);
  if (p != NULL) memset(p, 0, num * size);
  return p;
}

void umm_free(void *ptr) {
  if (ptr != NULL) {
    size_t *p = (size_t *) ptr - 2;
    s_umm_live -= p[0];
    free(p);
  }
}

static size_t pool_free(void) {
  size_t total, largest;
  core1_alloc_stats(&total, &largest);
  return total;
}

// ── The wire ────────────────────────────────────────────────────────────────

typedef struct {
  struct mg_mgr mgr;
  struct mg_tcpip_if ifp;
  struct mg_tcpip_driver drv;
} side_t;

static side_t s_cli, s_srv;

static struct {
  uint32_t data_frames;     // TCP data frames the client has sent
  uint32_t drop_every;      // lose every Nth of them
  uint32_t drop_first_n;    // lose the first N of them
  uint32_t lost;
  bool silent;              // the server has gone: nothing reaches it
  bool srv_silent;          // nothing reaches the client either
  uint32_t cap_seen;        // highest client seq sent minus highest ack seen
} s_wire;

static bool is_tcp_data(const uint8_t *f, size_t len) {
  const struct eth *e = (const struct eth *) f;
  const struct ip *ip = (const struct ip *) (e + 1);
  const struct tcp *t = (const struct tcp *) (ip + 1);
  if (len < sizeof(*e) + sizeof(*ip) + sizeof(*t)) return false;
  if (e->type != mg_htons(0x800) || ip->proto != 6) return false;
  return mg_ntohs(ip->len) > sizeof(*ip) + (size_t) ((t->off >> 4) * 4);
}

static size_t cli_tx(const void *buf, size_t len, struct mg_tcpip_if *ifp) {
  (void) ifp;
  if (is_tcp_data((const uint8_t *) buf, len)) {
    s_wire.data_frames++;
    if (s_wire.data_frames <= s_wire.drop_first_n ||
        (s_wire.drop_every && s_wire.data_frames % s_wire.drop_every == 0)) {
      s_wire.lost++;
      return len;
    }
  }
  if (!s_wire.silent) mg_tcpip_qwrite((void *) buf, len, &s_srv.ifp);
  return len;
}

static size_t srv_tx(const void *buf, size_t len, struct mg_tcpip_if *ifp) {
  (void) ifp;
  if (!s_wire.srv_silent) mg_tcpip_qwrite((void *) buf, len, &s_cli.ifp);
  return len;
}

static bool if_poll(struct mg_tcpip_if *ifp, bool s1) {
  (void) ifp, (void) s1;
  return true;
}

static void run_ms(uint64_t ms) {
  uint64_t i;
  for (i = 0; i < ms; i++) {
    s_now++;
    s_client_side = true;
    mg_mgr_poll(&s_cli.mgr, 0);
    s_client_side = false;
    mg_mgr_poll(&s_srv.mgr, 0);
  }
  s_client_side = true;
}

// ── The server ──────────────────────────────────────────────────────────────

static char *s_cert;
static uint8_t *s_stream;
static size_t s_stream_len;
static uint32_t s_srv_closed;
static uint32_t s_srv_errors;
static struct mg_connection *s_srv_conn;  // the server's end of the latest one

static void srv_fn(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_ACCEPT) {
    s_srv_conn = c;
    if (c->fn_data == NULL) {  // the TLS listener's (the plain one has a tag)
      struct mg_tls_opts o;
      memset(&o, 0, sizeof(o));
      o.cert = mg_str(s_cert);
      o.key = mg_str(s_cert);
      mg_tls_init(c, &o);
    }
  } else if (ev == MG_EV_READ) {
    size_t n = c->recv.len;
    if (s_stream_len + n <= MAX_STREAM) {
      memcpy(s_stream + s_stream_len, c->recv.buf, n);
      s_stream_len += n;
    }
    mg_iobuf_del(&c->recv, 0, n);
  } else if (ev == MG_EV_ERROR) {
    s_srv_errors++;
    (void) ev_data;
  } else if (ev == MG_EV_CLOSE) {
    s_srv_closed++;
    if (c == s_srv_conn) s_srv_conn = NULL;
  }
}

// The server sends: its buffers are the host's, not Core 1's pool.
static void srv_send(const void *buf, size_t len) {
  s_client_side = false;
  CHECK(s_srv_conn != NULL && mg_send(s_srv_conn, buf, len));
  s_client_side = true;
}

// ── The client ──────────────────────────────────────────────────────────────

// A reader takes what arrives the way the HTTP driver does (http.c): as much
// as the app has room for, at most `rate` bytes a millisecond (the store
// writing to the SD card at 1 MHz: ~80), and is_full while 16 KB or more is
// left unread (HTTP_RECV_PAUSE). What the HTTP driver can be left holding
// before it fails the transfer: HTTP_RECV_OVERFLOW.
#define READER_PAUSE (16u * 1024u)
#define READER_OVERFLOW (32u * 1024u)
// mbedTLS gathers a record in its own buffer, where the window cannot count it,
// and puts it in c->recv whole, so c->recv may end a record above the pause.
// Servers send records of up to 16 KB: 16 + 16 KB, just READER_OVERFLOW. This
// server's are at most MBEDTLS_SSL_OUT_CONTENT_LEN (the firmware's 8 KB).
#define READER_RECV_MAX (READER_PAUSE + MBEDTLS_SSL_OUT_CONTENT_LEN)
#define MAX_DOWN (1024u * 1024u)
static uint8_t *s_down;

typedef struct {
  bool hs_done;
  char err[64];
  bool reader;          // take what arrives (below); off: leave it in c->recv
  size_t rate;          // bytes a millisecond; 0: the app has stopped reading
  size_t burst;         // or, with a period: this much once every period
  uint64_t period;
  uint64_t last_ms;
  size_t got;           // bytes taken, into s_down
  size_t peak_recv;     // the most c->recv held
} cli_app_t;

static void reader_take(struct mg_connection *c, cli_app_t *a) {
  size_t n = a->period ? (s_now / a->period != a->last_ms / a->period ? a->burst : 0)
                       : (size_t) (s_now - a->last_ms) * a->rate;
  a->last_ms = s_now;
  if (c->recv.len > a->peak_recv) a->peak_recv = c->recv.len;
  if (n > c->recv.len) n = c->recv.len;
  if (a->got + n > MAX_DOWN) n = MAX_DOWN - a->got;
  if (n > 0) {
    memcpy(s_down + a->got, c->recv.buf, n);
    a->got += n;
    mg_iobuf_del(&c->recv, 0, n);
  }
  c->is_full = c->recv.len >= READER_PAUSE;
}

static void cli_fn(struct mg_connection *c, int ev, void *ev_data) {
  cli_app_t *a = (cli_app_t *) c->fn_data;
  if (a == NULL) return;
  if (ev == MG_EV_TLS_HS) a->hs_done = true;
  if (ev == MG_EV_ERROR) snprintf(a->err, sizeof(a->err), "%s", (char *) ev_data);
  if (a->reader && (ev == MG_EV_READ || ev == MG_EV_POLL)) reader_take(c, a);
}

static void side_up(side_t *s, uint32_t ip, uint8_t mac_last,
                    size_t (*tx)(const void *, size_t, struct mg_tcpip_if *)) {
  memset(s, 0, sizeof(*s));
  mg_mgr_init(&s->mgr);
  s->drv.tx = tx;
  s->drv.poll = if_poll;
  s->ifp.driver = &s->drv;
  s->ifp.mac[0] = 2, s->ifp.mac[5] = mac_last;
  s->ifp.ip = ip;
  s->ifp.mask = MASK;
  s->ifp.gw = GW_IP;
  s->ifp.recv_queue.size = 16384;
  s->ifp.enable_tcp_retransmit = true;
  mg_tcpip_init(&s->mgr, &s->ifp);
}

static void rig_up(void) {
  memset(&s_wire, 0, sizeof(s_wire));
  s_stream_len = 0;
  s_srv_closed = s_srv_errors = 0;
  s_client_side = true;
  side_up(&s_cli, CLIENT_IP, 2, cli_tx);
  s_client_side = false;
  side_up(&s_srv, SERVER_IP, 10, srv_tx);
  CHECK(mg_listen(&s_srv.mgr, "tcp://0.0.0.0:443", srv_fn, NULL) != NULL);
  CHECK(mg_listen(&s_srv.mgr, "tcp://0.0.0.0:80", srv_fn, (void *) "plain") !=
        NULL);
  s_srv_conn = NULL;
  s_client_side = true;
  run_ms(2100);
  CHECK_EQ_INT(s_cli.ifp.state, MG_TCPIP_STATE_READY);
  CHECK_EQ_INT(s_srv.ifp.state, MG_TCPIP_STATE_READY);
}

static void rig_down(void) {
  s_client_side = true;
  mg_mgr_free(&s_cli.mgr);
  s_client_side = false;
  mg_mgr_free(&s_srv.mgr);
  s_client_side = true;
}

static struct mg_connection *tls_connect(cli_app_t *a) {
  struct mg_tls_opts o;
  struct mg_connection *c;
  memset(a, 0, sizeof(*a));
  c = mg_connect(&s_cli.mgr, "tls://192.168.1.10:443", cli_fn, a);
  CHECK(c != NULL);
  if (c == NULL) return NULL;
  memset(&o, 0, sizeof(o));
  o.name = mg_str("localhost");
  mg_tls_init(c, &o);  // as wifi.c's tls_start: no CA, so no verification
  CHECK(c->is_tls_hs);
  return c;
}

static void pattern(uint8_t *buf, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) buf[i] = (uint8_t) (i * 31 + (i >> 8));
}

static bool stream_is_pattern(size_t n) {
  size_t i;
  if (s_stream_len != n) return false;
  for (i = 0; i < n; i++)
    if (s_stream[i] != (uint8_t) (i * 31 + (i >> 8))) return false;
  return true;
}

static bool conn_alive(struct mg_connection *c) {
  struct mg_connection *x;
  for (x = s_cli.mgr.conns; x != NULL; x = x->next)
    if (x == c) return true;
  return false;
}

// ── Tests ───────────────────────────────────────────────────────────────────

// More than one record, and each record more than may be in flight: mbedTLS is
// throttled on every one. Before upstream's fix mg_tls_send said all of c->send
// had gone and the rest of it was lost.
static void test_a_large_write_arrives_whole(void) {
  static uint8_t buf[60000];
  cli_app_t a;
  struct mg_connection *c;
  rig_up();
  c = tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  pattern(buf, sizeof(buf));
  CHECK(mg_send(c, buf, sizeof(buf)));
  run_ms(3000);
  CHECK_EQ_INT(c->send.len, 0);
  CHECK_EQ_INT(s_stream_len, sizeof(buf));
  CHECK(stream_is_pattern(sizeof(buf)));
  CHECK(a.err[0] == '\0');
  CHECK_EQ_INT(s_srv_errors, 0);
  rig_down();
}

// Growing c->send while throttled: the record in flight is the old one's.
static void test_writes_during_a_throttle_stay_in_order(void) {
  static uint8_t buf[40000];
  cli_app_t a;
  struct mg_connection *c;
  size_t off = 0;
  rig_up();
  c = tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  pattern(buf, sizeof(buf));
  while (off < sizeof(buf)) {
    size_t n = sizeof(buf) - off < 7000 ? sizeof(buf) - off : 7000;
    CHECK(mg_send(c, buf + off, n));
    off += n;
    run_ms(1);
  }
  run_ms(3000);
  CHECK(stream_is_pattern(sizeof(buf)));
  CHECK_EQ_INT(s_srv_errors, 0);
  rig_down();
}

static void test_lost_frames_do_not_touch_the_stream(void) {
  static uint8_t buf[60000];
  cli_app_t a;
  struct mg_connection *c;
  rig_up();
  c = tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  s_wire.drop_every = 7;
  pattern(buf, sizeof(buf));
  CHECK(mg_send(c, buf, sizeof(buf)));
  run_ms(120000);
  CHECK(s_wire.lost > 0);
  CHECK(s_cli.ifp.nrtx > 0);
  CHECK(stream_is_pattern(sizeof(buf)));
  CHECK_EQ_INT(s_srv_errors, 0);
  CHECK(a.err[0] == '\0');
  rig_down();
}

static void test_a_lost_client_hello_is_resent(void) {
  cli_app_t a;
  struct mg_connection *c;
  rig_up();
  s_wire.drop_first_n = 2;  // the ClientHello, then its first resend
  c = tls_connect(&a);
  run_ms(2500);
  CHECK(!a.hs_done);
  CHECK_EQ_INT(s_wire.lost, 2);
  run_ms(1000);
  CHECK(a.hs_done);
  CHECK(a.err[0] == '\0');
  (void) c;
  rig_down();
}

// Everything written before a close is delivered, with the FIN after it, and
// all that the connection held comes back.
static void test_close_delivers_what_was_written(void) {
  static uint8_t buf[30000];
  cli_app_t a;
  struct mg_connection *c;
  size_t pool0, umm0;
  rig_up();
  pool0 = pool_free();
  umm0 = s_umm_live;
  c = tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  printf("connected: pool %zu B, mbedTLS %zu B\n", pool0 - pool_free(),
         s_umm_live - umm0);
  pattern(buf, sizeof(buf));
  CHECK(mg_send(c, buf, sizeof(buf)));
  net_close_conn(c);
  CHECK(!c->is_closing);
  run_ms(100);
  CHECK(stream_is_pattern(sizeof(buf)));
  run_ms(2000);
  CHECK(!conn_alive(c));
  CHECK_EQ_INT(s_srv_closed, 1);
  CHECK_EQ_INT(s_srv_errors, 0);  // close_notify, then the FIN: a clean end
  CHECK_EQ_INT(pool_free(), pool0);
  CHECK_EQ_INT(s_umm_live, umm0);
  rig_down();
}

// Closing with a lost frame in flight: mbedTLS is long gone, the ciphertext
// is resent from the queue, and the server decrypts all of it.
static void test_the_queue_outlives_tls(void) {
  static uint8_t buf[4000];
  cli_app_t a;
  struct mg_connection *c;
  uint32_t lost;
  size_t umm0;
  rig_up();
  umm0 = s_umm_live;
  c = tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  pattern(buf, sizeof(buf));
  CHECK(mg_send(c, buf, sizeof(buf)));
  s_wire.drop_first_n = s_wire.data_frames + 1;  // the next frame
  lost = s_wire.lost;
  net_close_conn(c);
  run_ms(50);
  CHECK_EQ_INT(s_wire.lost, lost + 1);
  CHECK(conn_alive(c));
  CHECK(c->tls == NULL);
  CHECK_EQ_INT(s_umm_live, umm0);  // mbedTLS's memory has gone already
  run_ms(10000);
  CHECK(stream_is_pattern(sizeof(buf)));
  CHECK(!conn_alive(c));
  CHECK_EQ_INT(s_srv_errors, 0);
  rig_down();
}

// A peer gone silent: the close_notify is never acknowledged, so the connection
// lasts until the limit, but what it holds is that one record, not a TLS context.
static void test_a_silent_peer_holds_almost_nothing(void) {
  cli_app_t a;
  struct mg_connection *c;
  size_t pool0, umm0, pool_up;
  rig_up();
  pool0 = pool_free();
  umm0 = s_umm_live;
  c = tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  pool_up = pool0 - pool_free();
  s_wire.silent = true;
  net_close_conn(c);
  run_ms(5);
  CHECK(conn_alive(c));
  CHECK_EQ_INT(s_umm_live, umm0);
  printf("closing, silent peer: pool %zu B (connected: %zu B), mbedTLS 0 B\n",
         pool0 - pool_free(), pool_up);
  CHECK(pool0 - pool_free() < 2048);
  run_ms(NET_CLOSE_LIMIT_MS + 5);
  CHECK(!conn_alive(c));
  CHECK_EQ_INT(pool_free(), pool0);
  rig_down();
}

// The worry that kept the FIN out: open, write and close TLS sockets over and
// over with peers that answer nothing after the handshake, at about the pace
// the device can do handshakes. Nothing may run out, and it all comes back.
static void test_open_close_loop_against_silent_peers(void) {
  uint8_t buf[1000];
  size_t pool0, umm0, pool_min, umm_max = 0;
  int i, alive_max = 0;
  rig_up();
  pool0 = pool_min = pool_free();
  umm0 = s_umm_live;
  pattern(buf, sizeof(buf));
  for (i = 0; i < 40; i++) {
    cli_app_t a;
    struct mg_connection *c, *x;
    int alive = 0;
    s_wire.silent = false;
    c = tls_connect(&a);
    run_ms(50);
    CHECK(a.hs_done);
    CHECK(mg_send(c, buf, sizeof(buf)));
    s_wire.silent = true;  // the data goes nowhere, and neither will the FIN
    run_ms(5);
    net_close_conn(c);
    if (pool_free() < pool_min) pool_min = pool_free();
    if (s_umm_live - umm0 > umm_max) umm_max = s_umm_live - umm0;
    run_ms(945);
    for (x = s_cli.mgr.conns; x != NULL; x = x->next) alive++;
    if (alive > alive_max) alive_max = alive;
    CHECK(s_umm_live == umm0);
  }
  printf("40 cycles, 1 s each: at most %d connections, pool low %zu B of %u, "
         "mbedTLS peak %zu B\n", alive_max, pool_min, POOL_SIZE, umm_max);
  CHECK(alive_max <= NET_CLOSE_LIMIT_MS / 1000 + 1);
  s_wire.silent = false;
  run_ms(NET_CLOSE_LIMIT_MS + 100);
  CHECK(s_cli.mgr.conns == NULL);
  CHECK_EQ_INT(pool_free(), pool0);
  CHECK_EQ_INT(s_umm_live, umm0);
  rig_down();
}

// ── Tests: the receive window ───────────────────────────────────────────────

static void reader_start(cli_app_t *a, size_t rate) {
  a->reader = true;
  a->rate = rate;
  a->last_ms = s_now;
}

// Runs until the reader has `want` bytes, an error, or `limit_ms`; returns the
// lowest the client's pool got.
static size_t run_until_read(cli_app_t *a, size_t want, uint64_t limit_ms) {
  size_t pool_min = pool_free();
  uint64_t t;
  for (t = 0; t < limit_ms && a->got < want && a->err[0] == '\0'; t++) {
    run_ms(1);
    if (pool_free() < pool_min) pool_min = pool_free();
  }
  return pool_min;
}

static bool down_is(const uint8_t *buf, size_t n, const cli_app_t *a) {
  return a->got == n && memcmp(s_down, buf, n) == 0;
}

static void slow_tls_download(size_t rate, size_t n) {
  static uint8_t buf[MAX_DOWN];
  cli_app_t a;
  size_t pool0, pool_min;
  rig_up();
  pool0 = pool_free();
  tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  reader_start(&a, rate);
  pattern(buf, n);
  srv_send(buf, n);
  pool_min = run_until_read(&a, n, n / rate + 5000);
  CHECK_STR(a.err, "");
  CHECK(down_is(buf, n, &a));
  CHECK(a.peak_recv <= READER_RECV_MAX);
  printf("%zu KiB over TLS at %zu B/ms: c->recv peak %zu B, pool peak %zu B\n",
         n / 1024, rate, a.peak_recv, pool0 - pool_min);
  rig_down();
}

// The store's download on the device: the server sends far faster than the app
// can write to the SD card (1 MHz while the radio is on, ~80 KB/s). The window
// has to hold the server back. Before, the device took all it was sent, the
// raw TLS piled up behind the full c->recv and Core 1's pool ran out: "partial:
// oom", or "TLS recv error" once a record had been cut short.
static void test_a_slow_reader_gets_a_large_download_whole(void) {
  slow_tls_download(80, MAX_DOWN);
}

// Whatever the pace, c->recv stays within what the HTTP driver accepts: how
// many records mbedTLS has taken in when the window shuts depends on it.
static void test_any_slow_reader_stays_within_the_drivers_limit(void) {
  static const size_t rates[] = {20, 50, 150, 400, 1000, 2500};
  size_t i;
  for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    slow_tls_download(rates[i], 256 * 1024);
}

// An app that has fallen behind and reads a little now and then: each read lets
// a little more in, and mbedTLS gathers it into a record (in its own buffer,
// where the window does not count it). When the record is whole it lands in
// c->recv on top of what is there: c->recv may end at most a record (16 KB)
// above where the window shuts, and that has to stay within the 32 KB the
// drivers accept.
static void test_a_trickling_reader_stays_within_the_drivers_limit(void) {
  static uint8_t buf[256 * 1024];
  cli_app_t a;
  rig_up();
  tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  reader_start(&a, 0);
  pattern(buf, sizeof(buf));
  srv_send(buf, sizeof(buf));
  run_ms(3000);  // behind: the window has shut
  a.burst = 1500, a.period = 100;
  run_until_read(&a, 64 * 1024, 20000);
  CHECK_STR(a.err, "");
  CHECK(a.got >= 64 * 1024);
  CHECK(a.peak_recv <= READER_RECV_MAX);
  printf("trickling reader: c->recv peak %zu B\n", a.peak_recv);
  rig_down();
}

// The app stops reading altogether: the window shuts and the server waits. A
// Mongoose server (this one) never probes a shut window, so when the app reads
// again the client has to say so by itself.
static void test_a_reader_that_stops_holds_the_server_back(void) {
  static uint8_t buf[256 * 1024];
  cli_app_t a;
  struct mg_connection *c;
  rig_up();
  c = tls_connect(&a);
  run_ms(100);
  CHECK(a.hs_done);
  reader_start(&a, 0);
  pattern(buf, sizeof(buf));
  srv_send(buf, sizeof(buf));
  run_ms(5000);
  CHECK_STR(a.err, "");
  CHECK(conn_alive(c));
  CHECK(a.peak_recv <= READER_RECV_MAX);
  CHECK(s_srv_conn != NULL);
  if (s_srv_conn != NULL) {
    CHECK_EQ_INT(((struct connstate *) (s_srv_conn + 1))->win, 0);
    CHECK(s_srv_conn->send.len > sizeof(buf) / 2);  // still the server's
  }
  printf("stopped reader: c->recv %zu B (peak %zu), c->rtls %zu B\n",
         c->recv.len, a.peak_recv, c->rtls.len);
  reader_start(&a, 64 * 1024);  // the app catches up
  run_until_read(&a, sizeof(buf), 3000);
  CHECK_STR(a.err, "");
  CHECK(down_is(buf, sizeof(buf), &a));
  rig_down();
}

// Plain HTTP (a LAN server): the bytes go straight to c->recv.
static void test_a_slow_plain_reader_gets_a_large_download_whole(void) {
  static uint8_t buf[MAX_DOWN];
  cli_app_t a;
  memset(&a, 0, sizeof(a));
  rig_up();
  CHECK(mg_connect(&s_cli.mgr, "tcp://192.168.1.10:80", cli_fn, &a) != NULL);
  run_ms(100);
  CHECK(s_srv_conn != NULL);
  reader_start(&a, 80);
  pattern(buf, sizeof(buf));
  srv_send(buf, sizeof(buf));
  run_until_read(&a, sizeof(buf), 20000);
  CHECK_STR(a.err, "");
  CHECK(down_is(buf, sizeof(buf), &a));
  CHECK(a.peak_recv <= READER_OVERFLOW);
  rig_down();
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  char *buf;
  long n;
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (char *) malloc((size_t) n + 1);
  if (fread(buf, 1, (size_t) n, f) != (size_t) n) n = 0;
  buf[n] = '\0';
  fclose(f);
  return buf;
}

int main(void) {
  static uint8_t pool[POOL_SIZE] __attribute__((aligned(8)));
  s_cert = read_file(PICODECK_ROOT "/tests/e2e/certs/selfsigned.pem");
  CHECK(s_cert != NULL);
  s_stream = (uint8_t *) malloc(MAX_STREAM);
  s_down = (uint8_t *) malloc(MAX_DOWN);
  core1_alloc_init(pool, sizeof(pool));
  mg_log_set(MG_LL_NONE);

  test_a_large_write_arrives_whole();
  test_writes_during_a_throttle_stay_in_order();
  test_lost_frames_do_not_touch_the_stream();
  test_a_lost_client_hello_is_resent();
  test_close_delivers_what_was_written();
  test_the_queue_outlives_tls();
  test_a_silent_peer_holds_almost_nothing();
  test_open_close_loop_against_silent_peers();
  test_a_slow_reader_gets_a_large_download_whole();
  test_any_slow_reader_stays_within_the_drivers_limit();
  test_a_trickling_reader_stays_within_the_drivers_limit();
  test_a_reader_that_stops_holds_the_server_back();
  test_a_slow_plain_reader_gets_a_large_download_whole();

  free(s_down);
  free(s_stream);
  free(s_cert);
  return check_report("test_mg_tls_resend");
}
