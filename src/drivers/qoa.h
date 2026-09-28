#pragma once

// QOA ("Quite OK Audio") probing and whole-frame decode for the streaming
// file player.  Pure: caller-supplied buffers, no IO.  Decode defers to the
// vendored reference implementation (third_party/qoa, MIT); this module adds
// the player's limits (1-2 channels) and frame/seek arithmetic.
// Host-tested in tests/unit/test_qoa.c, fuzzed by tests/fuzz/fuzz_qoa.c.
//
// Every QOA frame carries its own LMS state, so frames decode independently:
// seeking is frame-index arithmetic and looping is a rewind, with no decoder
// state to carry across frames.

#include <stddef.h>
#include <stdint.h>

// Bytes qoa_parse needs from the start of the file: the 8-byte file header
// plus the 8-byte first-frame header.
#define QOA_HEADER_WINDOW 16u

// Largest compressed frame the player accepts (1-2 channels, 256 slices):
// 8-byte frame header + 16 bytes of LMS state per channel + 8 bytes per
// slice per channel.
#define QOA_MAX_FRAME_BYTES (8u + 16u * 2u + 8u * 256u * 2u)  // 4136

// Largest PCM output of one frame: 256 slices * 20 samples, 2 channels, s16.
#define QOA_MAX_FRAME_PCM_BYTES (256u * 20u * 2u * 2u)  // 20480

typedef struct {
    uint8_t  channels;           // 1 or 2
    uint32_t sample_rate;        // Hz
    uint32_t samples;            // content frames per channel, whole file
    uint32_t frame_samples;      // content frames per channel in a full frame
                                 // (the last frame may be shorter)
    uint32_t frame_size;         // bytes of a full frame, header included
    uint32_t first_frame_offset; // file offset of the first frame (8)
} qoa_info_t;

typedef enum {
    QOA_OK = 0,
    QOA_ERR_NOT_QOA,      // bad magic, zero samples, or a truncated header
    QOA_ERR_UNSUPPORTED,  // more than 2 channels
    QOA_ERR_BAD_FRAME,    // a frame header that disagrees with the file
    QOA_ERR_TRUNCATED,    // a frame that does not fit the buffer
} qoa_err_t;

qoa_err_t qoa_parse(const uint8_t *buf, size_t len, qoa_info_t *out);

const char *qoa_strerror(qoa_err_t err);

// File offset of the frame holding content frame `index`.  Every frame but
// the last is full, so this is exact.
uint32_t qoa_frame_offset(const qoa_info_t *info, uint32_t index);

// Decode the frame at buf (len bytes from the frame's start) into out, which
// must hold QOA_MAX_FRAME_PCM_BYTES.  Returns content frames per channel
// (frame_samples, or fewer for the last frame), 0 on a bad or truncated
// frame.
uint32_t qoa_frame_decode(const qoa_info_t *info, const uint8_t *buf,
                          size_t len, int16_t *out);
