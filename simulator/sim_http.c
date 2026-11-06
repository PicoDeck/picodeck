// sim_http.c — libcurl multi-based HTTP client for PicoDeck simulator
// Implements the full http.h API using libcurl on the host machine.
// Core 1 thread calls http_poll() which drives curl_multi_perform().

#define _GNU_SOURCE  // strcasestr
#include "http.h"
#include "wifi.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// ── Static pool ─────────────────────────────────────────────────────────────

static http_conn_t s_conns[HTTP_MAX_CONNECTIONS];
static CURLM *s_multi = NULL;

// Per-connection curl state (stored alongside the connection)
static struct curl_slist *s_hdrlists[HTTP_MAX_CONNECTIONS];
static bool s_paused[HTTP_MAX_CONNECTIONS];  // Core 1: curl's transfer is paused

// Guards the receive ring and spill: Core 1 (the curl callbacks) fills them
// while Core 0 reads them.
static pthread_mutex_t s_rx_lock = PTHREAD_MUTEX_INITIALIZER;

// Unread body bytes beyond the ring, as many as the device's stack would hold
// before it (HTTP_RECV_OVERFLOW in http.c), so a response of that size completes
// on the server's close even if the app reads only then. Past it the transfer
// pauses, so nothing is lost; the device has no such brake and fails instead.
#define SIM_HTTP_SPILL_MAX (32u * 1024u)

// ── Internal helpers ────────────────────────────────────────────────────────

static int conn_index(http_conn_t *c) {
    return (int)(c - s_conns);
}

// Deadlines use the host's clock, not the simulator's (virtual) one: the
// network runs in real time.
static uint32_t real_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

// A deadline `ms` from now; 0 (never fires) when the timeout is off.
static uint32_t deadline_after(uint32_t ms) {
    return ms ? real_ms() + ms : 0;
}

static bool deadline_passed(uint32_t now, uint32_t deadline) {
    return deadline != 0 && (int32_t)(now - deadline) > 0;
}

static void pending_set(http_conn_t *c, uint8_t bits) {
    __atomic_fetch_or(&c->pending, bits, __ATOMIC_RELAXED);
}

static void conn_fail(http_conn_t *c, const char *fmt, ...) {
    if (c->state == HTTP_STATE_DONE || c->state == HTTP_STATE_FAILED)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err), fmt, ap);
    va_end(ap);
    printf("[HTTP] Error (slot %d, state %d): %s\n", conn_index(c), (int)c->state, c->err);
    c->state = HTTP_STATE_FAILED;
    pending_set(c, HTTP_CB_FAILED | HTTP_CB_CLOSED);
}

// ── Ring buffer ─────────────────────────────────────────────────────────────

// Returns the bytes taken (less than len once the ring is full).
static uint32_t rx_write(http_conn_t *c, const uint8_t *data, uint32_t len) {
    uint32_t space = c->rx_cap - c->rx_count;
    if (len > space)
        len = space;
    if (len == 0)
        return 0;

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
    return len;
}

// Append to the spill (s_rx_lock held). False if out of memory.
static bool spill_append(http_conn_t *c, const uint8_t *data, uint32_t len) {
    uint32_t unread = c->spill_len - c->spill_off;
    if (unread)
        memmove(c->spill, c->spill + c->spill_off, unread);
    c->spill_off = 0;
    c->spill_len = unread;
    uint8_t *nb = realloc(c->spill, unread + len);
    if (!nb)
        return false;
    memcpy(nb + unread, data, len);
    c->spill = nb;
    c->spill_len = unread + len;
    return true;
}

// ── libcurl callbacks (called from Core 1 inside curl_multi_perform) ────────

static size_t sim_http_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    http_conn_t *c = (http_conn_t *)userdata;
    size_t total = size * nitems;

    // Parse status line: "HTTP/1.1 200 OK\r\n"
    if (total > 12 && strncmp(buffer, "HTTP/", 5) == 0) {
        const char *code_start = strchr(buffer, ' ');
        if (code_start) {
            c->status_code = atoi(code_start + 1);
            printf("[HTTP] Status %d (slot %d)\n", c->status_code, conn_index(c));
        }
        return total;
    }

    // Blank line = end of headers
    if (total <= 2) {
        if (c->status_code >= 100 && c->status_code < 200) {
            c->hdr_len = 0;  // an interim response (100 Continue): skip it
            c->hdr_count = 0;
            return total;
        }
        c->headers_done = true;
        c->state = HTTP_STATE_BODY;
        c->deadline_connect = 0;
        c->deadline_read = deadline_after(c->read_timeout_ms);
        pending_set(c, HTTP_CB_HEADERS);
        return total;
    }

    // Parse "Key: Value\r\n"
    char *colon = memchr(buffer, ':', total);
    if (!colon)
        return total;

    size_t key_len = colon - buffer;
    // Skip ": " after colon
    char *val_start = colon + 1;
    while (val_start < buffer + total && *val_start == ' ')
        val_start++;
    // Trim \r\n from value
    size_t val_len = (buffer + total) - val_start;
    while (val_len > 0 && (val_start[val_len - 1] == '\r' || val_start[val_len - 1] == '\n'))
        val_len--;

    // Check Content-Length
    if (key_len == 14 && strncasecmp(buffer, "Content-Length", 14) == 0) {
        c->content_length = atoi(val_start);
        printf("[HTTP] Content-Length %d (slot %d)\n", c->content_length, conn_index(c));
    }

    // Store in header buffer
    size_t need = key_len + 1 + val_len + 1;
    if (c->hdr_count >= HTTP_MAX_HDR_ENTRIES || c->hdr_len + need > HTTP_HEADER_BUF_MAX)
        return total;

    // Copy key (lowercased)
    c->hdr_keys[c->hdr_count] = &c->hdr_buf[c->hdr_len];
    for (size_t i = 0; i < key_len; i++) {
        char ch = buffer[i];
        c->hdr_buf[c->hdr_len++] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    c->hdr_buf[c->hdr_len++] = '\0';

    // Copy value
    c->hdr_vals[c->hdr_count] = &c->hdr_buf[c->hdr_len];
    memcpy(&c->hdr_buf[c->hdr_len], val_start, val_len);
    c->hdr_len += val_len;
    c->hdr_buf[c->hdr_len++] = '\0';

    c->hdr_count++;
    return total;
}

static size_t sim_http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_conn_t *c = (http_conn_t *)userdata;
    size_t total = size * nmemb;

    // What the ring cannot take goes to the spill, read after the ring. curl
    // keeps a chunk it is told to pause on, so nothing is dropped however far
    // behind the app is (a transfer may hand over more than the ring holds).
    pthread_mutex_lock(&s_rx_lock);
    uint32_t unread = c->spill_len - c->spill_off;
    if (unread > SIM_HTTP_SPILL_MAX) {
        pthread_mutex_unlock(&s_rx_lock);
        s_paused[conn_index(c)] = true;
        c->deadline_read = deadline_after(c->read_timeout_ms);  // not idle
        return CURL_WRITEFUNC_PAUSE;
    }
    uint32_t took = unread ? 0 : rx_write(c, (const uint8_t *)ptr, (uint32_t)total);
    bool ok = took == total ||
              spill_append(c, (const uint8_t *)ptr + took, (uint32_t)total - took);
    pthread_mutex_unlock(&s_rx_lock);
    if (!ok)
        return 0;  // curl fails the transfer (CURLE_WRITE_ERROR)
    c->body_received += (uint32_t)total;
    c->deadline_read = deadline_after(c->read_timeout_ms);
    pending_set(c, HTTP_CB_REQUEST);

    return total;
}

// ── Pool management (called from Core 0 / Lua bridge) ───────────────────────

void http_init(void) {
    memset(s_conns, 0, sizeof(s_conns));
    memset(s_hdrlists, 0, sizeof(s_hdrlists));
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_multi = curl_multi_init();
    printf("[HTTP] Simulator HTTP initialized (libcurl)\n");
}

http_conn_t *http_alloc(void) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (!s_conns[i].in_use) {
            memset(&s_conns[i], 0, sizeof(s_conns[i]));
            s_conns[i].in_use = true;
            s_conns[i].range_from = -1;
            s_conns[i].range_to = -1;
            s_conns[i].connect_timeout_ms = 10000;
            s_conns[i].read_timeout_ms = 30000;
            s_conns[i].content_length = -1;
            s_conns[i].hdr_buf = malloc(HTTP_HEADER_BUF_MAX);
            s_conns[i].rx_buf = malloc(HTTP_RECV_BUF_DEFAULT);
            s_conns[i].rx_cap = HTTP_RECV_BUF_DEFAULT;
            if (!s_conns[i].hdr_buf || !s_conns[i].rx_buf) {
                printf("[HTTP] Failed to allocate buffers for connection %d\n", i);
                http_free(&s_conns[i]);
                return NULL;
            }
            printf("[HTTP] Allocated connection %d\n", i);
            return &s_conns[i];
        }
    }
    printf("[HTTP] All %d connection slots in use\n", HTTP_MAX_CONNECTIONS);
    return NULL;
}

void http_free(http_conn_t *c) {
    if (!c)
        return;

    int idx = conn_index(c);

    // Push CLOSE if there's an active curl handle, or a start still queued
    // (Core 1 runs them in order, so the handle it makes is closed too).
    if (c->pcb || c->state == HTTP_STATE_QUEUED) {
        conn_req_t req = {.type = CONN_REQ_HTTP_CLOSE, .conn = c};
        wifi_req_push(&req);
    }

    // Wait briefly for Core 1 to process
    for (int i = 0; i < 200 && (c->pcb != NULL || c->state == HTTP_STATE_QUEUED); i++) {
        struct timespec ts = {0, 1000000}; // 1ms
        nanosleep(&ts, NULL);
    }

    free(c->path);
    free(c->extra_hdrs);
    free(c->tx_buf);
    free(c->rx_buf);
    free(c->hdr_buf);
    free(c->spill);

    if (s_hdrlists[idx]) {
        curl_slist_free_all(s_hdrlists[idx]);
        s_hdrlists[idx] = NULL;
    }

    memset(c, 0, sizeof(*c));
}

void http_close(http_conn_t *c) {
    if (!c)
        return;
    if (c->pcb) {
        conn_req_t req = {.type = CONN_REQ_HTTP_CLOSE, .conn = c};
        wifi_req_push(&req);
    }
    if (c->state != HTTP_STATE_QUEUED) {
        c->state = HTTP_STATE_IDLE;
    }
    c->pending = 0;
}

void http_close_all(void (*on_free)(void *lua_ud)) {
    // Push CLOSE for all active connections
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (s_conns[i].in_use) {
            conn_req_t req = {.type = CONN_REQ_HTTP_CLOSE, .conn = &s_conns[i]};
            wifi_req_push(&req);
        }
    }

    // Wait for Core 1 to process
    for (int wait = 0; wait < 100; wait++) {
        bool any_pending = false;
        for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
            if (s_conns[i].in_use &&
                (s_conns[i].pcb != NULL || s_conns[i].state == HTTP_STATE_QUEUED)) {
                any_pending = true;
                break;
            }
        }
        if (!any_pending)
            break;
        struct timespec ts = {0, 5000000}; // 5ms
        nanosleep(&ts, NULL);
    }

    // Free all connections
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (s_conns[i].in_use) {
            if (on_free && s_conns[i].lua_ud)
                on_free(s_conns[i].lua_ud);
            http_free(&s_conns[i]);
        }
    }
}

bool http_set_recv_buf(http_conn_t *c, uint32_t bytes) {
    if (!c || bytes == 0 || bytes > HTTP_RECV_BUF_MAX)
        return false;
    uint8_t *nb = malloc(bytes);
    if (!nb)
        return false;
    pthread_mutex_lock(&s_rx_lock);
    uint8_t *old = c->rx_buf;
    c->rx_buf = nb;
    c->rx_cap = bytes;
    c->rx_head = 0;
    c->rx_tail = 0;
    c->rx_count = 0;
    pthread_mutex_unlock(&s_rx_lock);
    free(old);
    return true;
}

// ── Request initiation (Core 0) ─────────────────────────────────────────────

static char *sim_strdup(const char *s) {
    if (!s)
        return NULL;
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d)
        memcpy(d, s, len);
    return d;
}

static bool start_request(http_conn_t *c, const char *method, const char *path,
                          const char *extra_hdr, const char *body, size_t body_len) {
    if (!c)
        return false;
    if (!wifi_is_available())
        return false;
    // Only between requests, as on the device.
    if (c->state != HTTP_STATE_IDLE && c->state != HTTP_STATE_DONE &&
        c->state != HTTP_STATE_FAILED)
        return false;

    // Reset state for new request
    c->status_code = 0;
    c->content_length = -1;
    c->body_received = 0;
    c->headers_done = false;
    pthread_mutex_lock(&s_rx_lock);
    c->rx_head = 0;
    c->rx_tail = 0;
    c->rx_count = 0;
    c->spill_len = c->spill_off = 0;
    pthread_mutex_unlock(&s_rx_lock);
    c->hdr_len = 0;
    c->hdr_count = 0;
    c->err[0] = '\0';
    c->pending = 0;

    strncpy(c->method, method, sizeof(c->method) - 1);

    free(c->path);
    c->path = sim_strdup(path);
    if (!c->path) {
        conn_fail(c, "path alloc failed");
        return false;
    }

    // setByteRange's range goes out as one more extra header, for this request.
    free(c->extra_hdrs);
    c->extra_hdrs = NULL;
    if (c->range_from >= 0) {
        size_t cap = (extra_hdr ? strlen(extra_hdr) : 0) + 48;
        c->extra_hdrs = malloc(cap);
        if (c->extra_hdrs) {
            int off = snprintf(c->extra_hdrs, cap, "%s", extra_hdr ? extra_hdr : "");
            if (c->range_to >= 0)
                snprintf(c->extra_hdrs + off, cap - off, "Range: bytes=%d-%d\r\n",
                         (int)c->range_from, (int)c->range_to);
            else
                snprintf(c->extra_hdrs + off, cap - off, "Range: bytes=%d-\r\n",
                         (int)c->range_from);
        }
        c->range_from = c->range_to = -1;
    } else if (extra_hdr) {
        c->extra_hdrs = sim_strdup(extra_hdr);
    }
    free(c->tx_buf);
    c->tx_buf = NULL;
    c->tx_len = 0;
    if (body && body_len > 0) {  // binary safe
        c->tx_buf = malloc(body_len);
        if (c->tx_buf) {
            memcpy(c->tx_buf, body, body_len);
            c->tx_len = (uint32_t)body_len;
        }
    }

    c->deadline_connect = deadline_after(c->connect_timeout_ms);
    c->deadline_read = 0;
    c->state = HTTP_STATE_QUEUED;

    conn_req_t req = {.type = CONN_REQ_HTTP_START, .conn = c};
    if (!wifi_req_push(&req)) {
        free(c->path);
        c->path = NULL;
        free(c->extra_hdrs);
        c->extra_hdrs = NULL;
        free(c->tx_buf);
        c->tx_buf = NULL;
        conn_fail(c, "request queue full");
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

void http_fail_unstarted(http_conn_t *c) {
    if (!c || (c->state != HTTP_STATE_IDLE && c->state != HTTP_STATE_DONE))
        return;
    if (!c->err[0])
        snprintf(c->err, sizeof(c->err), "WiFi not available");
    c->status_code = 0;
    c->state = HTTP_STATE_FAILED;
}

// ── Data reading (Core 0) ───────────────────────────────────────────────────

uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len) {
    if (!c || !out || len == 0)
        return 0;
    pthread_mutex_lock(&s_rx_lock);
    uint32_t n = (len < c->rx_count) ? len : c->rx_count;

    uint32_t till_end = c->rx_cap - c->rx_tail;
    if (n <= till_end) {
        memcpy(out, &c->rx_buf[c->rx_tail], n);
        c->rx_tail += n;
        if (c->rx_tail == c->rx_cap)
            c->rx_tail = 0;
    } else {
        memcpy(out, &c->rx_buf[c->rx_tail], till_end);
        memcpy(out + till_end, c->rx_buf, n - till_end);
        c->rx_tail = n - till_end;
    }
    c->rx_count -= n;
    // The ring first, then the spill (it came after).
    if (n < len && c->spill_off < c->spill_len) {
        uint32_t m = c->spill_len - c->spill_off;
        if (m > len - n)
            m = len - n;
        memcpy(out + n, c->spill + c->spill_off, m);
        c->spill_off += m;
        n += m;
    }
    pthread_mutex_unlock(&s_rx_lock);
    return n;
}

uint32_t http_bytes_available(http_conn_t *c) {
    if (!c)
        return 0;
    pthread_mutex_lock(&s_rx_lock);
    uint32_t n = c->rx_count + (c->spill_len - c->spill_off);
    pthread_mutex_unlock(&s_rx_lock);
    return n;
}

http_conn_t *http_get_conn(int idx) {
    return (idx >= 0 && idx < HTTP_MAX_CONNECTIONS && s_conns[idx].in_use)
               ? &s_conns[idx]
               : NULL;
}

uint8_t http_take_pending(http_conn_t *c) {
    if (!c)
        return 0;
    uint8_t p = __atomic_exchange_n(&c->pending, 0, __ATOMIC_RELAXED);
    return p;
}

// ── Core 1 functions ────────────────────────────────────────────────────────

extern bool sim_network_blocked(void);
extern const char *sim_wifi_get_error(void);

void sim_http_start(http_conn_t *c) {
    if (!c || !s_multi) {
        if (c) conn_fail(c, "curl not initialized");
        return;
    }
    if (c->state != HTTP_STATE_QUEUED)
        return;  // closed before Core 1 got to it
    if (sim_network_blocked()) {
        conn_fail(c, "network error: %s", sim_wifi_get_error());
        return;
    }

    int idx = conn_index(c);

    // Clean up any previous curl handle
    if (c->pcb) {
        curl_multi_remove_handle(s_multi, (CURL *)c->pcb);
        curl_easy_cleanup((CURL *)c->pcb);
        c->pcb = NULL;
    }
    if (s_hdrlists[idx]) {
        curl_slist_free_all(s_hdrlists[idx]);
        s_hdrlists[idx] = NULL;
    }

    CURL *easy = curl_easy_init();
    if (!easy) {
        conn_fail(c, "curl_easy_init failed");
        return;
    }

    // Build URL
    char url[1024];
    snprintf(url, sizeof(url), "%s://%s:%u%s",
             c->use_ssl ? "https" : "http",
             c->server, (unsigned)c->port,
             c->path ? c->path : "/");

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, c);

    // Callbacks
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, sim_http_header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, c);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sim_http_write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, c);

    // Do NOT follow redirects — apps handle redirects themselves
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);

    // Timeouts: http_poll enforces the connect and read deadlines (a read
    // timeout restarts with every byte; curl's own would cap the whole transfer).

    // SSL: the system CA bundle; setInsecure skips the check, as on the device.
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, c->insecure ? 0L : 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, c->insecure ? 0L : 2L);

    // Method
    if (strcmp(c->method, "POST") == 0) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        if (c->tx_buf && c->tx_len > 0) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, c->tx_buf);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)c->tx_len);
        } else {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, 0L);
        }
    }

    // Build header list
    struct curl_slist *hdrs = NULL;

    // Host header
    char host_hdr[256];
    if (c->port != (c->use_ssl ? 443 : 80))
        snprintf(host_hdr, sizeof(host_hdr), "Host: %s:%u", c->server, (unsigned)c->port);
    else
        snprintf(host_hdr, sizeof(host_hdr), "Host: %s", c->server);
    hdrs = curl_slist_append(hdrs, host_hdr);

    // Connection header
    hdrs = curl_slist_append(hdrs, c->keep_alive ? "Connection: keep-alive" : "Connection: close");

    // User-Agent (unless extra_hdrs provides one)
    bool has_ua = c->extra_hdrs &&
                  (strstr(c->extra_hdrs, "User-Agent:") != NULL ||
                   strstr(c->extra_hdrs, "user-agent:") != NULL);
    if (!has_ua) {
        hdrs = curl_slist_append(hdrs, "User-Agent: PicoDeck/1.0");
    }
    // The device sends only these: drop what curl would add by itself (an
    // "Expect" would also delay a large POST by a second).
    hdrs = curl_slist_append(hdrs, "Accept:");
    hdrs = curl_slist_append(hdrs, "Expect:");
    if (!(c->extra_hdrs && strcasestr(c->extra_hdrs, "Content-Type:")))
        hdrs = curl_slist_append(hdrs, "Content-Type:");
    if (!(c->extra_hdrs && strcasestr(c->extra_hdrs, "Accept-Encoding:")))
        hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");

    // Parse extra headers ("\r\n"-delimited)
    if (c->extra_hdrs) {
        char *copy = strdup(c->extra_hdrs);
        if (copy) {
            char *line = copy;
            while (*line) {
                char *eol = strstr(line, "\r\n");
                if (eol) {
                    *eol = '\0';
                    if (line[0])
                        hdrs = curl_slist_append(hdrs, line);
                    line = eol + 2;
                } else {
                    if (line[0])
                        hdrs = curl_slist_append(hdrs, line);
                    break;
                }
            }
            free(copy);
        }
        free(c->extra_hdrs);
        c->extra_hdrs = NULL;
    }

    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    s_hdrlists[idx] = hdrs;

    // Add to multi handle
    CURLMcode mc = curl_multi_add_handle(s_multi, easy);
    if (mc != CURLM_OK) {
        conn_fail(c, "curl_multi_add_handle: %s", curl_multi_strerror(mc));
        curl_easy_cleanup(easy);
        if (s_hdrlists[idx]) {
            curl_slist_free_all(s_hdrlists[idx]);
            s_hdrlists[idx] = NULL;
        }
        return;
    }

    c->pcb = easy;
    s_paused[idx] = false;
    c->state = HTTP_STATE_CONNECTING;
    printf("[HTTP] Started %s %s (slot %d)\n", c->method, url, idx);
}

void sim_http_close_handle(http_conn_t *c) {
    if (!c)
        return;

    int idx = conn_index(c);

    if (c->pcb && s_multi) {
        curl_multi_remove_handle(s_multi, (CURL *)c->pcb);
        curl_easy_cleanup((CURL *)c->pcb);
        c->pcb = NULL;
    }
    s_paused[idx] = false;
    if (s_hdrlists[idx]) {
        curl_slist_free_all(s_hdrlists[idx]);
        s_hdrlists[idx] = NULL;
    }
}

// Timeouts and paused transfers, before curl runs.
static void sim_http_tick(void) {
    uint32_t now = real_ms();
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        http_conn_t *c = &s_conns[i];
        if (!c->in_use || !c->pcb)
            continue;
        CURL *easy = (CURL *)c->pcb;
        if (c->state == HTTP_STATE_CONNECTING) {
            // Connected (and through TLS): the request is on its way, and the
            // read timeout runs from here.
            curl_off_t t = 0;
            curl_easy_getinfo(easy, CURLINFO_PRETRANSFER_TIME_T, &t);
            if (t > 0) {
                c->deadline_connect = 0;
                c->deadline_read = deadline_after(c->read_timeout_ms);
                c->state = HTTP_STATE_HEADERS;
            } else if (deadline_passed(now, c->deadline_connect)) {
                conn_fail(c, "connect timeout (%ums)", (unsigned)c->connect_timeout_ms);
                sim_http_close_handle(c);
                continue;
            }
        } else if (c->state == HTTP_STATE_HEADERS || c->state == HTTP_STATE_BODY) {
            if (deadline_passed(now, c->deadline_read)) {
                conn_fail(c, "read timeout (%ums)", (unsigned)c->read_timeout_ms);
                sim_http_close_handle(c);
                continue;
            }
        }
        if (s_paused[i]) {
            pthread_mutex_lock(&s_rx_lock);
            uint32_t unread = c->spill_len - c->spill_off;
            pthread_mutex_unlock(&s_rx_lock);
            if (unread <= SIM_HTTP_SPILL_MAX) {
                s_paused[i] = false;  // the callback sets it again if need be
                curl_easy_pause(easy, CURLPAUSE_CONT);
            }
        }
    }
}

void http_poll(void) {
    if (!s_multi)
        return;

    sim_http_tick();

    int still_running = 0;
    curl_multi_perform(s_multi, &still_running);

    // Check for completed transfers
    CURLMsg *msg;
    int msgs_left;
    while ((msg = curl_multi_info_read(s_multi, &msgs_left)) != NULL) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        CURL *easy = msg->easy_handle;
        http_conn_t *c = NULL;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &c);
        if (!c)
            continue;

        int idx = conn_index(c);
        CURLcode result = msg->data.result;

        if (result == CURLE_OK) {
            c->deadline_connect = c->deadline_read = 0;
            c->state = HTTP_STATE_DONE;
            pending_set(c, HTTP_CB_COMPLETE);
            printf("[HTTP] Transfer complete (slot %d, %u bytes)\n", idx, c->body_received);
        } else if (result == CURLE_PARTIAL_FILE && c->content_length >= 0) {
            // A body cut short never reports complete: what arrived stays readable.
            conn_fail(c, "connection closed after %u of %d bytes",
                      c->body_received, c->content_length);
        } else if (c->body_received > 0) {
            conn_fail(c, "partial: %s", curl_easy_strerror(result));
        } else {
            conn_fail(c, "curl error: %s", curl_easy_strerror(result));
        }

        // Cleanup the easy handle
        curl_multi_remove_handle(s_multi, easy);
        curl_easy_cleanup(easy);
        c->pcb = NULL;
        s_paused[idx] = false;

        if (s_hdrlists[idx]) {
            curl_slist_free_all(s_hdrlists[idx]);
            s_hdrlists[idx] = NULL;
        }

        // Free tx_buf if still allocated
        if (c->tx_buf) {
            free(c->tx_buf);
            c->tx_buf = NULL;
        }
    }
}

void http_fire_c_pending(void) {
    // No-op for simulator — native app C callbacks not used
}

// ── Mongoose stubs (declared in http.h but not used in simulator) ───────────

struct mg_connection;

void http_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
    (void)nc;
    (void)ev;
    (void)ev_data;
}

// ── Core 0 views (http.h) ───────────────────────────────────────────────────
// This libcurl layer frees slots synchronously, so there is nothing to
// reap; the getters read the fields directly (the firmware's http.c reads
// them race-free across its two cores).

void http_reap(void) {}

http_state_t http_get_state(http_conn_t *c) {
    return c ? c->state : HTTP_STATE_IDLE;
}

const char *http_get_error(http_conn_t *c) {
    return (c && c->err[0]) ? c->err : NULL;
}

bool http_headers_ready(http_conn_t *c) {
    return c && c->headers_done;
}

int http_get_status(http_conn_t *c) {
    return c ? c->status_code : 0;
}

void http_get_progress(http_conn_t *c, int *received, int *total) {
    if (received) *received = c ? (int)c->body_received : 0;
    if (total) *total = c ? (int)c->content_length : -1;
}

bool http_is_complete(http_conn_t *c) {
    return c && (c->state == HTTP_STATE_DONE || c->state == HTTP_STATE_FAILED);
}
