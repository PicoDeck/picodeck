// simulator/net/include/mongoose.h — SIM_FIRMWARE_NET only.
//
// Shadows third_party/mongoose/mongoose.h for the firmware network sources
// (src/drivers/wifi.c, http.c, tcp.c) when the simulator is built with
// -DSIM_FIRMWARE_NET=ON. This directory is first on those files' include
// path; mongoose.c itself still gets the real header (a quoted include finds
// the file next to the includer first).
//
// On the host Mongoose is built as MG_ARCH_UNIX: BSD sockets, no built-in
// TCP/IP stack (MG_ENABLE_TCPIP=0) and no TLS (MG_TLS_NONE). wifi.c,
// however, drives the Pico W network interface (struct mg_tcpip_if, the CYW43
// driver, mg_wifi_connect). Instead of #ifdef-ing that out of wifi.c, this
// header declares a stand-in interface with just the fields wifi.c touches,
// and simulator/net/sim_net_shim.c implements mg_tcpip_init/mg_wifi_connect/
// mg_wifi_disconnect on it: connecting brings the fake link READY at once
// (address 127.0.0.1) through wifi.c's own tcpip_cb, so wifi.c's status,
// SNTP and connectivity-check state machine runs unmodified.
//
// Nothing in mongoose.c dereferences mgr->ifp when MG_ENABLE_TCPIP is 0 (every
// use is under #if MG_ENABLE_TCPIP), so the stand-in never reaches it.

#ifndef PICODECK_SIM_NET_MONGOOSE_H
#define PICODECK_SIM_NET_MONGOOSE_H

#include "../../../third_party/mongoose/mongoose.h"

#if MG_ENABLE_TCPIP || MG_TLS != MG_TLS_NONE
#error "SIM_FIRMWARE_NET expects MG_ARCH_UNIX sockets with MG_ENABLE_TCPIP=0 and TLS off"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ── Stand-in for the built-in stack's network interface ───────────────────

struct mg_tcpip_if;
typedef void (*mg_tcpip_event_handler_t)(struct mg_tcpip_if *ifp, int ev,
                                         void *ev_data);

enum {
  MG_TCPIP_EV_ST_CHG,             // state change: uint8_t * (&ifp->state)
  MG_TCPIP_EV_WIFI_CONNECT_ERR,   // Wi-Fi connect failed: int * (error)
};

#define MG_TCPIP_STATE_DOWN 0
#define MG_TCPIP_STATE_UP 1
#define MG_TCPIP_STATE_REQ 2
#define MG_TCPIP_STATE_IP 3
#define MG_TCPIP_STATE_READY 4

struct mg_tcpip_driver {
  int unused;
};

struct mg_tcpip_if {
  struct mg_addr ip;               // 127.0.0.1 once READY (printed with %M)
  struct mg_tcpip_driver *driver;  // &mg_tcpip_driver_pico_w
  void *driver_data;               // struct mg_tcpip_driver_pico_w_data *
  mg_tcpip_event_handler_t pfn;    // wifi.c's tcpip_cb
  struct mg_mgr *mgr;
  struct {
    size_t size;
  } recv_queue;                    // set by wifi_init, unused on the host
  bool enable_tcp_retransmit;      // set by wifi_init; host TCP does its own
  uint32_t nrtx;                   // always 0 here
  uint8_t state;                   // MG_TCPIP_STATE_*
};

struct mg_tcpip_driver_pico_w_data {
  struct mg_wifi_data wifi;
};

extern struct mg_tcpip_driver mg_tcpip_driver_pico_w;

// Attach ifp to mgr (the link starts DOWN). No socket work.
void mg_tcpip_init(struct mg_mgr *mgr, struct mg_tcpip_if *ifp);

// Wi-Fi association on the stand-in interface. mongoose.c (no Wi-Fi driver
// enabled) already defines mg_wifi_connect/mg_wifi_disconnect as "No Wi-Fi
// driver" errors, so wifi.c's calls are renamed to the shim's versions.
bool sim_net_wifi_connect(struct mg_wifi_data *wifi);
bool sim_net_wifi_disconnect(void);
#define mg_wifi_connect sim_net_wifi_connect
#define mg_wifi_disconnect sim_net_wifi_disconnect

// ── Loopback endpoints for wifi.c's SNTP query and connectivity check ─────
// wifi.c defaults to udp://pool.ntp.org:123 and tcp://8.8.8.8:53; the net
// simulator build overrides WIFI_SNTP_URL / WIFI_CHECK_URL with these so a
// test run never leaves the machine. The check URL is a loopback listener
// the shim owns (connect succeeds → "connectivity check OK" → ONLINE); the
// SNTP URL is a loopback UDP socket that never answers (the query stays
// pending, as on a network with NTP blocked). PICODECK_SIM_NET_CHECK_URL and
// PICODECK_SIM_NET_SNTP_URL in the environment override them.
const char *sim_net_check_url(void);
const char *sim_net_sntp_url(void);

#ifdef __cplusplus
}
#endif

#endif  // PICODECK_SIM_NET_MONGOOSE_H
