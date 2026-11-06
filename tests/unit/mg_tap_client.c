// A client on Mongoose's built-in TCP (the device's stack) with a TAP device for
// its Wi-Fi, so that the peer is this machine's real TCP. Driven by
// test_mg_linux_peer.py, which runs it in a network namespace of its own. Frames
// it is told to lose are lost after the "driver" took them, as on the device.
//
//   mg_tap_client <tap> <my ip> <peer ip> <port> <mode> <bytes> <drop every Nth
//                 data frame, 0 = none> <resend 0|1> [cert: TLS]
// mode: small  one byte per millisecond
//       big    one write at a time, holding off past 16 KB of unsent data
// It closes as the app would (net_close_conn) once it has written, prints what
// happened as one line of JSON and exits when the connection has ended.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mongoose.h"
#include "net_close.h"

#define BACKLOG (16u * 1024u)

static int s_tap = -1;
static unsigned s_drop_every, s_data_frames, s_lost;
static bool s_connected;
static char s_err[80];
static bool s_tls;

static bool is_tcp_data(const uint8_t *f, size_t len) {
  size_t ihl, off;
  if (len < 54 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 6) return false;
  ihl = (size_t) (f[14] & 0x0f) * 4;
  off = (size_t) (f[14 + ihl + 12] >> 4) * 4;
  return (size_t) ((f[16] << 8) | f[17]) > ihl + off;
}

static size_t tap_tx(const void *buf, size_t len, struct mg_tcpip_if *ifp) {
  (void) ifp;
  if (is_tcp_data((const uint8_t *) buf, len)) {
    s_data_frames++;
    if (s_drop_every && s_data_frames % s_drop_every == 0) {
      s_lost++;
      return len;
    }
  }
  return write(s_tap, buf, len) == (ssize_t) len ? len : 0;
}

static bool tap_poll(struct mg_tcpip_if *ifp, bool s1) {
  static uint8_t frame[2048];
  ssize_t n;
  (void) s1;
  while ((n = read(s_tap, frame, sizeof(frame))) > 0)
    mg_tcpip_qwrite(frame, (size_t) n, ifp);
  return true;
}

static void app_fn(struct mg_connection *c, int ev, void *ev_data) {
  if (c->fn_data == NULL) return;
  if (ev == MG_EV_CONNECT && !s_tls) s_connected = true;
  if (ev == MG_EV_TLS_HS) s_connected = true;
  if (ev == MG_EV_ERROR)
    snprintf(s_err, sizeof(s_err), "%s", (char *) ev_data);
}

#if MG_TLS == MG_TLS_MBED
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t) mg_millis(); }
void *umm_malloc(size_t n) { return malloc(n); }
void *umm_calloc(size_t n, size_t s) { return calloc(n, s); }
void umm_free(void *p) { free(p); }
#endif

int main(int argc, char **argv) {
  static struct mg_mgr mgr;
  static struct mg_tcpip_if ifp;
  static struct mg_tcpip_driver drv;
  static int token;
  struct mg_connection *c;
  struct ifreq ifr;
  char url[64];
  size_t want, written = 0;
  bool small, closed = false;
  uint64_t t_close = 0, t_end, t_start;
  if (argc < 9) return 2;
  want = strtoul(argv[6], NULL, 0);
  small = strcmp(argv[5], "small") == 0;
  s_drop_every = (unsigned) strtoul(argv[7], NULL, 0);
  s_tls = argc > 9;

  s_tap = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
  if (s_tap < 0) return 3;
  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", argv[1]);
  if (ioctl(s_tap, TUNSETIFF, &ifr) < 0) return 4;

  mg_log_set(MG_LL_NONE);
  mg_mgr_init(&mgr);
  drv.tx = tap_tx;
  drv.poll = tap_poll;
  ifp.driver = &drv;
  ifp.mac[0] = 2, ifp.mac[5] = 2;
  ifp.ip = inet_addr(argv[2]);
  ifp.mask = inet_addr("255.255.255.0");
  ifp.gw = inet_addr("10.9.0.254");
  ifp.recv_queue.size = 16384;
  ifp.enable_tcp_retransmit = argv[8][0] == '1';
  mg_tcpip_init(&mgr, &ifp);
  t_start = mg_millis();
  while (ifp.state != MG_TCPIP_STATE_READY && mg_millis() - t_start < 5000) {
    mg_mgr_poll(&mgr, 0);
    usleep(1000);
  }

  snprintf(url, sizeof(url), "%s://%s:%s", s_tls ? "tls" : "tcp", argv[3], argv[4]);
  c = mg_connect(&mgr, url, app_fn, &token);
  if (c == NULL) return 5;
#if MG_TLS == MG_TLS_MBED
  if (s_tls) {
    struct mg_tls_opts o;
    memset(&o, 0, sizeof(o));
    o.name = mg_str("localhost");
    mg_tls_init(c, &o);
  }
#endif

  t_end = mg_millis() + 60000;
  while (mgr.conns != NULL && mg_millis() < t_end) {
    mg_mgr_poll(&mgr, 0);
    if (s_connected && !closed && mgr.conns == c) {
      if (small && written < want) {
        uint8_t b = (uint8_t) (written * 31 + (written >> 8));
        mg_send(c, &b, 1);
        written++;
      }
      while (!small && written < want && c->send.len < BACKLOG) {
        uint8_t chunk[4096];
        size_t i, n = want - written < sizeof(chunk) ? want - written : sizeof(chunk);
        for (i = 0; i < n; i++)
          chunk[i] = (uint8_t) ((written + i) * 31 + ((written + i) >> 8));
        mg_send(c, chunk, n);
        written += n;
      }
      if (written >= want) {
        net_close_conn(c);
        closed = true;
        t_close = mg_millis();
      }
    }
    usleep(500);
  }
  printf("{\"connected\": %s, \"written\": %zu, \"ended\": %s, \"err\": \"%s\", "
         "\"lost\": %u, \"resent\": %u, \"close_ms\": %llu}\n",
         s_connected ? "true" : "false", written,
         mgr.conns == NULL ? "true" : "false", s_err, s_lost, (unsigned) ifp.nrtx,
         closed ? (unsigned long long) (mg_millis() - t_close) : 0ULL);
  mg_mgr_free(&mgr);
  return 0;
}
