#pragma once

// ID3v2 tags at the front of an MP3 file (mp3_player.c). Pure, header-only;
// host-tested in tests/unit/test_mp3_id3.c.
//
// The player starts decoding, and loops back to, the first byte after the
// tags. Handed the tag, libmad found no sync word at each of its bytes in
// turn: the decode loop gives it the buffer afresh every frame (sync
// assumed), so each byte was a LOSTSYNC of its own, 32 to an update. A
// 4.5 KB tag (Nova Rail's music cut with ffmpeg) held the decoder up
// ~140 ms at every play and every loop, which the PCM ring could not cover
// at 44.1 kHz stereo; a 100 KB tag with cover art, seconds.

#include <stddef.h>
#include <stdint.h>

#define MP3_ID3V2_HEADER 10u

// The length of the ID3v2 tag that starts at p (n bytes there), header
// and footer included; 0 if p does not start one. Needs only the 10-byte
// header: the tag may run on past the n bytes.
static inline uint32_t mp3_id3v2_size(const uint8_t *p, size_t n) {
    if (n < MP3_ID3V2_HEADER || p[0] != 'I' || p[1] != 'D' || p[2] != '3')
        return 0;
    // "ID3", version (never 0xFF), flags, size as four 7-bit bytes.
    if (p[3] == 0xFF || p[4] == 0xFF || ((p[6] | p[7] | p[8] | p[9]) & 0x80))
        return 0;
    uint32_t size = ((uint32_t)p[6] << 21) | ((uint32_t)p[7] << 14) |
                    ((uint32_t)p[8] << 7) | (uint32_t)p[9];
    // A footer (v2.4 only: in v2.3 the flag's bit is undefined)
    uint32_t footer = (p[3] >= 4 && (p[5] & 0x10)) ? MP3_ID3V2_HEADER : 0u;
    return MP3_ID3V2_HEADER + size + footer;
}
