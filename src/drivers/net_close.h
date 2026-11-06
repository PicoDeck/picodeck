#pragma once

#include <stdbool.h>

// Ending a Mongoose connection that no HTTP or TCP slot will read again
// (Core 1 only). See "Closing" in src/drivers/CLAUDE.md.
//
// The slot lets go at once and is free for its next connection. The connection
// goes on without it: what the app wrote is still sent, then the FIN, and
// whatever arrives meanwhile is dropped. It ends when the peer answers the FIN,
// a second after the FIN if the peer does not, and in any case
// NET_CLOSE_LIMIT_MS after this call.
struct mg_connection;
struct mg_mgr;

#define NET_CLOSE_LIMIT_MS 10000

// Let nc go. fn_data is cleared, so no later event reaches the slot, and the
// caller drops its pointer: Mongoose frees nc later. Goes at once, with no FIN,
// when there is nothing to say goodbye on: no TCP connection yet, a TLS
// handshake under way, no link, or an error has already closed it.
void net_close_conn(struct mg_connection *nc);

// The link has gone: every connection let go here ends at the next poll. They
// could send nothing, and the stack's own timers do not run without a link.
void net_close_drop_all(struct mg_mgr *mgr);

// How many connections are still ending (the netstat dev command).
unsigned net_close_count(const struct mg_mgr *mgr);
