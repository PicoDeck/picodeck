#pragma once

// =============================================================================
// Dev-command handlers shared by the firmware serial console (dev_commands.c)
// and the simulator's `dev_command` control-channel RPC, so the E2E suite
// drives the same code push_app uses on hardware.
//
// Each handler writes a one-line reply without the "[DEV] " prefix (the
// firmware prints "[DEV] <reply>", the simulator returns it over RPC) and
// returns true on success. Host tools match the reply text ("Unzipped N
// files", "Deleted:", "no app running"), so keep the wording stable.
//
// Callers run them through dev_commands_process, i.e. on an app stack.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>

#define DEV_OP_REPLY_MAX 512  // audiostat's line is the longest (~500 worst case)

// "exit": ask the running app to exit. With no app running there is nothing
// to exit: the reply is an error, but the exit flag is still raised so a
// modal open at the launcher (system menu, text input) closes, and the
// launcher loop then drops it.
bool dev_op_exit(char *reply, size_t n);

// "unzip <zip> <dest>": args is modified (split at the space).
bool dev_op_unzip(char *args, char *reply, size_t n);

// "rm <path>": delete a file or a directory tree.
bool dev_op_rm(const char *path, char *reply, size_t n);

// "audiostat [reset]": Core 1's tick cost, the output's refill interrupt,
// the stream's and MP3's underruns (with when the stream ran dry: its
// start, a loop point, the gaps and the ring's low-water mark), busy
// sample voices and sys_khz, as one "Audio: key=value ..." line. reset
// starts a new window first. Host tools parse key=value pairs: add
// fields, never rename or drop one.
bool dev_op_audiostat(bool reset, char *reply, size_t n);
