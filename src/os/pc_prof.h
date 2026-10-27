#pragma once

// The `prof` dev command: PC sampling of the running app (pc_prof.c).
//   prof start [period_us] [max_samples]   (defaults 200 us, 32768; at most
//                                          262144 samples, 2 MB of PSRAM)
//   prof stop | dump | free
void pc_prof_command(const char *args);
