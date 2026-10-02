#include "wifi.h"
#include "tcp.h"
#include "../dev_commands.h"
#include "../os/clock.h"
#include "../os/config.h"
#include "../os/system_menu.h"
#include "../os/toast.h"
#include "ca_bundle.h"
#include "rng.h"
#include "display.h"
#include "http.h"
#include "bt_pad.h"

#include "mongoose.h"
#ifndef PICODECK_SIM_FIRMWARE_NET
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#endif
#include "umm_malloc.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ── State
// ─────────────────────────────────────────────────────────────────────

static bool s_available = false;
static _Atomic wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static char s_ssid[64] = {0};
static char s_pass[64] = {0};
static char s_ip[20] = {0};
// True once a requested disconnect has left the network AND Core 1 has
// stopped talking to the CYW43 (see c1_disconnect): callers change sysclk
// after it, which must not race the chip's PIO SPI.
static _Atomic bool s_hw_disconnected = true;
// Core 1: set by a requested disconnect, cleared by the next connect.  While
// set, wifi_poll never calls mg_mgr_poll (which polls the CYW43 driver).
static bool s_radio_quiet = false;
// Mirror of s_ifp.state == MG_TCPIP_STATE_READY for Core 0: the interface
// state itself belongs to Core 1 (Mongoose and the CYW43 driver write it).
static _Atomic bool s_link_ready = false;
static bool s_http_required = false;
static volatile bool s_disconnect_pending = false; // deferred disconnect from SNTP callback
static bool s_auto_connected = false;    // true only for boot auto-connect

// Connect timeout / retry
static uint32_t s_connect_start_ms = 0;
static int      s_connect_retries  = 0;
#define WIFI_CONNECT_TIMEOUT_MS  15000  // 15 seconds per attempt
#define WIFI_MAX_RETRIES         3

// SNTP retry
static uint8_t  s_sntp_retries = 0;
static uint32_t s_sntp_next_retry_ms = 0;
#define SNTP_MAX_RETRIES    3
#define SNTP_RETRY_DELAY_MS 2000

static struct mg_mgr s_mgr;
static struct mg_tcpip_if s_ifp;
static struct mg_tcpip_driver_pico_w_data s_driver_data;

// Core 1 ticks every 1 ms because its audio pollers need that cadence (see
// core1_entry). Polling the CYW43 that often buys nothing while associated
// with no sockets open, so idle polls are spaced to this interval — still
// frequent enough for DHCP renewal, ARP replies and link-state changes.
#define WIFI_IDLE_POLL_MS 5
static uint32_t s_last_idle_poll_ms = 0;

// True when any Mongoose connection other than the DNS resolver's own UDP
// socket (which stays open after the first lookup) exists: an HTTP/TCP
// socket, an SNTP query or the connectivity check.
static bool mgr_has_user_conns(void) {
  for (struct mg_connection *c = s_mgr.conns; c != NULL; c = c->next) {
    if (c != s_mgr.dns4.c && c != s_mgr.dns6.c)
      return true;
  }
  return false;
}

// ── Core 0 → Core 1 request queue
// ────────────────────────────────────────

#define REQ_QUEUE_SIZE 8
#define REQ_QUEUE_MASK (REQ_QUEUE_SIZE - 1)

static conn_req_t      s_req_queue[REQ_QUEUE_SIZE];
static _Atomic uint8_t s_req_head = 0;  // written by Core 0 (producer)
static _Atomic uint8_t s_req_tail = 0;  // written by Core 1 (consumer)

// Separate spinlock for WiFi state (s_ip, s_ssid) — NOT for the queue
static spin_lock_t *s_state_lock;

bool wifi_req_push(const conn_req_t *req) {
  uint8_t head = atomic_load_explicit(&s_req_head, memory_order_relaxed);
  uint8_t next = (head + 1) & REQ_QUEUE_MASK;
  uint8_t tail = atomic_load_explicit(&s_req_tail, memory_order_acquire);

  if (next == tail) {
    printf("WiFi: request queue full, dropping request type=%d\n", (int)req->type);
    return false;
  }

  s_req_queue[head] = *req;
  atomic_store_explicit(&s_req_head, next, memory_order_release);

  // Ring doorbell to wake Core 1 immediately (<1us) instead of waiting
  // for the 5ms polling timer.  Core 1's doorbell ISR sets the tick flag.
  multicore_doorbell_set_other_core(WIFI_IPC_DOORBELL);

  return true;
}

// ── Internet connectivity check
// ──────────────────────────────────────────────────────────────────────
// After WiFi associates and gets an IP, we verify actual internet
// reachability with a lightweight TCP connect to 8.8.8.8:53 (Google DNS).
// No DNS dependency, no TLS, minimal overhead.

// Where the SNTP query and the connectivity check go. Overridable only so the
// simulator's SIM_FIRMWARE_NET build can keep them on loopback
// (simulator/net/include/mongoose.h); the firmware never defines them.
#ifndef WIFI_SNTP_URL
#define WIFI_SNTP_URL "udp://pool.ntp.org:123"
#endif
#ifndef WIFI_CHECK_URL
#define WIFI_CHECK_URL "tcp://8.8.8.8:53"
#endif

static _Atomic bool     s_internet_ok = false;
static uint32_t s_connectivity_check_ms = 0;
static struct mg_connection *s_check_conn = NULL;

#define CONNECTIVITY_CHECK_INTERVAL_MS  60000   // recheck every 60s
#define CONNECTIVITY_CHECK_RETRY_MS     10000   // retry faster on failure
#define CONNECTIVITY_CHECK_TIMEOUT_MS   10000   // give up if check hangs

static uint32_t s_connectivity_deadline = 0;

static void connectivity_cb(struct mg_connection *c, int ev, void *ev_data) {
  (void)ev_data;
  if (ev == MG_EV_CONNECT) {
    printf("WiFi: connectivity check OK\n");
    s_internet_ok = true;
    s_check_conn = NULL;
    s_connectivity_deadline = 0;
    c->is_closing = 1;
    s_connectivity_check_ms =
        to_ms_since_boot(get_absolute_time()) + CONNECTIVITY_CHECK_INTERVAL_MS;
  } else if (ev == MG_EV_ERROR) {
    printf("WiFi: connectivity check failed: %s\n", (char *)ev_data);
    s_internet_ok = false;
    s_check_conn = NULL;
    s_connectivity_deadline = 0;
    c->is_closing = 1;
    toast_push("No internet connection", TOAST_STYLE_WARNING);
    s_connectivity_check_ms =
        to_ms_since_boot(get_absolute_time()) + CONNECTIVITY_CHECK_RETRY_MS;
  } else if (ev == MG_EV_CLOSE) {
    s_check_conn = NULL;
  }
}

static void start_connectivity_check(void) {
  if (s_check_conn) return; // already in progress
  s_check_conn = mg_connect(&s_mgr, WIFI_CHECK_URL, connectivity_cb, NULL);
  if (!s_check_conn) {
    s_internet_ok = false;
    s_connectivity_check_ms =
        to_ms_since_boot(get_absolute_time()) + CONNECTIVITY_CHECK_RETRY_MS;
  } else {
    s_connectivity_deadline =
        to_ms_since_boot(get_absolute_time()) + CONNECTIVITY_CHECK_TIMEOUT_MS;
  }
}

// ── SNTP
// ──────────────────────────────────────────────────────────────────────

static void sntp_cb(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_SNTP_TIME) {
    int64_t *t = (int64_t *)ev_data;
    printf("WiFi: SNTP sync OK, time: %lld\n", *t);
    clock_sntp_set((unsigned)(*t / 1000));
    s_sntp_retries = 0;
    c->is_closing = 1;
    if (!s_http_required && s_auto_connected &&
        system_menu_get_wifi_auto_disconnect()) {
      // Don't call mg_wifi_disconnect() directly here — we're inside
      // mg_mgr_poll() and calling it from within a Mongoose callback causes
      // reentrancy into the CYW43 driver. Set a flag; wifi_poll() will
      // perform the disconnect safely after mg_mgr_poll() returns.
      s_disconnect_pending = true;
    }
  } else if (ev == MG_EV_ERROR) {
    printf("WiFi: SNTP error: %s\n", (char *)ev_data);
    c->is_closing = 1;
    if (s_sntp_retries < SNTP_MAX_RETRIES) {
      s_sntp_retries++;
      s_sntp_next_retry_ms = to_ms_since_boot(get_absolute_time()) + SNTP_RETRY_DELAY_MS;
      printf("WiFi: SNTP retry %d/%d in %dms\n", s_sntp_retries, SNTP_MAX_RETRIES,
             SNTP_RETRY_DELAY_MS);
    } else {
      printf("WiFi: SNTP failed after %d retries\n", SNTP_MAX_RETRIES);
      toast_push("Time sync failed", TOAST_STYLE_ERROR);
    }
  } else if (ev == MG_EV_CLOSE) {
    // SNTP closed
  }
}

static void start_sntp(void) {
  printf("WiFi: Starting SNTP sync...\n");
  mg_sntp_connect(&s_mgr, WIFI_SNTP_URL, sntp_cb, NULL);
}

// ── Internal helpers
// ──────────────────────────────────────────────────────────

static void tcpip_cb(struct mg_tcpip_if *ifp, int ev, void *ev_data) {
  if (ev == MG_TCPIP_EV_ST_CHG) {
    uint8_t state = *(uint8_t *)ev_data;
    atomic_store_explicit(&s_link_ready, state == MG_TCPIP_STATE_READY,
                          memory_order_release);
    if (state == MG_TCPIP_STATE_READY) {
      s_status = WIFI_STATUS_CONNECTED;
      s_connect_start_ms = 0;
      s_connect_retries = 0;
      s_internet_ok = false;
      // Schedule connectivity check shortly after IP assignment
      s_connectivity_check_ms = to_ms_since_boot(get_absolute_time()) + 500;
      { uint32_t save = spin_lock_blocking(s_state_lock);
        mg_snprintf(s_ip, sizeof(s_ip), "%M", mg_print_ip, &ifp->ip);
        spin_unlock(s_state_lock, save);
      }
      printf("WiFi: connected  IP=%s\n", s_ip);
      start_sntp();
    } else if (state == MG_TCPIP_STATE_DOWN) {
      // Nothing in flight can complete now: fail it instead of letting it
      // wait for its timeout (or forever, before timeouts always ran).
      http_c1_fail_all("network down");
      tcp_c1_fail_all("network down");
      if (s_status == WIFI_STATUS_CONNECTED) {
        s_status = WIFI_STATUS_DISCONNECTED;
        { uint32_t save = spin_lock_blocking(s_state_lock);
          s_ip[0] = '\0';
          spin_unlock(s_state_lock, save);
        }
        s_internet_ok = false;
        s_connectivity_check_ms = 0;
        printf("WiFi: disconnected\n");
        toast_push("WiFi disconnected", TOAST_STYLE_WARNING);
      }
    }
  } else if (ev == MG_TCPIP_EV_WIFI_CONNECT_ERR) {
    s_status = WIFI_STATUS_FAILED;
    s_connect_start_ms = 0;
    printf("WiFi: connect failed (err=%d)\n", *(int *)ev_data);
  }
}

// A requested disconnect (Core 1).  mg_wifi_disconnect() is synchronous
// (cyw43_arch_disable_sta_mode: the disassociate ioctl has completed when it
// returns).  With pico_cyw43_arch_none the driver only runs when polled, and
// the link-down event itself would only arrive through another mg_mgr_poll —
// exactly the chip traffic a following clock change must not race.  So after
// the leave Core 1 stops polling Mongoose (s_radio_quiet) and takes the
// interface down itself, as mg_ip_link would on the next 1 s link check:
// tcpip_cb then marks the link down and fails what was in flight.  Only
// then is the radio reported idle.  The next CONN_REQ_WIFI_CONNECT resumes
// polling, and the interface comes back up (DHCP → READY) from DOWN, so a
// quick disconnect/reconnect cannot leave Mongoose on the stale READY.
static void c1_disconnect(void) {
  atomic_store_explicit(&s_hw_disconnected, false, memory_order_release);
  mg_wifi_disconnect();
  s_radio_quiet = true;
  if (s_ifp.state != MG_TCPIP_STATE_DOWN) {
    s_ifp.state = MG_TCPIP_STATE_DOWN;
    tcpip_cb(&s_ifp, MG_TCPIP_EV_ST_CHG, &s_ifp.state);
  }
  atomic_store_explicit(&s_hw_disconnected, true, memory_order_release);
}

// ── TLS
// ──────────────────────────────────────────────────────────────────────

// A verifying handshake needs the wall clock (certificate validity dates).
// Returns false — and, if SNTP has given up, starts another sync so a retry
// can succeed — when the clock has not been set yet.
static bool tls_clock_ready(void) {
  if (clock_is_set()) return true;
  wifi_status_t st = s_status;
  if (s_sntp_next_retry_ms == 0 && s_sntp_retries >= SNTP_MAX_RETRIES &&
      (st == WIFI_STATUS_CONNECTED || st == WIFI_STATUS_ONLINE)) {
    s_sntp_retries = 0;
    start_sntp();
  }
  return false;
}

#ifndef PICODECK_SIM_FIRMWARE_NET
// (Guard: the simulator's SIM_FIRMWARE_NET build runs this file on Mongoose
// MG_ARCH_UNIX with TLS compiled out, so there is no mbedTLS here; the #else
// branch below refuses TLS instead.)

// The root bundle, parsed ONCE (wifi_tls_init, at boot) into one chain that
// every connection shares read-only, instead of Mongoose re-parsing ~15 KB
// of PEM into PSRAM on Core 1 for each connection.  NULL if parsing failed:
// verifying connections are then refused.
static mbedtls_x509_crt *s_ca_chain = NULL;

bool wifi_tls_init(void) {
  if (s_ca_chain) return true;
  mbedtls_x509_crt *crt = (mbedtls_x509_crt *)umm_calloc(1, sizeof(*crt));
  if (!crt) return false;
  mbedtls_x509_crt_init(crt);
  // PEM: the length includes the terminating NUL.
  int rc = mbedtls_x509_crt_parse(crt, (const unsigned char *)g_ca_bundle_pem,
                                  g_ca_bundle_pem_len + 1);
  if (rc != 0) {  // > 0: that many roots failed to parse
    printf("[TLS] CA bundle parse failed (%d) — verified TLS disabled\n", rc);
    mbedtls_x509_crt_free(crt);
    umm_free(crt);
    return false;
  }
  int n = 0;
  for (mbedtls_x509_crt *c = crt; c && c->raw.len; c = c->next) n++;
  printf("[TLS] CA bundle: %d roots parsed\n", n);
  s_ca_chain = crt;
  return true;
}

// Start TLS on nc: SNI + host-name check, and the shared root chain unless
// the connection opted out.  mg_tls_init gets no opts.ca, so Mongoose sets
// VERIFY_NONE and loads nothing; we then attach the pre-parsed chain and
// require verification (safe: the handshake has not started yet, and mbedTLS
// reads ca_chain/authmode from the config at certificate-verify time).
static bool tls_start(struct mg_connection *nc, const char *host,
                      bool insecure) {
  // mbedTLS draws every nonce and ephemeral key from mg_random: never run a
  // handshake without this core's TRNG-seeded DRBG (rng.c).
  if (!rng_ready()) {
    printf("[TLS] %s: refused, no seeded RNG on Core 1\n", host);
    return false;
  }
  if (!insecure && !s_ca_chain) {
    printf("[TLS] %s: refused, no CA bundle\n", host);
    return false;
  }
  struct mg_tls_opts opts = {0};
  opts.name = mg_str(host);
  if (insecure)
    printf("[TLS] %s: certificate verification DISABLED (setInsecure)\n",
           host);
  mg_tls_init(nc, &opts);
  if (!nc->is_tls_hs || !nc->tls) return false;
  // Fail closed on the handshake's randomness too: Mongoose's mg_mbed_rng
  // ignores mg_random's failure and hands mbedTLS the zeroed buffer.
  // rng_mbedtls_random returns an mbedTLS error instead, so a DRBG that fails
  // after the rng_ready() check aborts the handshake rather than using zeros.
  mbedtls_ssl_conf_rng(&((struct mg_tls *)nc->tls)->conf, rng_mbedtls_random,
                       NULL);
  if (!insecure) {
    struct mg_tls *tls = (struct mg_tls *)nc->tls;
    mbedtls_ssl_conf_ca_chain(&tls->conf, s_ca_chain, NULL);
    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  }
  return true;
}

bool wifi_tls_verify_error(struct mg_connection *nc, char *out, size_t n) {
  if (!nc || !nc->tls) return false;
  struct mg_tls *tls = (struct mg_tls *)nc->tls;
  uint32_t flags = mbedtls_ssl_get_verify_result(&tls->ssl);
  if (flags == 0 || flags == (uint32_t)-1) return false;
  const char *why;
  if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
    why = "certificate not trusted (its root CA is not in the PicoDeck bundle)";
  else if (flags & MBEDTLS_X509_BADCERT_EXPIRED)
    why = "certificate expired";
  else if (flags & MBEDTLS_X509_BADCERT_FUTURE)
    why = "certificate not yet valid (is the clock right?)";
  else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
    why = "certificate does not match the host name";
  else if (flags & MBEDTLS_X509_BADCERT_REVOKED)
    why = "certificate revoked";
  else
    why = "certificate verification failed";
  snprintf(out, n, "TLS: %s (flags 0x%lx)", why, (unsigned long)flags);
  return true;
}

#else  // PICODECK_SIM_FIRMWARE_NET: TLS compiled out (simulator/net)

bool wifi_tls_init(void) { return false; }

static bool tls_start(struct mg_connection *nc, const char *host,
                      bool insecure) {
  (void)nc;
  (void)insecure;
  printf("[TLS] %s: refused, TLS is not built into the simulator\n", host);
  return false;
}

bool wifi_tls_verify_error(struct mg_connection *nc, char *out, size_t n) {
  (void)nc;
  (void)out;
  (void)n;
  return false;
}

#endif  // PICODECK_SIM_FIRMWARE_NET

// ── Core 1 request drainer
// ────────────────────────────────────────────────

// Called by wifi_poll() on Core 1 before mg_mgr_poll().
// Executes queued mg_* operations that Core 0 cannot safely call directly.
static void drain_requests(void) {
  for (;;) {
    uint8_t tail = atomic_load_explicit(&s_req_tail, memory_order_relaxed);
    uint8_t head = atomic_load_explicit(&s_req_head, memory_order_acquire);

    if (head == tail)
      break;

    conn_req_t req = s_req_queue[tail];
    atomic_store_explicit(&s_req_tail, (tail + 1) & REQ_QUEUE_MASK,
                          memory_order_release);

    switch (req.type) {

      case CONN_REQ_HTTP_START: {
        http_conn_t *c = req.conn;
        if (!c) break;

        struct mg_connection *old = (struct mg_connection *)c->pcb;
        if (c->req_keep_alive && old && !old->is_closing && !old->is_draining) {
          // Reuse the kept-alive connection.  The response is parsed by
          // http_ev_fn itself (no Mongoose HTTP handler to re-attach).
          if (!http_c1_begin(c, HTTP_STATE_SENDING)) break;
          mg_iobuf_del(&old->recv, 0, old->recv.len);  // stray bytes
          printf("[HTTP] Reusing connection for %s %s\n", c->method, c->path);
          http_c1_send_request(old, c);
          break;
        }

        // Released (or failed) before Core 1 got here: nothing to start.
        // Otherwise leave QUEUED *before* any mg_* call.
        if (!http_c1_begin(c, HTTP_STATE_CONNECTING)) break;
        http_c1_detach(c);  // the previous request's connection, if open

        char url[320];
        snprintf(url, sizeof(url), "%s://%s:%u",
                 c->use_ssl ? "https" : "http", c->server, c->port);
        printf("[HTTP] Connecting to %s (SSL=%d)...\n", url, c->use_ssl);

        if (c->use_ssl && !c->insecure && !tls_clock_ready()) {
          printf("[HTTP] Refusing TLS to %s: clock not set\n", c->server);
          http_c1_fail(c, WIFI_TLS_ERR_CLOCK);
          break;
        }

        // Plain mg_connect: http_ev_fn parses the response itself.
        struct mg_connection *nc = mg_connect(&s_mgr, url, http_ev_fn, c);
        if (!nc) {
          http_c1_fail(c, "mg_connect failed");
          break;
        }

        if (c->use_ssl && !tls_start(nc, c->server, c->insecure)) {
          printf("[HTTP] TLS init failed\n");
          nc->fn_data = NULL;  // keep MG_EV_CLOSE off the slot
          mg_close_conn(nc);
          http_c1_fail(c, "TLS init failed");
          break;
        }

        c->pcb = (void *)nc;
        break;
      }

      case CONN_REQ_HTTP_CLOSE:
        // The close handler: fn_data cleared, connection closed, then the
        // slot is RELEASED for Core 0 to reclaim (http.h).
        if (req.conn) http_c1_release(req.conn);
        break;

      case CONN_REQ_TCP_CONNECT: {
        tcp_conn_t *tc = (tcp_conn_t *)req.conn;
        // Released (or failed) before Core 1 got here: nothing to connect.
        if (!tc || !tcp_c1_begin_connect(tc)) break;
        char url[320];
        snprintf(url, sizeof(url), "%s://%s:%u",
                 tc->use_ssl ? "tls" : "tcp", tc->host, tc->port);
        printf("[TCP] Connecting to %s (SSL=%d)...\n", url, tc->use_ssl);
        if (tc->use_ssl && !tc->insecure && !tls_clock_ready()) {
          printf("[TCP] Refusing TLS to %s: clock not set\n", tc->host);
          tcp_c1_fail(tc, WIFI_TLS_ERR_CLOCK);
          break;
        }
        struct mg_connection *nc = mg_connect(&s_mgr, url, tcp_ev_fn, tc);
        if (!nc) {
          tcp_c1_fail(tc, "mg_connect failed");
          break;
        }
        // A tls:// URL only sets nc->is_tls; without mg_tls_init no
        // handshake runs. Same init as the HTTPS path above.
        if (tc->use_ssl && !tls_start(nc, tc->host, tc->insecure)) {
          nc->fn_data = NULL;  // keep MG_EV_CLOSE from marking it CLOSED
          mg_close_conn(nc);
          tcp_c1_fail(tc, "TLS init failed");
          break;
        }
        tc->pcb = (void *)nc;
        break;
      }

      case CONN_REQ_TCP_WRITE: {
        tcp_conn_t *tc = (tcp_conn_t *)req.conn;
        if (tc && req.data)
          tcp_c1_send(tc, req.data, req.data_len);
        if (req.data) umm_free(req.data);
        break;
      }

      case CONN_REQ_TCP_CLOSE:
        // The close handler: fn_data cleared, connection closed, then the
        // slot is RELEASED for Core 0 to reclaim (tcp.h).
        if (req.conn) tcp_c1_release((tcp_conn_t *)req.conn);
        break;

      case CONN_REQ_WIFI_CONNECT:
        s_radio_quiet = false;  // polling (and the CYW43 driver) resume
        s_driver_data.wifi.ssid = s_ssid;
        s_driver_data.wifi.pass = s_pass;
        mg_wifi_connect(&s_driver_data.wifi);
        break;

      case CONN_REQ_WIFI_DISCONNECT:
        c1_disconnect();
        break;
    }
  }
}

// ── Public API
// ─────────────────────────────────────────────────────────────────

// Mongoose log sink.  The vendored mongoose defaults to MG_LL_DEBUG, so
// Core 1 floods the shared serial console with ARP/IP-proto chatter; a log
// line landing mid-payload corrupts dev-command bulk transfers (b64
// screenshots and file transfers).  Runs on Core 1.
static void wifi_mg_log_sink(char c, void *param) {
  (void)param;
  if (dev_commands_transfer_active()) return;
  putchar(c);
}

void wifi_init(void) {
  // Quieten mongoose (default level is DEBUG — see wifi_mg_log_sink) and
  // route what remains through the transfer-aware sink.  Must happen before
  // Core 1 starts polling the event manager.
  mg_log_set(MG_LL_ERROR);
  mg_log_set_fn(wifi_mg_log_sink, NULL);

  // Claim a hardware spinlock for WiFi state protection (s_ip, s_ssid)
  int lock_num = spin_lock_claim_unused(true);
  s_state_lock = spin_lock_instance(lock_num);
  atomic_store(&s_req_head, 0);
  atomic_store(&s_req_tail, 0);

  // Claim doorbell for IPC wake-up (Core 0 rings, Core 1 handles)
  multicore_doorbell_claim(WIFI_IPC_DOORBELL, 0x3);  // both cores

  mg_mgr_init(&s_mgr);

  memset(&s_ifp, 0, sizeof(s_ifp));
  memset(&s_driver_data, 0, sizeof(s_driver_data));

  s_ifp.driver = &mg_tcpip_driver_pico_w;
  s_ifp.driver_data = &s_driver_data;
  s_ifp.pfn = tcpip_cb;
  s_ifp.recv_queue.size = 16384;

  mg_tcpip_init(&s_mgr, &s_ifp);
  s_ifp.pfn = tcpip_cb; // Ensure our callback is set

  if (s_mgr.ifp == NULL) {
    printf("WiFi: driver init failed — CYW43 not available\n");
    s_available = false;
    return;
  }

  s_available = true;
  printf("WiFi: Mongoose TCPIP ready\n");

  // Auto-connect if credentials are stored in /system/config.json.
  const char *ssid = config_get("wifi_ssid");
  const char *pass = config_get("wifi_pass");
  if (ssid && ssid[0]) {
    printf("WiFi: auto-connecting to '%s'\n", ssid);
    wifi_connect(ssid, pass ? pass : "");
    s_auto_connected = true; // mark as boot auto-connect (after wifi_connect clears it)
  }
}

bool wifi_is_available(void) { return s_available; }

void wifi_connect(const char *ssid, const char *password) {
  wifi_resume_radio();  // a connect needs the driver polling again
  if (!s_available || !ssid || !ssid[0])
    return;

  { uint32_t save = spin_lock_blocking(s_state_lock);
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';

    if (password) {
      strncpy(s_pass, password, sizeof(s_pass) - 1);
      s_pass[sizeof(s_pass) - 1] = '\0';
    } else {
      s_pass[0] = '\0';
    }

    s_ip[0] = '\0';
    spin_unlock(s_state_lock, save);
  }
  s_status = WIFI_STATUS_CONNECTING;
  s_connect_start_ms = to_ms_since_boot(get_absolute_time());
  s_connect_retries = 0;
  s_auto_connected = false; // user/app-initiated, don't auto-disconnect after SNTP

  printf("WiFi: connecting to '%s'...\n", s_ssid);

  // Queue the mg_wifi_connect() for Core 1 to execute
  conn_req_t req = {.type = CONN_REQ_WIFI_CONNECT};
  wifi_req_push(&req);
}

void wifi_disconnect(void) {
  if (!s_available)
    return;

  // Update status immediately so callers see DISCONNECTED right away
  s_status = WIFI_STATUS_DISCONNECTED;
  atomic_store_explicit(&s_hw_disconnected, false, memory_order_release);
  s_ssid[0] = '\0';
  s_ip[0] = '\0';
  printf("WiFi: disconnect queued\n");

  // Queue the mg_wifi_disconnect() for Core 1 to execute
  conn_req_t req = {.type = CONN_REQ_WIFI_DISCONNECT};
  wifi_req_push(&req);
}

wifi_status_t wifi_get_status(void) {
  if (atomic_load_explicit(&s_link_ready, memory_order_acquire))
    return s_internet_ok ? WIFI_STATUS_ONLINE : WIFI_STATUS_CONNECTED;
  return s_status;
}

bool wifi_has_internet(void) {
  return s_internet_ok &&
         atomic_load_explicit(&s_link_ready, memory_order_acquire);
}

bool wifi_hw_disconnected(void) {
  return atomic_load_explicit(&s_hw_disconnected, memory_order_acquire);
}

#ifndef PICODECK_SIM_FIRMWARE_NET
// cyw43_poll while the radio is paused (NULL otherwise).
static void (*s_paused_poll)(void);

// Both run under the CYW43 async context's lock, on its core (Core 0, where
// wifi_init ran cyw43_arch_init).
static uint32_t radio_pause(void *param) {
  (void)param;
  if (!cyw43_poll)
    return 0;  // paused already
  cyw43_ll_bus_sleep(&cyw43_state.cyw43_ll, true);
  cyw43_sleep = 0;  // nothing left for the driver's sleep countdown
  // With no poll function the context's worker does nothing: the host-wake
  // interrupt fires at most once (its handler disables it, and only a poll
  // enables it again) and the sleep checks stop.
  s_paused_poll = cyw43_poll;
  cyw43_poll = NULL;
  return 0;
}

static uint32_t radio_resume(void *param) {
  (void)param;
  if (!s_paused_poll)
    return 0;
  cyw43_poll = s_paused_poll;
  s_paused_poll = NULL;
  // Whatever the chip raised meanwhile, and the host-wake interrupt again.
  cyw43_schedule_internal_poll_dispatch(cyw43_poll);
  return 0;
}
#endif

void wifi_pause_radio(void) {
#ifndef PICODECK_SIM_FIRMWARE_NET
  // Only with Core 1 off the driver. wifi_hw_disconnected() alone does not
  // say so: it stays true from boot, and from the last disconnect, through
  // a connect (wifi_connect never clears it). Core 1 polls the CYW43 only
  // while connecting or connected, so those states rule a pause out: a
  // paused driver reads as powered off (cyw43_poll NULL), and the next
  // call into it from Core 1 would power-cycle the chip and reload its
  // firmware.
  // Nor while Bluetooth is on: it polls the chip through the same driver.
  wifi_status_t st = wifi_get_status();
  if (!s_available || !wifi_hw_disconnected() || bt_pad_radio_in_use() ||
      st == WIFI_STATUS_CONNECTED || st == WIFI_STATUS_CONNECTING ||
      st == WIFI_STATUS_ONLINE)
    return;
  async_context_execute_sync(cyw43_arch_async_context(), radio_pause, NULL);
#endif
}

void wifi_resume_radio(void) {
#ifndef PICODECK_SIM_FIRMWARE_NET
  // s_paused_poll changes only in radio_pause/radio_resume, which run here
  // on Core 0: read unlocked, it spares every wifi_connect the context's
  // lock, which Core 1 holds through each of its calls into the driver.
  if (!s_available || !s_paused_poll)
    return;
  async_context_execute_sync(cyw43_arch_async_context(), radio_resume, NULL);
#endif
}

#ifndef PICODECK_SIM_FIRMWARE_NET
// The state machine driving the CYW43's SPI: in the PIO whose function GPIO
// CYW43_PIN_WL_CLOCK has, side-setting that pin (cyw43_bus_pio_spi.c keeps
// its PIO/SM in a private struct). -1 when not found.
static int cyw43_bus_sm(PIO *pio_out) {
  static const uint8_t k_func[] = {GPIO_FUNC_PIO0, GPIO_FUNC_PIO1,
                                   GPIO_FUNC_PIO2};
  uint func = gpio_get_function(CYW43_PIN_WL_CLOCK);
  for (uint i = 0; i < NUM_PIOS && i < count_of(k_func); i++) {
    if (func != k_func[i])
      continue;
    PIO pio = pio_get_instance(i);
    uint pin = CYW43_PIN_WL_CLOCK - pio_get_gpio_base(pio);
    for (int sm = 0; sm < 4; sm++) {
      uint32_t ctrl = pio->sm[sm].pinctrl;
      if (((ctrl & PIO_SM0_PINCTRL_SIDESET_BASE_BITS) >>
           PIO_SM0_PINCTRL_SIDESET_BASE_LSB) == pin &&
          ((ctrl & PIO_SM0_PINCTRL_SIDESET_COUNT_BITS) >>
           PIO_SM0_PINCTRL_SIDESET_COUNT_LSB) == 1 &&
          pio_sm_is_claimed(pio, (uint)sm)) {
        *pio_out = pio;
        return sm;
      }
    }
  }
  return -1;
}

static uint32_t bus_clock(void *arg) {
  uint32_t khz = *(const uint32_t *)arg;
  PIO pio;
  int sm = cyw43_bus_sm(&pio);
  if (sm < 0)
    return 1;
  uint32_t div = (khz + 99999u) / 100000u;
  if (div < 2)
    div = 2;
  pio_sm_set_clkdiv_int_frac8(pio, (uint)sm, div, 0);
  return 0;
}
#endif

void wifi_bus_hold(bool hold) {
#ifndef PICODECK_SIM_FIRMWARE_NET
  async_context_t *ctx = s_available ? cyw43_arch_async_context() : NULL;
  if (!ctx)
    return;
  if (hold)
    async_context_acquire_lock_blocking(ctx);
  else
    async_context_release_lock(ctx);
#else
  (void)hold;
#endif
}

void wifi_bus_clock(uint32_t khz) {
#ifndef PICODECK_SIM_FIRMWARE_NET
  if (!s_available)
    return;
  if (async_context_execute_sync(cyw43_arch_async_context(), bus_clock,
                                 &khz) != 0)
    printf("WiFi: CYW43 bus state machine not found\n");
#else
  (void)khz;
#endif
}

const char *wifi_get_ip(void) {
  wifi_status_t st = wifi_get_status();
  if (st != WIFI_STATUS_CONNECTED && st != WIFI_STATUS_ONLINE)
    return NULL;
  // Copy under lock to prevent torn reads from Core 1 writes
  static char s_ip_copy[20];
  uint32_t save = spin_lock_blocking(s_state_lock);
  mg_snprintf(s_ip, sizeof(s_ip), "%M", mg_print_ip, &s_ifp.ip);
  memcpy(s_ip_copy, s_ip, sizeof(s_ip_copy));
  spin_unlock(s_state_lock, save);
  return s_ip_copy[0] ? s_ip_copy : NULL;
}

const char *wifi_get_ssid(void) {
  static char s_ssid_copy[64];
  uint32_t save = spin_lock_blocking(s_state_lock);
  memcpy(s_ssid_copy, s_ssid, sizeof(s_ssid_copy));
  spin_unlock(s_state_lock, save);
  return s_ssid_copy[0] ? s_ssid_copy : NULL;
}

void wifi_set_http_required(bool required) { s_http_required = required; }
bool wifi_get_http_required(void) { return s_http_required; }

void wifi_poll(void) {
  if (!s_available)
    return;

  // Only Core 1 owns the Mongoose manager. All other callers are legacy
  // call sites from before Core 1 took ownership; they are now safe no-ops
  // since Core 1's core1_entry() drives the stack on its 1 ms tick.
  if (get_core_num() != 1)
    return;

  // Top up the pre-generated random pool while the stack is shallow: TLS
  // draws from it deep inside mg_mgr_poll (rng.h).
  rng_refill();

  // Always drain requests — connect requests must be processed even when
  // disconnected/failed, otherwise queued WIFI_CONNECT never executes.
  drain_requests();

  // Skip Mongoose polling when disconnected to save power, and never after
  // a requested disconnect until the next connect (c1_disconnect).
  wifi_status_t st = wifi_get_status();
  bool link = !s_radio_quiet &&
              (st == WIFI_STATUS_CONNECTED || st == WIFI_STATUS_CONNECTING ||
               st == WIFI_STATUS_ONLINE);
  bool polled = false;
  if (link) {
    // Associated but idle (no sockets, not mid-connect): poll at
    // WIFI_IDLE_POLL_MS instead of every tick.
    bool idle = st != WIFI_STATUS_CONNECTING && !mgr_has_user_conns();
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (!idle || now_ms - s_last_idle_poll_ms >= WIFI_IDLE_POLL_MS) {
      if (idle) s_last_idle_poll_ms = now_ms;
      mg_mgr_poll(&s_mgr, 0);
      polled = true;
    }
  }

  // Enforce HTTP and TCP connection/read timeouts on every tick, link or
  // not: a request in flight must never hang.
  http_check_timeouts();
  tcp_check_timeouts();

  if (!link || !polled)
    return;

  // Connect timeout: if stuck in CONNECTING, retry or give up
  if (s_status == WIFI_STATUS_CONNECTING && s_connect_start_ms > 0) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - s_connect_start_ms > WIFI_CONNECT_TIMEOUT_MS) {
      if (s_connect_retries < WIFI_MAX_RETRIES) {
        s_connect_retries++;
        printf("WiFi: connect timeout, retry %d/%d\n",
               s_connect_retries, WIFI_MAX_RETRIES);
        mg_wifi_disconnect();
        s_driver_data.wifi.ssid = s_ssid;
        s_driver_data.wifi.pass = s_pass;
        mg_wifi_connect(&s_driver_data.wifi);
        s_connect_start_ms = now_ms;
      } else {
        printf("WiFi: connect failed after %d retries\n", WIFI_MAX_RETRIES);
        mg_wifi_disconnect();
        s_status = WIFI_STATUS_FAILED;
        toast_push("WiFi connection failed", TOAST_STYLE_WARNING);
        s_connect_start_ms = 0;
        s_connect_retries = 0;
      }
    }
  }

  // Internet connectivity check timer
  if (s_connectivity_check_ms > 0 &&
      (st == WIFI_STATUS_CONNECTED || st == WIFI_STATUS_ONLINE)) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms >= s_connectivity_check_ms) {
      s_connectivity_check_ms = 0;
      start_connectivity_check();
    }
  }

  // Connectivity check timeout — force-close if hanging
  if (s_connectivity_deadline > 0) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms >= s_connectivity_deadline) {
      printf("WiFi: connectivity check timeout\n");
      if (s_check_conn) {
        s_check_conn->is_closing = 1;
        s_check_conn = NULL;
      }
      s_internet_ok = false;
      s_connectivity_deadline = 0;
      s_connectivity_check_ms = now_ms + CONNECTIVITY_CHECK_RETRY_MS;
    }
  }

  // SNTP retry timer
  if (s_sntp_next_retry_ms > 0 &&
      (st == WIFI_STATUS_CONNECTED || st == WIFI_STATUS_ONLINE)) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms >= s_sntp_next_retry_ms) {
      s_sntp_next_retry_ms = 0;
      start_sntp();
    }
  }

  // Process any disconnect deferred from inside a Mongoose callback (SNTP etc.)
  // We're already on Core 1 here, so call mg_wifi_disconnect() directly.
  if (s_disconnect_pending) {
    s_disconnect_pending = false;
    // Before c1_disconnect: its synchronous link-down (tcpip_cb) must see an
    // intended disconnect, not warn "WiFi disconnected" (as wifi_disconnect).
    s_status = WIFI_STATUS_DISCONNECTED;
    c1_disconnect();
    { uint32_t save = spin_lock_blocking(s_state_lock);
      s_ssid[0] = '\0';
      s_ip[0] = '\0';
      spin_unlock(s_state_lock, save);
    }
    s_internet_ok = false;
    printf("WiFi: disconnected (SNTP deferred)\n");
  }
}

// mbedtls/time support.  (Guard: not in the simulator's SIM_FIRMWARE_NET
// build, which has no mbedTLS and must not replace the host's time().)
#ifndef PICODECK_SIM_FIRMWARE_NET
#include "mbedtls/platform_time.h"

mbedtls_ms_time_t mbedtls_platform_ms_time(void) {
  return (mbedtls_ms_time_t)(time_us_64() / 1000);
}

// Override time() for mbedtls certificate validation
time_t time(time_t *t) {
  time_t now = (time_t)clock_get_epoch();
  if (t)
    *t = now;
  return now;
}
#endif  // !PICODECK_SIM_FIRMWARE_NET
