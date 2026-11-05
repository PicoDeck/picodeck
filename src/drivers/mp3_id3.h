#pragma once

// ID3 tags in an MP3 file (mp3_player.c). Pure, header-only; host-tested in
// tests/unit/test_mp3_id3.c.
//
// The player starts decoding, and loops back to, the first byte after the
// tags at the front of the file. Handed a tag, libmad found no sync word at
// each of its bytes in turn: the decode loop gives it the buffer afresh every
// frame (sync assumed), so each byte was a LOSTSYNC of its own, 32 to an
// update. A 4.5 KB tag (Nova Rail's music cut with ffmpeg) held the decoder
// up ~140 ms at every play and every loop, which the PCM ring could not cover
// at 44.1 kHz stereo; a 100 KB tag with cover art, seconds.
//
// A tag further on (issue #63: after the audio, or between two streams joined
// with cat) is met only where the decoder lost sync, which a stream's frames
// never make it do. There the player also needs mp3_tag_size() and
// mp3_stream_resumes(), which are strict: bytes taken for a tag are not heard,
// and skipping audio is worse than walking a tag.

#include <stdbool.h>
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

#define MP3_ID3V1_SIZE     128u
// Bytes mp3_stream_resumes() reads where a tag ends.
#define MP3_RESUME_BYTES   4u

static inline bool mp3_tag_magic(const uint8_t *p, size_t n, const char *magic) {
    for (size_t i = 0; i < 3; i++)
        if (i < n && p[i] != (uint8_t)magic[i])
            return false;
    return true;
}

// The fields an ID3v2 header and footer share, as the specification allows
// them: version 2.2 to 2.4, no reserved flag set, a size of 7-bit bytes.
static inline bool mp3_id3v2_fields_ok(const uint8_t *p) {
    uint8_t major = p[3];
    if (major < 2 || major > 4 || p[4] == 0xFF)
        return false;
    uint8_t reserved = major == 2 ? 0x3Fu : major == 3 ? 0x1Fu : 0x0Fu;
    return !(p[5] & reserved) && !((p[6] | p[7] | p[8] | p[9]) & 0x80);
}

// p's n bytes end before they can tell an ID3v2 header or footer from noise.
static inline bool mp3_tag_cut_short(const uint8_t *p, size_t n) {
    return n < MP3_ID3V2_HEADER &&
           (mp3_tag_magic(p, n, "ID3") || mp3_tag_magic(p, n, "3DI"));
}

// The length of the tag that starts at p, if what is there may be one; 0 if
// it cannot be. Where the tag ends is the caller's to check, against the
// file's size and with mp3_stream_resumes():
//  - an ID3v2 tag: as mp3_id3v2_size(), with every header field valid;
//  - an ID3v2 footer on its own (its header behind us: only a v2.4 tag has
//    one, and it says so): its own 10 bytes;
//  - an ID3v1 tag: "TAG" and 128 bytes. Nothing else marks one.
static inline uint32_t mp3_tag_size(const uint8_t *p, size_t n) {
    if (n >= 3 && mp3_tag_magic(p, 3, "TAG"))
        return MP3_ID3V1_SIZE;
    if (n < MP3_ID3V2_HEADER)
        return 0;
    if (mp3_tag_magic(p, 3, "ID3"))
        return mp3_id3v2_fields_ok(p) ? mp3_id3v2_size(p, n) : 0;
    if (mp3_tag_magic(p, 3, "3DI") && mp3_id3v2_fields_ok(p) && p[3] == 4 && (p[5] & 0x10))
        return MP3_ID3V2_HEADER;
    return 0;
}

// The header of a frame the decoder can play (MPEG 1, 2 or 2.5, Layer III,
// a bitrate and a sample rate that exist): four bytes.
static inline bool mp3_frame_header_ok(const uint8_t *p) {
    return p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 &&
           ((p[1] >> 3) & 3u) != 1u &&   // MPEG version: reserved
           ((p[1] >> 1) & 3u) == 1u &&   // Layer III
           (p[2] >> 4) != 15u &&         // bitrate: not allowed
           ((p[2] >> 2) & 3u) != 3u;     // sample rate: reserved
}

// Whether the stream goes on at p (MP3_RESUME_BYTES there): a frame, or another
// tag. A tag's size is believed only if it lands on one of them. (The first
// three bytes of a tag, or the sync word and fields of a frame, are as much as
// the size can be held to here; the tag it lands on then answers for itself.)
static inline bool mp3_stream_resumes(const uint8_t *p) {
    if (mp3_frame_header_ok(p))
        return true;
    if (mp3_tag_magic(p, 3, "ID3") || mp3_tag_magic(p, 3, "3DI"))
        return p[3] >= 2 && p[3] <= 4;
    return mp3_tag_magic(p, 3, "TAG");
}
