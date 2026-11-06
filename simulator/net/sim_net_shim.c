// simulator/net/sim_net_shim.c — host shims for the firmware network stack.
//
// Built only with -DSIM_FIRMWARE_NET=ON (simulator/CMakeLists.txt), which
// compiles the firmware's src/drivers/wifi.c, http.c and tcp.c against
// Mongoose as MG_ARCH_UNIX (BSD sockets, no TLS) in place of the simulator's
// libcurl layer (sim_wifi.c, sim_http.c, sim_tcp.c). This file supplies what
// those sources take from the Pico SDK and from firmware-only modules:
//
//   hardware/sync.h spinlocks   → one pthread mutex per spinlock number
//   pico/multicore.h            → get_core_num() from a thread-local; doorbells
//   rng.h + mg_random           → getrandom(2) (rng.c drives the RP2350 TRNG)
//   os/core1_alloc.h            → calloc/free, so ASan sees Mongoose's buffers
//   the CYW43 interface         → a stand-in link (see net/include/mongoose.h)
//
// Thread model: Core 0 is the simulator's main thread (launcher, Lua VM);
// Core 1 is its core1_thread, which calls sim_net_core1_init() once and then
// wifi_poll() every tick. As on the device, only Core 1 touches Mongoose;
// Core 0 reaches it through wifi_req_push()'s ring.

#include "mongoose.h"  // net/include shadow: real Mongoose + stand-in iface
#include "sim_net.h"

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "os/core1_alloc.h"
#include "rng.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

// ── Spinlocks ───────────────────────────────────────────────────────────────
// 32 numbered locks like the RP2350's SIO spinlocks. As in the Pico SDK,
// spin_lock_claim_unused() hands out only 24..31 (PICO_SPINLOCK_ID_CLAIM_FREE_
// FIRST..LAST) and panics when they run out with required=true, so running
// out of spinlocks fails here exactly where it fails on the device. A
// claimed number's mutex is reused across claim/unclaim; it is never
// destroyed.

#define SIM_NUM_SPIN_LOCKS 32
#define SIM_SPIN_CLAIM_FIRST 24
#define SIM_SPIN_CLAIM_LAST 31

struct sim_spin_lock {
  pthread_mutex_t mutex;
};

static struct sim_spin_lock s_spin_locks[SIM_NUM_SPIN_LOCKS];
static bool s_spin_claimed[SIM_NUM_SPIN_LOCKS];
static pthread_mutex_t s_claim_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t s_spin_once = PTHREAD_ONCE_INIT;

static void spin_locks_init(void) {
  for (int i = 0; i < SIM_NUM_SPIN_LOCKS; i++)
    pthread_mutex_init(&s_spin_locks[i].mutex, NULL);
}

int spin_lock_claim_unused(bool required) {
  pthread_once(&s_spin_once, spin_locks_init);
  pthread_mutex_lock(&s_claim_mutex);
  for (int i = SIM_SPIN_CLAIM_FIRST; i <= SIM_SPIN_CLAIM_LAST; i++) {
    if (!s_spin_claimed[i]) {
      s_spin_claimed[i] = true;
      pthread_mutex_unlock(&s_claim_mutex);
      return i;
    }
  }
  pthread_mutex_unlock(&s_claim_mutex);
  if (required) {
    // The SDK panics here too.
    fprintf(stderr, "[SIMNET] panic: No spin locks are available "
                    "(spin_lock_claim_unused, ids %d-%d all claimed)\n",
            SIM_SPIN_CLAIM_FIRST, SIM_SPIN_CLAIM_LAST);
    fflush(stdout);
    abort();
  }
  return -1;
}

void spin_lock_unclaim(int lock_num) {
  if (lock_num < 0 || lock_num >= SIM_NUM_SPIN_LOCKS) return;
  pthread_mutex_lock(&s_claim_mutex);
  s_spin_claimed[lock_num] = false;
  pthread_mutex_unlock(&s_claim_mutex);
}

spin_lock_t *spin_lock_instance(int lock_num) {
  pthread_once(&s_spin_once, spin_locks_init);
  if (lock_num < 0 || lock_num >= SIM_NUM_SPIN_LOCKS) return NULL;
  return &s_spin_locks[lock_num];
}

uint32_t spin_lock_blocking(spin_lock_t *lock) {
  pthread_mutex_lock(&lock->mutex);
  return 0;
}

void spin_unlock(spin_lock_t *lock, uint32_t saved_irq) {
  (void)saved_irq;
  pthread_mutex_unlock(&lock->mutex);
}

// ── Cores and doorbells ─────────────────────────────────────────────────────

static _Thread_local unsigned int s_core_num = 0;
static _Atomic uint32_t s_doorbells_rung = 0;

void sim_net_core1_init(void) { s_core_num = 1; }

unsigned int get_core_num(void) { return s_core_num; }

void multicore_doorbell_claim(unsigned int doorbell_num,
                              unsigned int core_mask) {
  (void)doorbell_num;
  (void)core_mask;
}

void multicore_doorbell_set_other_core(unsigned int doorbell_num) {
  (void)doorbell_num;
  atomic_fetch_add_explicit(&s_doorbells_rung, 1, memory_order_relaxed);
}

uint32_t sim_net_doorbell_count(void) {
  return atomic_load_explicit(&s_doorbells_rung, memory_order_relaxed);
}

// ── Randomness (rng.h, mg_random) ───────────────────────────────────────────
// The host kernel CSPRNG stands in for the TRNG-seeded CTR_DRBG. With TLS
// compiled out nothing here protects a secret; Mongoose draws DNS ids and
// ephemeral values from it.

static bool host_random(void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  while (len > 0) {
    ssize_t n = getrandom(p, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      memset(buf, 0, len);
      return false;
    }
    p += n;
    len -= (size_t)n;
  }
  return true;
}

bool rng_init_this_core(void) { return true; }
bool rng_ready(void) { return true; }
bool rng_bytes(void *buf, size_t len) { return host_random(buf, len); }
void rng_refill(void) {}
bool rng_trng_read(uint8_t *out, size_t len) { return host_random(out, len); }

int rng_mbedtls_random(void *p_rng, unsigned char *out, size_t len) {
  (void)p_rng;
  return host_random(out, len) ? 0 : -1;
}

bool mg_random(void *buf, size_t len) { return host_random(buf, len); }

// ── Core 1 allocator (os/core1_alloc.h) ─────────────────────────────────────
// On the device Mongoose allocates from a private first-fit pool in PSRAM.
// Here it is the host heap, so ASan tracks every Mongoose buffer.

void core1_alloc_init(void *pool, size_t pool_size) {
  (void)pool;
  (void)pool_size;
}
void core1_alloc_stats(size_t *free_total, size_t *largest) {
  *free_total = *largest = 0;  // the host heap: nothing to report
}
void *core1_malloc(size_t size) { return malloc(size); }
void *core1_calloc(size_t count, size_t size) { return calloc(count, size); }
void core1_free(void *ptr) { free(ptr); }
bool core1_owns(const void *ptr) {
  (void)ptr;
  return true;
}

// ── Stand-in network interface ──────────────────────────────────────────────
// Everything below runs on Core 1 (wifi_init runs on Core 0 before Core 1
// polls, as on the device).

struct mg_tcpip_driver mg_tcpip_driver_pico_w;

// wifi.c's one interface (its s_ifp), recorded by mg_tcpip_init.
static struct mg_tcpip_if *s_ifp;

void mg_tcpip_init(struct mg_mgr *mgr, struct mg_tcpip_if *ifp) {
  ifp->mgr = mgr;
  ifp->state = MG_TCPIP_STATE_DOWN;
  memset(&ifp->ip, 0, sizeof(ifp->ip));
  mgr->ifp = ifp;  // wifi_init tests this for "driver present"
  s_ifp = ifp;
  printf("[SIMNET] firmware network stack on Mongoose/POSIX (TLS off)\n");
}

// Link state changes go through wifi.c's tcpip_cb, as the CYW43 driver's do.
static void set_state(struct mg_tcpip_if *ifp, uint8_t state) {
  ifp->state = state;
  if (ifp->pfn) ifp->pfn(ifp, MG_TCPIP_EV_ST_CHG, &ifp->state);
}

// Association and DHCP complete at once: the link goes straight to READY
// with 127.0.0.1 (a host socket build has no interface of its own).
bool sim_net_wifi_connect(struct mg_wifi_data *wifi) {
  if (!s_ifp) return false;
  printf("[SIMNET] associate with '%s' (stand-in link, 127.0.0.1)\n",
         wifi && wifi->ssid ? wifi->ssid : "");
  memset(&s_ifp->ip, 0, sizeof(s_ifp->ip));
  s_ifp->ip.addr.ip4 = htonl(INADDR_LOOPBACK);
  set_state(s_ifp, MG_TCPIP_STATE_READY);
  return true;
}

bool sim_net_wifi_disconnect(void) {
  if (!s_ifp) return true;
  memset(&s_ifp->ip, 0, sizeof(s_ifp->ip));
  set_state(s_ifp, MG_TCPIP_STATE_DOWN);
  return true;
}

// ── Loopback SNTP / connectivity-check endpoints ────────────────────────────

static int s_check_fd = -1;
static int s_sntp_fd = -1;
static char s_check_url[64];
static char s_sntp_url[64];
static pthread_once_t s_endpoints_once = PTHREAD_ONCE_INIT;

static int loopback_socket(int type, char *url, size_t url_len,
                           const char *scheme) {
  int fd = socket(AF_INET, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port = 0;
  socklen_t sl = sizeof(sa);
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
      (type == SOCK_STREAM && listen(fd, SOMAXCONN) != 0) ||
      getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
    close(fd);
    return -1;
  }
  snprintf(url, url_len, "%s://127.0.0.1:%u", scheme,
           (unsigned)ntohs(sa.sin_port));
  return fd;
}

static void endpoints_init(void) {
  const char *env = getenv("PICODECK_SIM_NET_CHECK_URL");
  if (env && env[0]) {
    snprintf(s_check_url, sizeof(s_check_url), "%s", env);
  } else {
    s_check_fd = loopback_socket(SOCK_STREAM, s_check_url, sizeof(s_check_url),
                                 "tcp");
    if (s_check_fd < 0)
      snprintf(s_check_url, sizeof(s_check_url), "tcp://127.0.0.1:1");
  }
  env = getenv("PICODECK_SIM_NET_SNTP_URL");
  if (env && env[0]) {
    snprintf(s_sntp_url, sizeof(s_sntp_url), "%s", env);
  } else {
    s_sntp_fd = loopback_socket(SOCK_DGRAM, s_sntp_url, sizeof(s_sntp_url),
                                "udp");
    if (s_sntp_fd < 0)
      snprintf(s_sntp_url, sizeof(s_sntp_url), "udp://127.0.0.1:1");
  }
}

const char *sim_net_check_url(void) {
  pthread_once(&s_endpoints_once, endpoints_init);
  // Close the previous checks' connections (the check only needs connect()
  // to succeed; it closes its end straight away).
  if (s_check_fd >= 0) {
    int fd;
    while ((fd = accept(s_check_fd, NULL, NULL)) >= 0) close(fd);
  }
  return s_check_url;
}

const char *sim_net_sntp_url(void) {
  pthread_once(&s_endpoints_once, endpoints_init);
  return s_sntp_url;
}
