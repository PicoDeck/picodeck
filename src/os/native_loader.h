#pragma once
#include "app_runner.h"

// Native (PIE ELF) app runner — handles APP_TYPE_NATIVE entries.
extern const AppRunner g_native_runner;

// ── Per-launch resource tracking ─────────────────────────────────────────────
// Every object a native app creates through the API is tracked against it
// (app_identity.h: app_files_* for files, app_res_* for the kinds below) and
// whatever the app did not free is freed when it returns.  On firmware the
// app gets a per-launch copy of g_api whose create/free entries do the
// tracking; the simulator's Unicorn trampolines call the same helpers.
// HTTP/TCP connections are not listed: the loader closes the whole pool
// (http_close_all / tcp_close_all), which only native apps use meanwhile.
typedef enum {
  NATIVE_RES_QMI = 1,     // psram->qmiAlloc block (umm_malloc)
  NATIVE_RES_IMAGE,       // graphics->load / newBlank
  NATIVE_RES_SAMPLE,      // soundplayer->sampleLoad
  NATIVE_RES_PLAYER,      // soundplayer->playerNew
  NATIVE_RES_FILEPLAYER,  // soundplayer->filePlayerNew
  NATIVE_RES_MP3,         // soundplayer->mp3PlayerNew
  NATIVE_RES_VIDEO,       // video->newPlayer
  NATIVE_RES_MOD,         // modplayer->create
  NATIVE_RES_TERMINAL,    // terminal->create
  NATIVE_RES_AES,         // crypto->aesNew
  NATIVE_RES_ECDH,        // crypto->ecdhX25519 / ecdhP256
  NATIVE_RES_GFX3D,       // gfx3d->create (its textures and images are its own)
  NATIVE_RES_GFX3D_MESH,  // gfx3d->meshNew
} native_res_kind_t;

// Free one object of `kind` with its driver's destroy function.
void native_res_release(int kind, void *h);

// Track a freshly created h for the running app and return it; if it cannot
// be tracked (PSRAM exhausted) free it and return NULL.  NULL passes through.
void *native_res_adopt(int kind, void *h);

// An app-side free: untrack h and free it, or do nothing if h is not a live
// handle of this kind (double free, stray pointer, NULL).
void native_res_drop(int kind, void *h);

// The same pair for files (sdcard_fopen handles, app_files_* list).
void *native_file_adopt(void *f);
void native_file_drop(void *f);

// Free everything the running app still holds (files included); logs a
// summary naming the app.  Called by the loader after the app returns.
void native_res_release_all(const char *app_name);
