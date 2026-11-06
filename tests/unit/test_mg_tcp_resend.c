// Mongoose's built-in TCP (third_party/mongoose/mongoose.c, the device's stack)
// against a scripted peer, on a virtual clock. The simulator's firmware-net
// build cannot do this: it runs Mongoose on BSD sockets, without this code.
//
// The peer behaves as a host's TCP does where it matters here: it answers ARP,
// keeps segments that arrive out of order and acknowledges cumulatively. What
// it does beyond that (lose a frame, close its window, go silent) each test
// asks for. The driver's tx hands it every frame the stack sends, so a frame
// "lost" here is one the driver accepted, the case the device's stack could
// not recover from.
//
//   ./test_mg_tcp_resend             run everything
//   TCP_RESEND_OFF=1 ./test_mg_tcp_resend
//                                    the resend off: the tests that depend on
//                                    it must fail (how the test is checked)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "mongoose.c"
#pragma GCC diagnostic pop

#include "net_close.h"

#include "check.h"

#define PEER_IP 0x0a01a8c0u    // 192.168.1.10, network order on a little-endian host
#define CLIENT_IP 0x0201a8c0u  // 192.168.1.2
#define GW_IP 0x0101a8c0u      // 192.168.1.1
#define MASK 0x00ffffffu
#define PEER_PORT 5000
#define PEER_ISN 0x1000u
#define STREAM_MAX (512u * 1024u)

static uint64_t s_now = 1000;
uint64_t mg_millis(void) { return s_now; }

static const uint8_t s_peer_mac[6] = {2, 0, 0, 0, 0, 0x10};
static const uint8_t s_client_mac[6] = {2, 0, 0, 0, 0, 0x02};

// ── The peer ────────────────────────────────────────────────────────────────

typedef struct {
  struct mg_tcpip_if *ifp;
  uint16_t client_port;
  uint32_t first_seq;  // the client's first data byte
  uint32_t rcv_nxt;
  uint32_t snd_nxt;
  uint32_t start_seq;  // the sequence number to start the client at, 0 = its own
  uint16_t win;
  uint8_t *rx;        // the stream as received, by offset from first_seq
  uint8_t *have;      // one flag per byte
  size_t in_order;    // bytes received in order so far

  // What the test asks for
  bool dead;          // no answer to anything
  bool fin_back;      // answer a FIN with a FIN of its own
  bool ack_only;      // acknowledge a FIN and stay open (the app has not closed)
  bool no_ack;        // take data, acknowledge nothing
  uint32_t drop_at;   // lose the data frames numbered from here (1 = first) ...
  uint32_t drop_n;    // ... this many
  uint32_t refuse;    // the driver refuses this many frames

  // What it saw
  uint32_t syns;
  uint32_t data_frames;
  uint32_t resent_frames;  // data frames below the highest seq seen
  uint32_t max_seq_end;
  uint32_t lost_frames;
  uint32_t fins;
  uint32_t fin_seq;
  uint32_t rsts;
  uint64_t fin_at;         // virtual time of the first FIN
  uint32_t max_in_flight;  // highest seq sent minus rcv_nxt at the time
  uint32_t frames_refused;
  uint64_t data_times[32];  // when each frame of an ending stream was seen
  uint32_t n_data_times;
} peer_t;

static peer_t s_peer;

static uint16_t csum_add(const void *buf, size_t len, uint32_t sum) {
  const uint8_t *p = (const uint8_t *) buf;
  while (len > 1) {
    sum += (uint32_t) ((p[0] << 8) | p[1]);
    p += 2;
    len -= 2;
  }
  if (len) sum += (uint32_t) (p[0] << 8);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t) sum;
}

static void peer_send(peer_t *p, uint8_t flags, uint32_t seq, uint32_t ack,
                      const void *data, size_t len) {
  uint8_t frame[1600], *f = frame;
  struct eth *e = (struct eth *) f;
  struct ip *ip = (struct ip *) (e + 1);
  struct tcp *t = (struct tcp *) (ip + 1);
  size_t hlen = sizeof(*t) + ((flags & TH_SYN) ? 4 : 0), tcplen = hlen + len;
  uint8_t pseudo[12];
  uint32_t sum;
  memset(frame, 0, sizeof(frame));
  memcpy(e->dst, s_client_mac, 6);
  memcpy(e->src, s_peer_mac, 6);
  e->type = mg_htons(0x800);
  ip->ver = 0x45;
  ip->len = mg_htons((uint16_t) (sizeof(*ip) + tcplen));
  ip->ttl = 64;
  ip->proto = 6;
  ip->src = PEER_IP;
  ip->dst = CLIENT_IP;
  ip->csum = (uint16_t) mg_htons((uint16_t) ~csum_add(ip, sizeof(*ip), 0));
  t->sport = mg_htons(PEER_PORT);
  t->dport = mg_htons(p->client_port);
  t->seq = mg_htonl(seq);
  t->ack = mg_htonl(ack);
  t->off = (uint8_t) ((hlen / 4) << 4);
  t->flags = flags;
  t->win = mg_htons(p->win);
  if (flags & TH_SYN) {  // MSS 1460
    uint8_t *o = (uint8_t *) (t + 1);
    o[0] = 2, o[1] = 4, o[2] = 0x05, o[3] = 0xb4;
  }
  if (len) memcpy((uint8_t *) t + hlen, data, len);
  memcpy(pseudo, &ip->src, 8);
  pseudo[8] = 0, pseudo[9] = 6;
  pseudo[10] = (uint8_t) (tcplen >> 8), pseudo[11] = (uint8_t) tcplen;
  sum = csum_add(pseudo, sizeof(pseudo), 0);
  t->csum = (uint16_t) mg_htons((uint16_t) ~csum_add(t, tcplen, sum));
  mg_tcpip_qwrite(frame, sizeof(*e) + sizeof(*ip) + tcplen, p->ifp);
}

static void peer_ack(peer_t *p) {
  peer_send(p, TH_ACK, p->snd_nxt, p->rcv_nxt, NULL, 0);
}

static void peer_rx_arp(peer_t *p, const uint8_t *frame) {
  const struct arp *a = (const struct arp *) (frame + sizeof(struct eth));
  uint8_t out[sizeof(struct eth) + sizeof(struct arp)];
  struct eth *e = (struct eth *) out;
  struct arp *r = (struct arp *) (e + 1);
  if (a->op != mg_htons(1) || a->tpa != PEER_IP) return;
  memset(out, 0, sizeof(out));
  memcpy(e->dst, s_client_mac, 6);
  memcpy(e->src, s_peer_mac, 6);
  e->type = mg_htons(0x806);
  *r = *a;
  r->op = mg_htons(2);
  memcpy(r->sha, s_peer_mac, 6);
  r->spa = PEER_IP;
  memcpy(r->tha, a->sha, 6);
  r->tpa = a->spa;
  mg_tcpip_qwrite(out, sizeof(out), p->ifp);
}

static void peer_rx_data(peer_t *p, uint32_t seq, const uint8_t *d, size_t n) {
  uint32_t off = seq - p->first_seq, end = off + (uint32_t) n, sent_end;
  if (off + n <= STREAM_MAX) {
    size_t i;
    for (i = 0; i < n; i++) {
      p->rx[off + i] = d[i];
      p->have[off + i] = 1;
    }
    while (p->in_order < STREAM_MAX && p->have[p->in_order]) p->in_order++;
    p->rcv_nxt = p->first_seq + (uint32_t) p->in_order;
  }
  sent_end = end;
  if (sent_end > p->max_seq_end) p->max_seq_end = sent_end;
  if (p->max_seq_end - (uint32_t) p->in_order > p->max_in_flight)
    p->max_in_flight = p->max_seq_end - (uint32_t) p->in_order;
}

static void peer_rx_tcp(peer_t *p, const uint8_t *frame) {
  const struct ip *ip = (const struct ip *) (frame + sizeof(struct eth));
  const struct tcp *t = (const struct tcp *) (ip + 1);
  size_t hlen = (size_t) ((t->off >> 4) * 4);
  size_t n = (size_t) mg_ntohs(ip->len) - sizeof(*ip) - hlen;
  const uint8_t *data = (const uint8_t *) t + hlen;
  uint32_t seq = mg_ntohl(t->seq);

  if (t->flags & TH_RST) {
    p->rsts++;
    return;
  }
  if (t->flags & TH_SYN) {
    uint32_t client_seq;
    p->syns++;
    p->client_port = mg_ntohs(t->sport);
    client_seq = p->start_seq ? p->start_seq : seq + 1;
    p->first_seq = client_seq;
    p->rcv_nxt = client_seq;
    p->snd_nxt = PEER_ISN + 1;
    peer_send(p, TH_SYN | TH_ACK, PEER_ISN, client_seq, NULL, 0);
    return;
  }
  if (n > 0) {
    p->data_frames++;
    if (p->n_data_times < 32) p->data_times[p->n_data_times++] = s_now;
    if (p->drop_n && p->data_frames >= p->drop_at &&
        p->data_frames < p->drop_at + p->drop_n) {
      p->lost_frames++;
      return;
    }
    if (seq - p->first_seq + n <= p->max_seq_end) p->resent_frames++;
    peer_rx_data(p, seq, data, n);
    if (!p->no_ack) peer_ack(p);
  } else if ((t->flags & TH_FIN) == 0 && seq == p->rcv_nxt - 1) {
    peer_ack(p);  // a keep-alive probe
  }
  if (t->flags & TH_FIN) {
    uint32_t fin_end = seq + (uint32_t) n + 1;
    if (p->fins++ == 0) {
      p->fin_at = s_now;
      p->fin_seq = seq + (uint32_t) n;
    }
    if (p->no_ack || p->rcv_nxt != fin_end - 1) return;  // out of order: wait
    p->rcv_nxt = fin_end;
    if (p->ack_only) {
      peer_ack(p);
    } else if (p->fin_back) {
      peer_send(p, TH_FIN | TH_ACK, p->snd_nxt, p->rcv_nxt, NULL, 0);
      p->snd_nxt++;
    }
  }
}

// ── The fake driver ─────────────────────────────────────────────────────────

static size_t if_tx(const void *buf, size_t len, struct mg_tcpip_if *ifp) {
  peer_t *p = &s_peer;
  const uint8_t *frame = (const uint8_t *) buf;
  const struct eth *e = (const struct eth *) frame;
  (void) ifp;
  if (p->refuse > 0) {
    p->refuse--;
    p->frames_refused++;
    return 0;
  }
  if (!p->dead) {
    if (e->type == mg_htons(0x806)) peer_rx_arp(p, frame);
    if (e->type == mg_htons(0x800)) peer_rx_tcp(p, frame);
  }
  return len;
}

static bool if_poll(struct mg_tcpip_if *ifp, bool s1) {
  (void) ifp, (void) s1;
  return true;
}

// ── The app ─────────────────────────────────────────────────────────────────

typedef struct {
  bool connected;
  bool closed;
  char err[64];
  size_t writes;
} app_t;

static void app_fn(struct mg_connection *c, int ev, void *ev_data) {
  app_t *a = (app_t *) c->fn_data;
  if (a == NULL) return;
  if (ev == MG_EV_CONNECT) a->connected = true;
  if (ev == MG_EV_CLOSE) a->closed = true;
  if (ev == MG_EV_WRITE) a->writes += (size_t) *(long *) ev_data;
  if (ev == MG_EV_ERROR) snprintf(a->err, sizeof(a->err), "%s", (char *) ev_data);
}

// ── The harness ─────────────────────────────────────────────────────────────

typedef struct {
  struct mg_mgr mgr;
  struct mg_tcpip_if ifp;
  struct mg_tcpip_driver drv;
  struct mg_connection *c;
  struct connstate *s;
  app_t app;
} rig_t;

static bool resend_on(void) {
  const char *off = getenv("TCP_RESEND_OFF");
  return off == NULL || off[0] != '1';
}

static void run_ms(rig_t *r, uint64_t ms) {
  uint64_t i;
  for (i = 0; i < ms; i++) {
    s_now++;
    mg_mgr_poll(&r->mgr, 0);
  }
}

// Up, connected, the peer set up by `setup` first.
static void rig_up(rig_t *r, bool resend, void (*setup)(peer_t *)) {
  uint8_t *rx = s_peer.rx, *have = s_peer.have;
  memset(r, 0, sizeof(*r));
  memset(&s_peer, 0, sizeof(s_peer));
  if (!rx) rx = (uint8_t *) malloc(STREAM_MAX);
  if (!have) have = (uint8_t *) malloc(STREAM_MAX);
  memset(have, 0, STREAM_MAX);
  s_peer.rx = rx, s_peer.have = have;
  s_peer.ifp = &r->ifp;
  s_peer.win = 65535;
  s_peer.fin_back = true;
  if (setup) setup(&s_peer);

  mg_log_set(MG_LL_NONE);
  mg_mgr_init(&r->mgr);
  r->drv.tx = if_tx;
  r->drv.poll = if_poll;
  r->ifp.driver = &r->drv;
  memcpy(r->ifp.mac, s_client_mac, 6);
  r->ifp.ip = CLIENT_IP;
  r->ifp.mask = MASK;
  r->ifp.gw = GW_IP;
  r->ifp.recv_queue.size = 16384;
  r->ifp.enable_tcp_retransmit = resend && resend_on();
  mg_tcpip_init(&r->mgr, &r->ifp);
  run_ms(r, 2100);
  CHECK_EQ_INT(r->ifp.state, MG_TCPIP_STATE_READY);

  r->c = mg_connect(&r->mgr, "tcp://192.168.1.10:5000", app_fn, &r->app);
  CHECK(r->c != NULL);
  r->s = (struct connstate *) (r->c + 1);
  run_ms(r, 50);
  CHECK(r->app.connected);
}

static void rig_down(rig_t *r) {
  mg_mgr_free(&r->mgr);  // closes what is left, and the interface
}

static void pattern(uint8_t *buf, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) buf[i] = (uint8_t) (i * 31 + (i >> 8));
}

static bool peer_has_pattern(size_t n) {
  size_t i;
  if (s_peer.in_order != n) return false;
  for (i = 0; i < n; i++)
    if (s_peer.rx[i] != (uint8_t) (i * 31 + (i >> 8))) return false;
  return true;
}

static bool conn_alive(rig_t *r, struct mg_connection *c) {
  struct mg_connection *x;
  for (x = r->mgr.conns; x != NULL; x = x->next)
    if (x == c) return true;
  return false;
}

// ── Tests: the resend ───────────────────────────────────────────────────────

static void drop_second_frame(peer_t *p) {
  p->drop_at = 2;
  p->drop_n = 1;
}

// What the device did: the hole stays and nothing says so on either side.
static void test_without_the_resend_a_lost_frame_stalls(void) {
  rig_t r;
  uint8_t buf[6000];
  pattern(buf, sizeof(buf));
  rig_up(&r, false, drop_second_frame);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 20);
  CHECK_EQ_INT(s_peer.lost_frames, 1);
  CHECK_EQ_INT(s_peer.resent_frames, 0);
  CHECK(s_peer.in_order < sizeof(buf));
  run_ms(&r, 60000);
  CHECK(s_peer.in_order < sizeof(buf));
  CHECK_EQ_INT(s_peer.resent_frames, 0);
  CHECK(r.app.err[0] == '\0');
  CHECK(conn_alive(&r, r.c));
  rig_down(&r);
}

static void test_a_lost_frame_is_resent_after_a_second(void) {
  rig_t r;
  uint8_t buf[6000];
  uint64_t t0;
  pattern(buf, sizeof(buf));
  rig_up(&r, true, drop_second_frame);
  mg_send(r.c, buf, sizeof(buf));
  t0 = s_now;
  run_ms(&r, 900);
  CHECK_EQ_INT(s_peer.lost_frames, 1);
  CHECK(s_peer.in_order < sizeof(buf));  // not yet
  run_ms(&r, 200);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK(s_peer.resent_frames >= 1);
  CHECK(s_now - t0 >= 1000);
  CHECK(r.app.err[0] == '\0');
  rig_down(&r);
}

static void drop_first_frame(peer_t *p) {
  p->drop_at = 1;
  p->drop_n = 1;
}

// The pattern that stalled on the device: many one-byte writes, one frame lost.
static void test_small_writes_survive_a_lost_frame(void) {
  rig_t r;
  int i;
  rig_up(&r, true, drop_first_frame);
  for (i = 0; i < 200; i++) {
    uint8_t b = (uint8_t) (i * 31 + (i >> 8));
    mg_send(r.c, &b, 1);
    run_ms(&r, 5);
  }
  run_ms(&r, 1500);
  CHECK(peer_has_pattern(200));
  CHECK_EQ_INT(s_peer.lost_frames, 1);
  rig_down(&r);
}

static void drop_many_frames(peer_t *p) {
  p->drop_at = 3;
  p->drop_n = 6;
}

static void test_several_lost_frames_each_come_back(void) {
  rig_t r;
  static uint8_t buf[40000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, drop_many_frames);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 30000);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK_EQ_INT(r.c->send.len, 0);
  CHECK_EQ_INT(r.s->txq.len, 0);
  CHECK(r.s->txq.buf == NULL);
  rig_down(&r);
}

// Sequence numbers across the 32-bit wrap: every comparison is modular.
static void start_near_the_wrap(peer_t *p) {
  p->start_seq = 0xfffff000u;
  p->drop_at = 4;
  p->drop_n = 1;
}

static void test_sequence_numbers_wrap(void) {
  rig_t r;
  static uint8_t buf[20000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, start_near_the_wrap);
  CHECK_EQ_U32(r.s->seq, 0xfffff000u);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 5000);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK_EQ_INT(r.s->txq.len, 0);
  rig_down(&r);
}

// ── Tests: what it holds ────────────────────────────────────────────────────

static void peer_takes_without_acking(peer_t *p) { p->no_ack = true; }

static void test_in_flight_and_queue_stay_capped(void) {
  rig_t r;
  static uint8_t buf[64000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, peer_takes_without_acking);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 500);
  CHECK(r.s->txq.len <= MG_TCPIP_WIN);
  CHECK_EQ_U32(r.s->seq - r.s->txq_una, r.s->txq.len);
  CHECK(r.s->txq.size <= MG_TCPIP_WIN + MG_TCPIP_TXQ_ALIGN);
  CHECK(r.c->send.len >= sizeof(buf) - MG_TCPIP_WIN);
  rig_down(&r);
}

// Upstream keeps a length prefix per segment: 6000 one-byte writes would hold
// 30 KB. As one run of bytes it is 6000, whatever the writes were.
static void test_small_writes_do_not_inflate_the_queue(void) {
  rig_t r;
  int i;
  rig_up(&r, true, peer_takes_without_acking);
  for (i = 0; i < 7000; i++) {
    uint8_t b = (uint8_t) i;
    mg_send(r.c, &b, 1);
    run_ms(&r, 1);
  }
  CHECK_EQ_INT(r.s->txq.len, MG_TCPIP_WIN);
  CHECK(r.s->txq.size <= MG_TCPIP_WIN + MG_TCPIP_TXQ_ALIGN);
  rig_down(&r);
}

static void test_queue_goes_when_acknowledged(void) {
  rig_t r;
  uint8_t buf[3000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, NULL);
  CHECK(r.s->txq.buf == NULL);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 20);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK(r.s->txq.buf == NULL);
  CHECK_EQ_INT(r.s->txq.size, 0);
  rig_down(&r);
}

static void small_window(peer_t *p) { p->win = 2000; }

static void test_the_peers_window_is_honoured(void) {
  rig_t r;
  static uint8_t buf[20000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, small_window);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 2000);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK(s_peer.max_in_flight <= 2000);
  rig_down(&r);
}

static void test_a_zero_window_holds_then_resumes(void) {
  rig_t r;
  static uint8_t buf[12000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, NULL);
  s_peer.win = 0;
  peer_ack(&s_peer);  // tells the client the window is shut
  run_ms(&r, 5);
  CHECK_EQ_INT(r.s->win, 0);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 3000);
  CHECK_EQ_INT(s_peer.data_frames, 0);
  CHECK_EQ_INT(r.c->send.len, sizeof(buf));
  CHECK(r.app.err[0] == '\0');
  s_peer.win = 65535;
  peer_ack(&s_peer);  // the window opens
  run_ms(&r, 2000);
  CHECK(peer_has_pattern(sizeof(buf)));
  rig_down(&r);
}

// A lost window update: the keep-alive probe's answer carries it, so the stall
// lasts at most one keep-alive period.
static void test_a_lost_window_update_recovers_at_the_keepalive(void) {
  rig_t r;
  static uint8_t buf[12000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, NULL);
  s_peer.win = 0;
  peer_ack(&s_peer);
  run_ms(&r, 5);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 100);
  s_peer.win = 65535;  // opened, and the update never reaches the client
  run_ms(&r, MG_TCPIP_KEEPALIVE_MS + 2000);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK(r.app.err[0] == '\0');
  rig_down(&r);
}

// A frame the driver refuses never left: nothing is counted as sent and it goes
// on the next poll.
static void test_a_refused_frame_is_tried_again_at_once(void) {
  rig_t r;
  uint8_t buf[1000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, NULL);
  s_peer.refuse = 3;
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 20);
  CHECK_EQ_INT(s_peer.frames_refused, 3);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK_EQ_INT(s_peer.resent_frames, 0);
  CHECK_EQ_INT(r.s->txq.len, 0);
  rig_down(&r);
}

// A resend the driver refuses is not counted as one, and goes on the next poll.
static void test_a_refused_resend_is_tried_again_at_once(void) {
  rig_t r;
  uint8_t buf[1000];
  uint64_t lost_at;
  pattern(buf, sizeof(buf));
  rig_up(&r, true, drop_first_frame);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 10);
  lost_at = s_now;
  CHECK_EQ_INT(s_peer.lost_frames, 1);
  run_ms(&r, MG_TCPIP_RTX_MS - 10);
  s_peer.refuse = 2;
  run_ms(&r, 20);
  CHECK_EQ_INT(s_peer.frames_refused, 2);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK(s_now - lost_at < MG_TCPIP_RTX_MS + 20);
  rig_down(&r);
}

// A partial ACK: the rest is what goes next, at the right sequence number.
static void test_a_partial_ack_resends_only_the_rest(void) {
  rig_t r;
  uint8_t buf[1000];
  uint32_t una;
  pattern(buf, sizeof(buf));
  rig_up(&r, true, peer_takes_without_acking);
  una = r.s->seq;
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 5);
  CHECK_EQ_INT(r.s->txq.len, sizeof(buf));
  s_peer.no_ack = false;
  peer_send(&s_peer, TH_ACK, s_peer.snd_nxt, una + 400, NULL, 0);
  run_ms(&r, 5);
  CHECK_EQ_INT(r.s->txq.len, 600);
  CHECK_EQ_U32(r.s->txq_una, una + 400);
  CHECK(r.s->txq.buf != NULL && memcmp(r.s->txq.buf, buf + 400, 600) == 0);
  // An ACK from before that one changes nothing.
  peer_send(&s_peer, TH_ACK, s_peer.snd_nxt, una + 100, NULL, 0);
  run_ms(&r, 5);
  CHECK_EQ_INT(r.s->txq.len, 600);
  // One for bytes never sent changes nothing either.
  peer_send(&s_peer, TH_ACK, s_peer.snd_nxt, r.s->seq + 5000, NULL, 0);
  run_ms(&r, 5);
  CHECK_EQ_INT(r.s->txq.len, 600);
  rig_down(&r);
}

// ── Tests: how a dead peer ends ─────────────────────────────────────────────

static void peer_dies_after_connect(rig_t *r) { (void) r; s_peer.dead = true; }

// Resends back off, cap at a minute, and the keep-alive ends it: no spinning,
// one error, everything freed (LeakSanitizer sees the rest).
static void test_a_dead_peer_ends_with_an_error(void) {
  rig_t r;
  uint8_t buf[500];
  uint64_t t0;
  pattern(buf, sizeof(buf));
  rig_up(&r, true, NULL);
  peer_dies_after_connect(&r);
  mg_send(r.c, buf, sizeof(buf));
  t0 = s_now;
  run_ms(&r, 30000);
  CHECK_EQ_INT(r.ifp.nrtx, 4);  // at 1, 3, 7 and 15 s
  CHECK_EQ_INT(r.s->txq_retries, 4);
  CHECK(r.app.err[0] == '\0');
  run_ms(&r, 100000);
  CHECK_EQ_INT(r.ifp.nrtx, 7);  // and 31, 63 and 123 s: 60 s apart from there
  while (!r.app.closed && s_now - t0 < 400000) run_ms(&r, 100);
  CHECK(r.app.closed);
  CHECK_STR(r.app.err, "keepalive");
  CHECK(s_now - t0 >= MG_TCPIP_KEEPALIVE_MS * 3);
  CHECK(s_now - t0 <= MG_TCPIP_KEEPALIVE_MS * 4 + 1000);
  CHECK_EQ_INT(r.ifp.nrtx, 7);
  rig_down(&r);
}

static void test_retransmit_counter_counts(void) {
  rig_t r;
  uint8_t buf[1000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, drop_first_frame);
  CHECK_EQ_INT(r.ifp.nrtx, 0);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 1200);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK_EQ_INT(r.ifp.nrtx, 1);
  rig_down(&r);
}

// ── Tests: closing ──────────────────────────────────────────────────────────

// Everything written before the close arrives, then the FIN, at the end of it.
static void test_close_sends_the_data_then_a_fin(void) {
  rig_t r;
  static uint8_t buf[30000];
  struct mg_connection *c;
  pattern(buf, sizeof(buf));
  rig_up(&r, true, NULL);
  c = r.c;
  mg_send(c, buf, sizeof(buf));
  net_close_conn(c);
  CHECK(c->is_draining);
  CHECK(!c->is_closing);
  run_ms(&r, 2000);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK_EQ_INT(s_peer.fins, 1);
  CHECK_EQ_U32(s_peer.fin_seq, s_peer.first_seq + sizeof(buf));
  CHECK(!conn_alive(&r, c));
  CHECK(!r.app.closed);  // detached: nothing reached the app
  rig_down(&r);
}

static void test_a_frame_lost_just_before_the_close_still_arrives(void) {
  rig_t r;
  uint8_t buf[800];
  struct mg_connection *c;
  pattern(buf, sizeof(buf));
  rig_up(&r, true, drop_first_frame);
  c = r.c;
  mg_send(c, buf, sizeof(buf));
  net_close_conn(c);
  run_ms(&r, 500);
  CHECK_EQ_INT(s_peer.fins, 0);  // the FIN waits for the data
  CHECK(conn_alive(&r, c));
  run_ms(&r, 1000);
  CHECK(peer_has_pattern(sizeof(buf)));
  CHECK_EQ_INT(s_peer.fins, 1);
  CHECK(!conn_alive(&r, c));
  rig_down(&r);
}

// A peer that acknowledges the FIN and keeps its side open: gone a second
// after the FIN, whatever the peer does next.
static void peer_keeps_its_side_open(peer_t *p) {
  p->fin_back = false;
  p->ack_only = true;
}

static void test_a_peer_that_stays_open_costs_one_second(void) {
  rig_t r;
  struct mg_connection *c;
  rig_up(&r, true, peer_keeps_its_side_open);
  c = r.c;
  net_close_conn(c);
  run_ms(&r, 5);
  CHECK_EQ_INT(s_peer.fins, 1);
  CHECK(conn_alive(&r, c));
  run_ms(&r, MG_TCPIP_FIN_MS + 5);
  CHECK(!conn_alive(&r, c));
  rig_down(&r);
}

static void test_a_dead_peer_lets_a_close_go_at_the_limit(void) {
  rig_t r;
  uint8_t buf[800];
  struct mg_connection *c;
  pattern(buf, sizeof(buf));
  rig_up(&r, true, NULL);
  c = r.c;
  peer_dies_after_connect(&r);
  mg_send(c, buf, sizeof(buf));
  net_close_conn(c);
  run_ms(&r, NET_CLOSE_LIMIT_MS - 100);
  CHECK(conn_alive(&r, c));
  if (conn_alive(&r, c)) CHECK(r.s->txq.len > 0);  // waiting for the ACK
  run_ms(&r, 200);
  CHECK(!conn_alive(&r, c));
  rig_down(&r);
}

// A peer that answers every probe but never opens its window: the keep-alive
// would hold it for ever, the limit does not.
static void test_a_shut_window_lets_a_close_go_at_the_limit(void) {
  rig_t r;
  uint8_t buf[3000];
  struct mg_connection *c;
  pattern(buf, sizeof(buf));
  rig_up(&r, true, NULL);
  c = r.c;
  s_peer.win = 0;
  peer_ack(&s_peer);
  run_ms(&r, 5);
  mg_send(c, buf, sizeof(buf));
  net_close_conn(c);
  run_ms(&r, NET_CLOSE_LIMIT_MS - 100);
  CHECK(conn_alive(&r, c));
  if (conn_alive(&r, c)) CHECK_EQ_INT(c->send.len, sizeof(buf));
  run_ms(&r, 200);
  CHECK(!conn_alive(&r, c));
  rig_down(&r);
}

// What arrives after the close is read by nobody: it must not pile up.
static void test_data_after_the_close_is_dropped(void) {
  rig_t r;
  struct mg_connection *c;
  uint8_t chunk[1000];
  int i;
  memset(chunk, 'x', sizeof(chunk));
  rig_up(&r, true, peer_keeps_its_side_open);
  c = r.c;
  c->is_full = 1;
  net_close_conn(c);
  CHECK(!c->is_full);
  run_ms(&r, 2);
  CHECK(r.c->recv.buf == NULL);
  for (i = 0; i < 100; i++) {
    peer_send(&s_peer, TH_PUSH | TH_ACK, s_peer.snd_nxt, s_peer.rcv_nxt, chunk,
              sizeof(chunk));
    s_peer.snd_nxt += sizeof(chunk);
    run_ms(&r, 2);
    if (!conn_alive(&r, c)) break;
    CHECK_EQ_INT(c->recv.len, 0);
  }
  rig_down(&r);
}

static void test_a_connecting_socket_closes_at_once(void) {
  rig_t r;
  struct mg_connection *c;
  rig_up(&r, true, NULL);
  s_peer.dead = true;
  c = mg_connect(&r.mgr, "tcp://192.168.1.10:5001", app_fn, NULL);
  CHECK(c != NULL);
  CHECK(c->is_connecting);
  net_close_conn(c);
  CHECK(c->is_closing);
  CHECK(!c->is_draining);
  run_ms(&r, 2);
  CHECK(!conn_alive(&r, c));
  rig_down(&r);
}

// With no link there is nobody to say goodbye to, and the stack's timers do
// not run: a closed socket goes at once, one closing before the link fell too.
static void test_no_link_means_no_waiting(void) {
  rig_t r;
  struct mg_connection *c, *c2;
  uint8_t buf[500];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, peer_keeps_its_side_open);
  c = r.c;
  c2 = mg_connect(&r.mgr, "tcp://192.168.1.10:5000", app_fn, NULL);
  CHECK(c2 != NULL);
  run_ms(&r, 20);
  s_peer.dead = true;
  mg_send(c, buf, sizeof(buf));
  net_close_conn(c);
  run_ms(&r, 2);
  CHECK(conn_alive(&r, c));
  r.ifp.state = MG_TCPIP_STATE_DOWN;
  net_close_drop_all(&r.mgr);
  CHECK(c->is_closing);
  net_close_conn(c2);
  CHECK(c2->is_closing);
  run_ms(&r, 2);
  CHECK(!conn_alive(&r, c));
  CHECK(!conn_alive(&r, c2));
  rig_down(&r);
}

// ── Tests: the peer ends it ─────────────────────────────────────────────────

static void test_a_reset_frees_the_queue(void) {
  rig_t r;
  uint8_t buf[3000];
  pattern(buf, sizeof(buf));
  rig_up(&r, true, peer_takes_without_acking);
  mg_send(r.c, buf, sizeof(buf));
  run_ms(&r, 10);
  CHECK(r.s->txq.len > 0);
  peer_send(&s_peer, TH_RST, s_peer.snd_nxt, 0, NULL, 0);
  run_ms(&r, 5);
  CHECK(r.app.closed);
  CHECK_STR(r.app.err, "peer RST");
  rig_down(&r);  // LeakSanitizer: the queue went with the connection
}

int main(void) {
  test_without_the_resend_a_lost_frame_stalls();
  test_a_lost_frame_is_resent_after_a_second();
  test_small_writes_survive_a_lost_frame();
  test_several_lost_frames_each_come_back();
  test_sequence_numbers_wrap();
  test_in_flight_and_queue_stay_capped();
  test_small_writes_do_not_inflate_the_queue();
  test_queue_goes_when_acknowledged();
  test_the_peers_window_is_honoured();
  test_a_zero_window_holds_then_resumes();
  test_a_lost_window_update_recovers_at_the_keepalive();
  test_a_refused_frame_is_tried_again_at_once();
  test_a_refused_resend_is_tried_again_at_once();
  test_a_partial_ack_resends_only_the_rest();
  test_a_dead_peer_ends_with_an_error();
  test_retransmit_counter_counts();
  test_close_sends_the_data_then_a_fin();
  test_a_frame_lost_just_before_the_close_still_arrives();
  test_a_peer_that_stays_open_costs_one_second();
  test_a_dead_peer_lets_a_close_go_at_the_limit();
  test_a_shut_window_lets_a_close_go_at_the_limit();
  test_data_after_the_close_is_dropped();
  test_a_connecting_socket_closes_at_once();
  test_no_link_means_no_waiting();
  test_a_reset_frees_the_queue();
  free(s_peer.rx);
  free(s_peer.have);
  return check_report("test_mg_tcp_resend");
}
