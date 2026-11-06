// sim_tcp.c — POSIX socket + libcurl CONNECT_ONLY TCP client for PicoDeck simulator
// Implements tcp.h API. Non-TLS uses raw POSIX sockets, TLS uses curl CONNECT_ONLY.

#include "tcp.h"
#include "wifi.h"
#include "os.h"

#include <curl/curl.h>
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/tcp.h>

// ── Static pool ─────────────────────────────────────────────────────────────

static tcp_conn_t s_conns[TCP_MAX_CONNECTIONS];

// Per-connection: either a raw socket fd or a CURL* handle for TLS
// We store the fd in pcb (cast to intptr_t + 1 so 0 means "no connection").
// For TLS connections, pcb holds a CURL* pointer directly.
// We need a way to distinguish: use a flag.
static bool s_is_tls[TCP_MAX_CONNECTIONS];
// For non-TLS: socket fd stored as (intptr_t)(fd + 1) in pcb (0 = no socket)
// For TLS: pcb = CURL* handle

static int get_socket_fd(tcp_conn_t *c) {
    return (int)((intptr_t)c->pcb - 1);
}

static void set_socket_fd(tcp_conn_t *c, int fd) {
    c->pcb = (void *)((intptr_t)(fd + 1));
}

static int conn_index(tcp_conn_t *c) {
    return (int)(c - s_conns);
}

// Guards the receive ring: Core 1 fills it while Core 0 reads it.
static pthread_mutex_t s_rx_lock = PTHREAD_MUTEX_INITIALIZER;

// Deadlines use the host's clock, not the simulator's (virtual) one: the
// network runs in real time.
static uint32_t real_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static bool deadline_passed(uint32_t now, uint32_t deadline) {
    return deadline != 0 && (int32_t)(now - deadline) > 0;
}

static void pending_set(tcp_conn_t *c, uint32_t bits) {
    __atomic_fetch_or(&c->pending, bits, __ATOMIC_RELAXED);
}

// FAILED with an error text; the socket (if any) is gone.
static void conn_fail(tcp_conn_t *c, const char *fmt, const char *arg) {
    snprintf(c->err, sizeof(c->err), fmt, arg);
    c->deadline_connect = c->deadline_read = 0;
    c->state = TCP_STATE_FAILED;
    pending_set(c, TCP_CB_FAILED);
}

// ── Ring buffer helpers ─────────────────────────────────────────────────────

static void rx_write(tcp_conn_t *c, const uint8_t *data, uint32_t len) {
    uint32_t space = c->rx_cap - c->rx_count;
    if (len > space)
        len = space;
    if (len == 0)
        return;

    uint32_t till_end = c->rx_cap - c->rx_head;
    if (len <= till_end) {
        memcpy(&c->rx_buf[c->rx_head], data, len);
        c->rx_head += len;
        if (c->rx_head == c->rx_cap)
            c->rx_head = 0;
    } else {
        memcpy(&c->rx_buf[c->rx_head], data, till_end);
        memcpy(c->rx_buf, data + till_end, len - till_end);
        c->rx_head = len - till_end;
    }
    c->rx_count += len;
}

// The lowercase hex SHA-256 of the peer certificate curl saw (CERTINFO's PEM
// "Cert:" field, decoded), as tcp.c computes it on the device; "" when curl
// gives none.
static void sim_peer_fingerprint(CURL *easy, char *out, size_t n) {
    if (n < 65) return;
    out[0] = '\0';
    struct curl_certinfo *ci = NULL;
    if (curl_easy_getinfo(easy, CURLINFO_CERTINFO, &ci) != CURLE_OK || !ci || ci->num_of_certs < 1)
        return;
    for (struct curl_slist *s = ci->certinfo[0]; s; s = s->next) {
        if (strncmp(s->data, "Cert:", 5) != 0) continue;
        const char *begin = strstr(s->data, "-----BEGIN CERTIFICATE-----");
        const char *end = begin ? strstr(begin, "-----END CERTIFICATE-----") : NULL;
        if (!end) return;
        begin += strlen("-----BEGIN CERTIFICATE-----");
        size_t cap = (size_t)(end - begin);
        unsigned char *b64 = malloc(cap + 1), *der = malloc(cap);
        size_t len = 0, der_len = 0;
        if (b64 && der) {
            for (const char *p = begin; p < end; p++)
                if (*p != '\n' && *p != '\r' && *p != ' ') b64[len++] = (unsigned char)*p;
            unsigned char h[32];
            if (mbedtls_base64_decode(der, cap, &der_len, b64, len) == 0 &&
                mbedtls_sha256(der, der_len, h, 0) == 0) {
                for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
            }
        }
        free(b64);
        free(der);
        return;
    }
}

// ── Pool management ─────────────────────────────────────────────────────────

void tcp_init(void) {
    memset(s_conns, 0, sizeof(s_conns));
    memset(s_is_tls, 0, sizeof(s_is_tls));
    printf("[TCP] Simulator TCP initialized\n");
}

tcp_conn_t *tcp_alloc(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!s_conns[i].in_use) {
            memset(&s_conns[i], 0, sizeof(tcp_conn_t));
            s_conns[i].rx_buf = malloc(TCP_RECV_BUF_DEFAULT);
            if (!s_conns[i].rx_buf)
                return NULL;
            s_conns[i].rx_cap = TCP_RECV_BUF_DEFAULT;
            s_conns[i].connect_timeout_ms = 15000;
            s_conns[i].in_use = true;
            s_is_tls[i] = false;
            return &s_conns[i];
        }
    }
    return NULL;
}

void tcp_free(tcp_conn_t *c) {
    if (!c)
        return;
    // Core 1 closes the socket and leaves the slot before it is cleared (the
    // close of a connect still queued comes after it, in order).
    tcp_close(c);
    for (int i = 0; i < 200 && (c->pcb != NULL || c->state == TCP_STATE_QUEUED); i++) {
        struct timespec ts = {0, 1000000};  // 1 ms
        nanosleep(&ts, NULL);
    }
    free(c->rx_buf);
    int idx = conn_index(c);
    s_is_tls[idx] = false;
    memset(c, 0, sizeof(tcp_conn_t));
}

tcp_conn_t *tcp_get_conn(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNECTIONS)
        return NULL;
    return s_conns[idx].in_use ? &s_conns[idx] : NULL;
}

// ── Request functions (Core 0, queue to Core 1) ─────────────────────────────

bool tcp_connect(tcp_conn_t *c, const char *host, uint16_t port, bool use_ssl) {
    if (!c || !wifi_is_available())
        return false;
    if (c->state != TCP_STATE_IDLE && c->state != TCP_STATE_CLOSED &&
        c->state != TCP_STATE_FAILED)
        return false;  // in progress or connected, as on the device
    if (host != c->host) {  // Lua passes the slot's own buffer
        strncpy(c->host, host, sizeof(c->host) - 1);
        c->host[sizeof(c->host) - 1] = '\0';
    }
    c->port = port;
    c->use_ssl = use_ssl;
    c->pending = 0;
    pthread_mutex_lock(&s_rx_lock);
    c->rx_head = c->rx_tail = c->rx_count = 0;
    pthread_mutex_unlock(&s_rx_lock);
    c->err[0] = '\0';
    c->fingerprint[0] = '\0';
    c->deadline_connect = c->connect_timeout_ms ? real_ms() + c->connect_timeout_ms : 0;
    c->deadline_read = 0;
    c->state = TCP_STATE_QUEUED;

    conn_req_t req = {.type = CONN_REQ_TCP_CONNECT, .conn = (http_conn_t *)c};
    return wifi_req_push(&req);
}

int tcp_write(tcp_conn_t *c, const void *buf, int len) {
    if (!c || c->state != TCP_STATE_CONNECTED || !c->pcb)
        return -1;

    void *copy = malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, buf, len);

    conn_req_t req = {
        .type = CONN_REQ_TCP_WRITE,
        .conn = (http_conn_t *)c,
        .data = copy,
        .data_len = (uint32_t)len,
    };

    if (!wifi_req_push(&req)) {
        free(copy);
        return -1;
    }
    return len;
}

void tcp_close(tcp_conn_t *c) {
    if (!c || (!c->pcb && c->state != TCP_STATE_QUEUED))
        return;
    conn_req_t req = {.type = CONN_REQ_TCP_CLOSE, .conn = (http_conn_t *)c};
    // A close right after a burst of writes finds the queue full. Dropped, the
    // socket would stay open and the server would never see the close.
    for (int i = 0; i < 2000 && !wifi_req_push(&req); i++) {
        struct timespec ts = {0, 1000000};  // 1 ms
        nanosleep(&ts, NULL);
    }
}

// ── Data reading (Core 0) ───────────────────────────────────────────────────

int tcp_read(tcp_conn_t *c, void *buf, int len) {
    if (!c || len == 0)
        return 0;
    pthread_mutex_lock(&s_rx_lock);
    uint32_t n = ((uint32_t)len < c->rx_count) ? (uint32_t)len : c->rx_count;

    uint32_t till_end = c->rx_cap - c->rx_tail;
    if (n <= till_end) {
        memcpy(buf, &c->rx_buf[c->rx_tail], n);
        c->rx_tail += n;
        if (c->rx_tail == c->rx_cap)
            c->rx_tail = 0;
    } else {
        memcpy(buf, &c->rx_buf[c->rx_tail], till_end);
        memcpy((uint8_t *)buf + till_end, c->rx_buf, n - till_end);
        c->rx_tail = n - till_end;
    }
    c->rx_count -= n;
    pthread_mutex_unlock(&s_rx_lock);
    return (int)n;
}

uint32_t tcp_bytes_available(tcp_conn_t *c) {
    if (!c)
        return 0;
    pthread_mutex_lock(&s_rx_lock);
    uint32_t n = c->rx_count;
    pthread_mutex_unlock(&s_rx_lock);
    return n;
}

const char *tcp_get_error(tcp_conn_t *c) {
    return (c && c->err[0]) ? c->err : NULL;
}

// ── Core 0 helpers (tcp.h) ──────────────────────────────────────────────────
// This layer frees slots synchronously: nothing to reap.

void tcp_reap(void) {}

void tcp_close_all(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++)
        if (s_conns[i].in_use)
            tcp_free(&s_conns[i]);
}

tcp_conn_state_t tcp_get_state(tcp_conn_t *c) {
    return c ? c->state : TCP_STATE_IDLE;
}

uint32_t tcp_take_pending_bits(tcp_conn_t *c, uint32_t mask) {
    if (!c)
        return 0;
    return __atomic_fetch_and(&c->pending, ~mask, __ATOMIC_RELAXED) & mask;
}

void tcp_set_connect_timeout(tcp_conn_t *c, uint32_t ms) {
    if (c) c->connect_timeout_ms = ms;
}

void tcp_set_read_timeout(tcp_conn_t *c, uint32_t ms) {
    if (!c)
        return;
    c->read_timeout_ms = ms;
    if (c->state == TCP_STATE_CONNECTED)
        c->deadline_read = ms ? real_ms() + ms : 0;
}

uint32_t tcp_take_pending(tcp_conn_t *c) {
    if (!c)
        return 0;
    uint32_t p = __atomic_exchange_n(&c->pending, (uint32_t)0, __ATOMIC_RELAXED);
    return p;
}

// ── Core 1 drain handlers ───────────────────────────────────────────────────

extern bool sim_network_blocked(void);
extern const char *sim_wifi_get_error(void);

// The socket is up: CONNECTED, with the read timeout running.
static void connected(tcp_conn_t *c) {
    c->deadline_connect = 0;
    c->deadline_read = c->read_timeout_ms ? real_ms() + c->read_timeout_ms : 0;
    c->state = TCP_STATE_CONNECTED;
    pending_set(c, TCP_CB_CONNECT);
}

void sim_tcp_start_connect(tcp_conn_t *c) {
    if (c->state != TCP_STATE_QUEUED)
        return;  // closed before Core 1 got to it
    if (sim_network_blocked()) {
        conn_fail(c, "network error: %s", sim_wifi_get_error());
        return;
    }
    int idx = conn_index(c);

    if (c->use_ssl) {
        // TLS: use libcurl CONNECT_ONLY
        CURL *easy = curl_easy_init();
        if (!easy) {
            conn_fail(c, "%s", "curl_easy_init failed");
            return;
        }

        char url[256];
        snprintf(url, sizeof(url), "https://%s:%u", c->host, (unsigned)c->port);
        curl_easy_setopt(easy, CURLOPT_URL, url);
        curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L);
        // setInsecure skips the certificate check, as on the device. No ALPN:
        // the device's Mongoose offers none, and this is a raw TLS socket.
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, c->insecure ? 0L : 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, c->insecure ? 0L : 2L);
        curl_easy_setopt(easy, CURLOPT_SSL_ENABLE_ALPN, 0L);
        curl_easy_setopt(easy, CURLOPT_CERTINFO, 1L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, (long)c->connect_timeout_ms);

        // Blocking perform on Core 1 — OK since it's a background thread
        c->state = TCP_STATE_CONNECTING;
        CURLcode res = curl_easy_perform(easy);
        if (res != CURLE_OK) {
            conn_fail(c, "TLS connect: %s", curl_easy_strerror(res));
            curl_easy_cleanup(easy);
            return;
        }

        sim_peer_fingerprint(easy, c->fingerprint, sizeof(c->fingerprint));
        c->pcb = easy;
        s_is_tls[idx] = true;
        connected(c);
        printf("[TCP] TLS connected to %s:%u (slot %d)\n", c->host, c->port, idx);
    } else {
        // Plain TCP: POSIX socket
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)c->port);

        int err = getaddrinfo(c->host, port_str, &hints, &res);
        if (err != 0 || !res) {
            conn_fail(c, "DNS: %s", gai_strerror(err));
            if (res) freeaddrinfo(res);
            return;
        }

        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            conn_fail(c, "socket: %s", strerror(errno));
            freeaddrinfo(res);
            return;
        }

        // Non-blocking from the start: sim_tcp_poll finishes the connect, so
        // an unanswered one times out as on the device and holds up nothing.
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = connect(fd, res->ai_addr, res->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            conn_fail(c, "connect: %s", strerror(errno));
            close(fd);
            freeaddrinfo(res);
            return;
        }
        freeaddrinfo(res);

        set_socket_fd(c, fd);
        s_is_tls[idx] = false;
        if (rc == 0) {
            connected(c);
            printf("[TCP] Connected to %s:%u (slot %d, fd %d)\n", c->host, c->port, idx, fd);
        } else {
            c->state = TCP_STATE_CONNECTING;
        }
    }
}

void sim_tcp_do_write(tcp_conn_t *c, void *data, uint32_t len) {
    if (!c || !c->pcb || c->state != TCP_STATE_CONNECTED) {
        free(data);
        return;
    }

    int idx = conn_index(c);

    if (s_is_tls[idx]) {
        // TLS: curl_easy_send, all of it (the device's stack buffers a write).
        const char *p = data;
        size_t left = len;
        for (int tries = 0; left > 0 && tries < 2000; tries++) {
            size_t sent = 0;
            CURLcode res = curl_easy_send((CURL *)c->pcb, p, left, &sent);
            if (res != CURLE_OK && res != CURLE_AGAIN) {
                conn_fail(c, "TLS send: %s", curl_easy_strerror(res));
                break;
            }
            p += sent;
            left -= sent;
            if (res == CURLE_AGAIN) {
                struct timespec ts = {0, 1000000};  // 1 ms
                nanosleep(&ts, NULL);
            }
        }
    } else {
        // Plain TCP: send(), all of it (the device's stack buffers a write).
        int fd = get_socket_fd(c);
        const char *p = data;
        size_t left = len;
        for (int tries = 0; left > 0 && tries < 2000; tries++) {
            ssize_t sent = send(fd, p, left, MSG_NOSIGNAL);
            if (sent >= 0) {
                p += sent;
                left -= (size_t)sent;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                poll(&pfd, 1, 1);
            } else {
                conn_fail(c, "send: %s", strerror(errno));
                break;
            }
        }
    }

    free(data);
}

void sim_tcp_do_close(tcp_conn_t *c) {
    if (!c)
        return;

    int idx = conn_index(c);

    if (c->pcb) {
        if (s_is_tls[idx]) {
            curl_easy_cleanup((CURL *)c->pcb);
        } else {
            int fd = get_socket_fd(c);
            close(fd);
        }
        c->pcb = NULL;
    }

    c->deadline_connect = c->deadline_read = 0;
    c->state = TCP_STATE_CLOSED;
    pending_set(c, TCP_CB_CLOSED);
    s_is_tls[idx] = false;
}

// A timeout: the socket goes, and the slot fails with the device's text.
static void poll_fail_timeout(tcp_conn_t *c, int idx, const char *what, uint32_t ms) {
    if (s_is_tls[idx])
        curl_easy_cleanup((CURL *)c->pcb);
    else
        close(get_socket_fd(c));
    c->pcb = NULL;
    s_is_tls[idx] = false;
    char msg[48];
    snprintf(msg, sizeof(msg), "%s timeout (%ums)", what, (unsigned)ms);
    conn_fail(c, "%s", msg);
}

// Received bytes: into the ring, and the read timeout starts over.
static void got_data(tcp_conn_t *c, const uint8_t *data, uint32_t len) {
    pthread_mutex_lock(&s_rx_lock);
    rx_write(c, data, len);
    pthread_mutex_unlock(&s_rx_lock);
    if (c->read_timeout_ms)
        c->deadline_read = real_ms() + c->read_timeout_ms;
    pending_set(c, TCP_CB_READ);
}

// ── sim_tcp_poll() — called from wifi_poll() on Core 1 ──────────────────────

void sim_tcp_poll(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &s_conns[i];
        if (!c->in_use || !c->pcb)
            continue;
        uint32_t now = real_ms();

        if (c->state == TCP_STATE_CONNECTING) {
            if (s_is_tls[i])
                continue;
            int cfd = get_socket_fd(c);
            struct pollfd cp = {.fd = cfd, .events = POLLOUT};
            if (poll(&cp, 1, 0) > 0) {
                int so = 0;
                socklen_t solen = sizeof(so);
                getsockopt(cfd, SOL_SOCKET, SO_ERROR, &so, &solen);
                if (so) {
                    close(cfd);
                    c->pcb = NULL;
                    conn_fail(c, "connect: %s", strerror(so));
                } else {
                    connected(c);
                    printf("[TCP] Connected to %s:%u (slot %d, fd %d)\n", c->host, c->port, i, cfd);
                }
            } else if (deadline_passed(now, c->deadline_connect)) {
                poll_fail_timeout(c, i, "connect", c->connect_timeout_ms);
            }
            continue;
        }
        if (c->state != TCP_STATE_CONNECTED)
            continue;
        if (deadline_passed(now, c->deadline_read)) {
            poll_fail_timeout(c, i, "read", c->read_timeout_ms);
            continue;
        }

        pthread_mutex_lock(&s_rx_lock);
        uint32_t space = c->rx_cap - c->rx_count;
        pthread_mutex_unlock(&s_rx_lock);
        if (space == 0)
            continue;

        uint8_t buf[4096];
        uint32_t to_read = space < sizeof(buf) ? space : sizeof(buf);

        if (s_is_tls[i]) {
            // TLS: curl_easy_recv
            size_t nread = 0;
            CURLcode res = curl_easy_recv((CURL *)c->pcb, buf, to_read, &nread);
            if (res == CURLE_OK && nread > 0) {
                got_data(c, buf, (uint32_t)nread);
            } else if (res != CURLE_AGAIN) {
                // CURLE_OK with nothing read: the server closed (a TLS
                // close_notify, which Gemini servers always send); any other
                // result but CURLE_AGAIN is an error. Either way it is over.
                c->deadline_read = 0;
                c->state = TCP_STATE_CLOSED;
                pending_set(c, TCP_CB_CLOSED);
                curl_easy_cleanup((CURL *)c->pcb);
                c->pcb = NULL;
                s_is_tls[i] = false;
            }
        } else {
            // Plain TCP: poll + recv
            int fd = get_socket_fd(c);
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            int ret = poll(&pfd, 1, 0); // non-blocking
            if (ret > 0 && (pfd.revents & POLLIN)) {
                ssize_t n = recv(fd, buf, to_read, 0);
                if (n > 0) {
                    got_data(c, buf, (uint32_t)n);
                } else if (n == 0) {
                    // Peer closed
                    c->deadline_read = 0;
                    c->state = TCP_STATE_CLOSED;
                    pending_set(c, TCP_CB_CLOSED);
                    close(fd);
                    c->pcb = NULL;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    conn_fail(c, "recv: %s", strerror(errno));
                    close(fd);
                    c->pcb = NULL;
                }
            } else if (ret > 0 && (pfd.revents & (POLLERR | POLLHUP))) {
                c->deadline_read = 0;
                c->state = TCP_STATE_CLOSED;
                pending_set(c, TCP_CB_CLOSED);
                close(fd);
                c->pcb = NULL;
            }
        }
    }
}

// ── Mongoose stub ───────────────────────────────────────────────────────────

struct mg_connection;

void tcp_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
    (void)nc;
    (void)ev;
    (void)ev_data;
}
