#pragma once

// ======================================================================
// Integrity for dev-console transfers: the FNV-1a the transfers report, and
// the wire format of the `screenshot` command's binary stream. Pure, shared by
// the firmware (dev_commands.c) and the simulator's `screenshot` RPC, format
// "serial" (sim_socket_handler.c). The host's reader is tools/scrn_frame.py.
//
//   "SCRN" | width u16 | height u16 | format u16 (565) | flags u16    12 bytes, LE
//   width * height * 2 bytes of pixels, as the framebuffer holds them
//   "\n[DEV] SCRN_END fnv1a=xxxxxxxx\n"          when flags & SCRN_FLAG_TRAILER
//
// The hash is over the pixels alone, as screenshot64's is, so a frame reads the
// same either way. Before the trailer existed the flags were two zero bytes of
// padding that no reader looked at: an old host still takes the pixels by their
// offset and closes the port on the trailer, and a new host seeing flags 0 knows
// an old device has no trailer to wait for.
// ======================================================================

#include <stdint.h>
#include <stdio.h>

#define SERIAL_FNV1A_INIT 2166136261u

// FNV-1a 32-bit: a cheap check for serial transfers, not a cryptographic one
// (OTA does its own SHA-256 before touching flash).
static inline uint32_t serial_fnv1a_update(uint32_t h, const uint8_t *p,
                                           uint32_t n) {
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

#define SCRN_HEADER_LEN 12
#define SCRN_FORMAT_RGB565 565
#define SCRN_FLAG_TRAILER 0x0001u
#define SCRN_TRAILER_LEN 31

static inline void scrn_frame_header(uint8_t out[SCRN_HEADER_LEN], uint16_t width,
                                     uint16_t height, uint16_t format,
                                     uint16_t flags) {
    const uint16_t fields[4] = {width, height, format, flags};
    out[0] = 'S';
    out[1] = 'C';
    out[2] = 'R';
    out[3] = 'N';
    for (int i = 0; i < 4; i++) {
        out[4 + i * 2] = (uint8_t)fields[i];
        out[5 + i * 2] = (uint8_t)(fields[i] >> 8);
    }
}

static inline void scrn_frame_trailer(char out[SCRN_TRAILER_LEN + 1],
                                      uint32_t hash) {
    snprintf(out, SCRN_TRAILER_LEN + 1, "\n[DEV] SCRN_END fnv1a=%08lx\n",
             (unsigned long)hash);
}
