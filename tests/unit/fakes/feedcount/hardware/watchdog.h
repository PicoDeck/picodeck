// hardware/watchdog.h for a host test that counts the feeds: long SD and zip
// loops must feed the device's 10 s watchdog, which nothing else can see.
#ifndef HARDWARE_WATCHDOG_H
#define HARDWARE_WATCHDOG_H

extern int g_watchdog_feeds;  // defined by the test
static inline void watchdog_update(void) { g_watchdog_feeds++; }

#endif  // HARDWARE_WATCHDOG_H
