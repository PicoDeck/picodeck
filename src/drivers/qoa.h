#pragma once

// QOA ("Quite OK Audio") probing and streaming decode for the file player.
// Pure: caller-supplied buffers, no IO.  The decoder reimplements the
// reference (third_party/qoa, MIT) a few slices at a time, so Core 1 decodes
// only what the stream ring can take each tick; host-tested sample for
// sample against the reference in tests/unit/test_qoa.c (which also encodes
// its fixtures with it) and fuzzed by tests/fuzz/fuzz_qoa.c.  The firmware
// does not build the reference.
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
    QOA_ERR_UNSUPPORTED,  // more than 2 channels, or above 192 kHz
    QOA_ERR_BAD_FRAME,    // a frame header that disagrees with the file
    QOA_ERR_TRUNCATED,    // a frame that does not fit the buffer
} qoa_err_t;

qoa_err_t qoa_parse(const uint8_t *buf, size_t len, qoa_info_t *out);

const char *qoa_strerror(qoa_err_t err);

// File offset of the frame holding content frame `index`.  Every frame but
// the last is full, so this is exact.
uint32_t qoa_frame_offset(const qoa_info_t *info, uint32_t index);

// The size field of the frame header at buf (len bytes available): where
// the next frame starts.  0 when len is shorter than a frame header.
uint32_t qoa_frame_bytes(const uint8_t *buf, size_t len);

// Streaming decode of one frame, a few slices per call: Core 1 decodes only
// what the stream ring can take each tick, into a small buffer, so no tick
// decodes a whole frame and no whole-frame PCM scratch is needed.  The LMS
// state lives here between calls; the frame's bytes stay with the caller.
typedef struct {
    int32_t  history[2][4];
    int32_t  weights[2][4];
    uint32_t samples;    // content frames per channel in this frame
    uint32_t pos;        // content frames decoded so far
    uint32_t off;        // byte offset of the next slice group in the frame
    uint8_t  channels;
} qoa_dec_t;

// Start decoding the frame at frame (len bytes available): check its header
// against info (channels, rate, a size that fits len and holds its samples)
// and load its LMS state.  Returns its content frames per channel, 0 for a
// bad or truncated frame.
uint32_t qoa_dec_begin(qoa_dec_t *d, const qoa_info_t *info,
                       const uint8_t *frame, size_t len);

// Decode the next whole slices of the frame that fit in max_frames content
// frames into out (interleaved 16-bit).  Returns the frames decoded: 0 at
// the end of the frame, or when max_frames holds no whole slice.
uint32_t qoa_dec_run(qoa_dec_t *d, const uint8_t *frame, int16_t *out,
                     uint32_t max_frames);

