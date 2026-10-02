#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../os/os.h"   // wifi_status_t
#include "http.h"       // http_conn_t (for conn_req_t)

// =============================================================================
// WiFi Driver — CYW43 on Pimoroni Pico Plus 2W
//
// CYW43 WiFi chip on SPI1. LCD uses PIO0 SPI (separate bus, no contention).
// Core 1 exclusively owns all Mongoose/CYW43 operations.
//
// Connection is non-blocking: call wifi_connect(), then poll wifi_get_status()
// or let the OS Lua hook drive wifi_poll() automatically in the background.
//
// Compile guard: WIFI_ENABLED=1 is defined by CMakeLists when the board has
// a CYW43 chip. All functions are safe no-ops when WIFI_ENABLED is absent.
//
// Thread model: Core 1 is the sole owner of the Mongoose manager. Core 0
// never calls mg_* functions directly. Instead it pushes conn_req_t entries
// to a spinlock-guarded ring buffer; Core 1's wifi_poll() drains them before
// each mg_mgr_poll() call.
// =============================================================================

// Initialise CYW43 hardware and enable station mode. Call once during boot
// after sdcard / config are initialised. Sets wifi_is_available() if hardware
// is found. If config holds "wifi_ssid" / "wifi_pass", starts auto-connect.
void wifi_init(void);

// Returns true if CYW43 hardware was found and initialised successfully.
bool wifi_is_available(void);

// Begin connecting to a WiFi network (non-blocking, WPA/WPA2).
// s_status transitions to WIFI_STATUS_CONNECTING immediately; check
// wifi_get_status() for CONNECTED or FAILED.
void wifi_connect(const char *ssid, const char *password);

// Disconnect from the current network.
void wifi_disconnect(void);

// Current connection state (WIFI_STATUS_*).
wifi_status_t wifi_get_status(void);

// Null-terminated IP address string (e.g. "192.168.1.42"), or NULL if not
// connected.
const char *wifi_get_ip(void);

// SSID of the current or pending connection, or NULL if fully disconnected.
const char *wifi_get_ssid(void);

// Drive the CYW43 lwip-poll state machine and update connection status.
// Must be called regularly from Core 1. No-op when WiFi is not available.
void wifi_poll(void);

// Set whether an app with HTTP requirement is running. When true, WiFi will
// not automatically disconnect after SNTP time sync.
void wifi_set_http_required(bool required);
bool wifi_get_http_required(void);

// Returns true if internet connectivity has been verified (TCP to 8.8.8.8:53).
// This goes beyond wifi_get_status() == CONNECTED which only means DHCP is done.
bool wifi_has_internet(void);

// Returns true once the CYW43 hardware disconnect has actually completed on
// Core 1: the station has left the network and Core 1 no longer polls the
// chip (until the next wifi_connect). wifi_disconnect() is async
// (queue-based); this flag lets callers wait before voltage- or
// clock-sensitive operations (SD flash programming, a sysclk change).
bool wifi_hw_disconnected(void);

// Quiets the CYW43 driver for a sysclk change, once wifi_hw_disconnected()
// and not connecting or connected (a no-op otherwise: Core 1 may be
// polling the chip): puts its SPI bus to sleep at the current clock and
// stops its polling until wifi_resume_radio(). The driver does its work in
// its async context's low-priority interrupt on Core 0, and sleeps the bus
// by itself ~2.5 s after its last activity (50 checks 50 ms apart); at a
// sysclk its PIO SPI was not set up for (the video's 300 MHz, an app's
// system_clock_khz) that handshake fails 64 times 1 ms apart
// ("cyw43_kso_set(0): failed"): Core 0 stalls for ~68 ms. A connect still
// in flight when the radio went idle can wake the bus again later, hence
// the pause, not only the sleep. Core 0.
void wifi_pause_radio(void);
// Undoes wifi_pause_radio() (a no-op when not paused): call it once sysclk
// is back to the one the CYW43's PIO SPI was set up for (200 MHz).
// wifi_connect() calls it too. Core 0.
void wifi_resume_radio(void);

// Clock changes (launcher_apply_clock, Core 0, with Core 1 paused). The
// CYW43's PIO SPI clock is clk_sys / (2 x div); the SDK sets div once, at
// init (2: 50 MHz at 200 MHz, the chip's limit), so at 300 MHz the bus ran
// at 75 MHz. wifi_bus_hold(true) takes the driver's async-context lock, so
// no CYW43 transfer (WiFi or Bluetooth, from Core 0's driver interrupt)
// runs across the switch; wifi_bus_clock(khz) sets div = ceil(khz / 100 MHz)
// (at least 2), the bus at or under 50 MHz at that clk_sys: once for the
// faster of the two clocks before the switch (in spec at both), once for
// the new clock after it. wifi_bus_hold(false) releases the lock (the
// driver's pending work runs then). No-ops without the CYW43.
void wifi_bus_hold(bool hold);
void wifi_bus_clock(uint32_t khz);

// How long a caller should wait for wifi_hw_disconnected() after
// wifi_disconnect(): Core 1 drains the request on its next tick, but may be
// inside a long mg_mgr_poll (a TLS handshake) first.  Bounded; the callers
// that change sysclk (launcher, video) use this one value.
#define WIFI_HW_DISCONNECT_WAIT_MS 2000

// ── Core 0 → Core 1 request queue ────────────────────────────────────────────
//
// Core 0 (Lua main loop) must never call mg_* functions directly. Instead it
// populates the relevant http_conn_t fields and pushes a conn_req_t to this
// queue. Core 1's drain_requests() (called from wifi_poll) executes the
// actual Mongoose operations.

typedef enum {
    CONN_REQ_HTTP_START,      // new TCP conn (or reuse keep-alive) + send request
    CONN_REQ_HTTP_CLOSE,      // mark nc->is_closing = 1
    CONN_REQ_TCP_CONNECT,     // new generic TCP conn
    CONN_REQ_TCP_WRITE,       // send data over TCP
    CONN_REQ_TCP_CLOSE,       // close generic TCP
    CONN_REQ_WIFI_CONNECT,    // mg_wifi_connect()
    CONN_REQ_WIFI_DISCONNECT, // mg_wifi_disconnect()
} conn_req_type_t;

typedef struct {
    conn_req_type_t  type;
    http_conn_t     *conn;       // NULL for wifi ops; tcp_conn_t* for tcp ops
    char             ssid[64];   // used by CONN_REQ_WIFI_CONNECT
    char             pass[64];
    void            *data;       // used by CONN_REQ_TCP_WRITE (malloc'd)
    uint32_t         data_len;
} conn_req_t;

// Push a request from Core 0 to the Core 1 queue.
// Returns false (and does nothing) if the 8-slot queue is full.
// On success, rings a doorbell to wake Core 1 immediately (<1us latency
// vs. the previous 5ms polling interval).
bool wifi_req_push(const conn_req_t *req);

// ── TLS policy (Core 1) ──────────────────────────────────────────────────────
// Every TLS connection (HTTPS and tls:// TCP) verifies the server certificate
// against the root bundle in ca_bundle.c and the host name, unless the
// connection opted out (setInsecure).  Verification needs the wall clock, so
// a verifying handshake is refused until SNTP has set it.
#define WIFI_TLS_ERR_CLOCK "clock not set: TLS needs the time from SNTP " \
                           "(stay online a few seconds and retry)"

// Parse the CA bundle once into the chain every verifying connection shares.
// Call at boot, before Core 1 starts, on a large stack (x509 parsing of RSA-
// 4096/P-384 roots is deep): main() runs it via app_stack_run_os.  False if
// it failed; verifying TLS is then refused (insecure connections still work).
bool wifi_tls_init(void);

// If nc failed because the peer's certificate did not verify, write a
// readable reason ("TLS: certificate not trusted ...") to out and return
// true.  Call from MG_EV_ERROR, while nc->tls is still valid.
struct mg_connection;
bool wifi_tls_verify_error(struct mg_connection *nc, char *out, size_t n);

// Doorbell number used for Core 0 → Core 1 IPC wake-up.
// Claimed in wifi_init(); Core 1 registers the ISR in core1_entry().
#define WIFI_IPC_DOORBELL 0
