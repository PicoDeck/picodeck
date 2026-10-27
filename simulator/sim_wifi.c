// sim_wifi.c — Mock WiFi + IPC queue for PicoDeck simulator
// Always reports "connected" with fake IP/SSID. Provides the IPC ring buffer
// that bridges Core 0 (Lua) → Core 1 (network thread).

#include "http.h"
#include "tcp.h"
#include "wifi.h"
#include "os.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

// ── Mock WiFi state ─────────────────────────────────────────────────────────

static wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static char s_ssid[64] = "";
static char s_ip[16] = "";
static bool s_http_required = false;

// ── IPC ring buffer (Core 0 → Core 1) ──────────────────────────────────────

#define IPC_QUEUE_SIZE 8

static conn_req_t s_queue[IPC_QUEUE_SIZE];
static int s_queue_head = 0;
static int s_queue_tail = 0;
static int s_queue_count = 0;
static pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// ── Forward declarations for drain handlers ─────────────────────────────────

// Defined in sim_http.c
extern void sim_http_start(http_conn_t *c);
extern void sim_http_close_handle(http_conn_t *c);

// Defined in sim_tcp.c
extern void sim_tcp_start_connect(tcp_conn_t *c);
extern void sim_tcp_do_write(tcp_conn_t *c, void *data, uint32_t len);
extern void sim_tcp_do_close(tcp_conn_t *c);
extern void sim_tcp_poll(void);

// ── WiFi API implementation ─────────────────────────────────────────────────

void wifi_init(void) {
    s_status = WIFI_STATUS_ONLINE;
    strncpy(s_ssid, "SimulatorWiFi", sizeof(s_ssid));
    strncpy(s_ip, "192.168.1.100", sizeof(s_ip));
    printf("[WiFi] Simulator mock WiFi initialized (always online)\n");
}

extern bool sim_wifi_is_available(void);

bool wifi_is_available(void) {
    return sim_wifi_is_available();
}

void wifi_connect(const char *ssid, const char *password) {
    (void)password;
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    s_status = WIFI_STATUS_ONLINE;
    printf("[WiFi] Mock connect to '%s' — instant success\n", ssid);
}

void wifi_disconnect(void) {
    s_status = WIFI_STATUS_DISCONNECTED;
    s_ssid[0] = '\0';
    printf("[WiFi] Mock disconnect\n");
}

extern int sim_wifi_forced_status(void);

wifi_status_t wifi_get_status(void) {
    int forced = sim_wifi_forced_status();
    return forced >= 0 ? (wifi_status_t)forced : s_status;
}

const char *wifi_get_ip(void) {
    return (s_status == WIFI_STATUS_CONNECTED || s_status == WIFI_STATUS_ONLINE)
               ? s_ip : NULL;
}

bool wifi_has_internet(void) {
    return s_status == WIFI_STATUS_ONLINE;
}

const char *wifi_get_ssid(void) {
    return s_ssid[0] ? s_ssid : NULL;
}

void wifi_set_http_required(bool required) {
    s_http_required = required;
}

bool wifi_get_http_required(void) {
    return s_http_required;
}

bool wifi_hw_disconnected(void) {
    return false;
}

// No CYW43 driver to quiet across a clock change (wifi.h).
void wifi_pause_radio(void) {}
void wifi_resume_radio(void) {}
void wifi_bus_hold(bool hold) { (void)hold; }
void wifi_bus_clock(uint32_t khz) { (void)khz; }
uint32_t wifi_bus_errors(void) { return 0; }

// ── IPC queue ───────────────────────────────────────────────────────────────

bool wifi_req_push(const conn_req_t *req) {
    pthread_mutex_lock(&s_queue_mutex);
    if (s_queue_count >= IPC_QUEUE_SIZE) {
        pthread_mutex_unlock(&s_queue_mutex);
        printf("[WiFi] IPC queue full!\n");
        return false;
    }
    s_queue[s_queue_head] = *req;
    s_queue_head = (s_queue_head + 1) % IPC_QUEUE_SIZE;
    s_queue_count++;
    pthread_mutex_unlock(&s_queue_mutex);
    return true;
}

static bool queue_pop(conn_req_t *out) {
    pthread_mutex_lock(&s_queue_mutex);
    if (s_queue_count == 0) {
        pthread_mutex_unlock(&s_queue_mutex);
        return false;
    }
    *out = s_queue[s_queue_tail];
    s_queue_tail = (s_queue_tail + 1) % IPC_QUEUE_SIZE;
    s_queue_count--;
    pthread_mutex_unlock(&s_queue_mutex);
    return true;
}

// ── wifi_poll() — called from Core 1 thread ─────────────────────────────────

void wifi_poll(void) {
    // Drain the IPC queue
    conn_req_t req;
    while (queue_pop(&req)) {
        switch (req.type) {
        case CONN_REQ_HTTP_START:
            sim_http_start(req.conn);
            break;
        case CONN_REQ_HTTP_CLOSE:
            sim_http_close_handle(req.conn);
            break;
        case CONN_REQ_TCP_CONNECT:
            sim_tcp_start_connect((tcp_conn_t *)req.conn);
            break;
        case CONN_REQ_TCP_WRITE:
            sim_tcp_do_write((tcp_conn_t *)req.conn, req.data, req.data_len);
            break;
        case CONN_REQ_TCP_CLOSE:
            sim_tcp_do_close((tcp_conn_t *)req.conn);
            break;
        case CONN_REQ_WIFI_CONNECT:
            strncpy(s_ssid, req.ssid, sizeof(s_ssid) - 1);
            s_status = WIFI_STATUS_ONLINE;
            break;
        case CONN_REQ_WIFI_DISCONNECT:
            s_status = WIFI_STATUS_DISCONNECTED;
            s_ssid[0] = '\0';
            break;
        }
    }

    // Run curl multi + poll TCP sockets
    http_poll();
    sim_tcp_poll();
}
