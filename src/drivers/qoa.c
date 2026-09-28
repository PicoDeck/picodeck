#include "qoa.h"

#include <string.h>

#include "pico/stdlib.h"    // pico.h first on the device (sound.c does the same)
#include "pico/platform.h"  // __time_critical_func, __not_in_flash

// Big-endian header fields.
static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

qoa_err_t qoa_parse(const uint8_t *buf, size_t len, qoa_info_t *out) {
    if (!buf || !out || len < QOA_HEADER_WINDOW)
        return QOA_ERR_NOT_QOA;

    // File header: "qoaf" + total samples per channel (0 = a streaming file
    // of unknown length, which the player cannot seek or loop: refused, as
    // the reference decoder does).  The first frame header gives channels
    // and sample rate.
    uint32_t samples = be32(buf + 4);
    uint32_t channels = buf[8];
    uint32_t rate = ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 8) | buf[11];
    if (memcmp(buf, "qoaf", 4) != 0 || samples == 0 || channels == 0 ||
        rate == 0)
        return QOA_ERR_NOT_QOA;
    if (channels > 2 || rate > 192000)  // wav.c's limit
        return QOA_ERR_UNSUPPORTED;

    // First-frame header: fsamples and fsize, so the player knows a full
    // frame's geometry without reading past the header window.
    const uint8_t *fh = buf + 8;  // first frame follows the file header
    uint32_t fsamples = be16(fh + 4);
    uint32_t fsize = be16(fh + 6);
    if (fsamples == 0 || fsamples > 256u * 20u)
        return QOA_ERR_BAD_FRAME;
    uint32_t slices = (fsamples + 19u) / 20u;
    if (fsize != 8u + 16u * channels + 8u * slices * channels)
        return QOA_ERR_BAD_FRAME;
    if (fsize > QOA_MAX_FRAME_BYTES)
        return QOA_ERR_BAD_FRAME;

    out->channels = (uint8_t)channels;
    out->sample_rate = rate;
    out->samples = samples;
    out->frame_samples = fsamples;
    out->frame_size = fsize;
    out->first_frame_offset = 8;
    return QOA_OK;
}

const char *qoa_strerror(qoa_err_t err) {
    switch (err) {
    case QOA_OK:             return "ok";
    case QOA_ERR_NOT_QOA:    return "not a QOA file";
    case QOA_ERR_UNSUPPORTED:return "unsupported QOA file (more than 2 channels or 192 kHz)";
    case QOA_ERR_BAD_FRAME:  return "bad QOA frame header";
    case QOA_ERR_TRUNCATED:  return "truncated QOA frame";
    }
    return "unknown QOA error";
}

uint32_t qoa_frame_offset(const qoa_info_t *info, uint32_t index) {
    return info->first_frame_offset + (index / info->frame_samples) * info->frame_size;
}

uint32_t qoa_frame_bytes(const uint8_t *buf, size_t len) {
    if (!buf || len < 8)
        return 0;
    return be16(buf + 6);
}

// ── Streaming decode ────────────────────────────────────────────────────────
// The reference decoder's arithmetic (third_party/qoa, with PicoDeck patch 2:
// 64-bit prediction, clamped), a slice at a time with the LMS state in
// registers.  It runs on Core 1 from RAM, table included: from flash it
// would contend with Core 0 for the XIP cache (sound.c's mixer measured
// 5x slower there under a gfx3d game).

// qoa_dequant_tab: the scalefactor's residual for each 3-bit code.
static const int16_t __not_in_flash("qoa") k_dequant[16][8] = {
	{   1,    -1,    3,    -3,    5,    -5,     7,     -7},
	{   5,    -5,   18,   -18,   32,   -32,    49,    -49},
	{  16,   -16,   53,   -53,   95,   -95,   147,   -147},
	{  34,   -34,  113,  -113,  203,  -203,   315,   -315},
	{  63,   -63,  210,  -210,  378,  -378,   588,   -588},
	{ 104,  -104,  345,  -345,  621,  -621,   966,   -966},
	{ 158,  -158,  528,  -528,  950,  -950,  1477,  -1477},
	{ 228,  -228,  760,  -760, 1368, -1368,  2128,  -2128},
	{ 316,  -316, 1053, -1053, 1895, -1895,  2947,  -2947},
	{ 422,  -422, 1405, -1405, 2529, -2529,  3934,  -3934},
	{ 548,  -548, 1828, -1828, 3290, -3290,  5117,  -5117},
	{ 696,  -696, 2320, -2320, 4176, -4176,  6496,  -6496},
	{ 868,  -868, 2893, -2893, 5207, -5207,  8099,  -8099},
	{1064, -1064, 3548, -3548, 6386, -6386,  9933,  -9933},
	{1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
	{1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

uint32_t qoa_dec_begin(qoa_dec_t *d, const qoa_info_t *info,
                       const uint8_t *frame, size_t len) {
    if (!d || !info || !frame || info->channels < 1 || info->channels > 2)
        return 0;
    uint32_t ch = info->channels;
    uint32_t head = 8u + 16u * ch;
    if (len < head)
        return 0;
    uint32_t rate = ((uint32_t)frame[1] << 16) | ((uint32_t)frame[2] << 8) | frame[3];
    uint32_t samples = be16(frame + 4);
    uint32_t fsize = be16(frame + 6);
    if (frame[0] != ch || rate != info->sample_rate || samples == 0 ||
        fsize > len)
        return 0;
    // The slices the samples need must lie inside the frame.
    if (head + (samples + 19u) / 20u * ch * 8u > fsize)
        return 0;
    const uint8_t *p = frame + 8;
    for (uint32_t c = 0; c < ch; c++, p += 16) {
        for (int i = 0; i < 4; i++) {
            d->history[c][i] = (int16_t)be16(p + 2 * i);
            d->weights[c][i] = (int16_t)be16(p + 8 + 2 * i);
        }
    }
    d->samples = samples;
    d->pos = 0;
    d->off = head;
    d->channels = (uint8_t)ch;
    return samples;
}

static inline int32_t clamp_s16(int32_t v) {
    if ((uint32_t)(v + 32768) > 65535u)
        return v < -32768 ? -32768 : 32767;
    return v;
}

uint32_t __time_critical_func(qoa_dec_run)(qoa_dec_t *d, const uint8_t *frame,
                                           int16_t *out, uint32_t max_frames) {
    uint32_t ch = d->channels, n = 0;
    while (d->pos < d->samples) {
        uint32_t len = d->samples - d->pos;
        if (len > 20)
            len = 20;
        if (n + len > max_frames)
            break;
        for (uint32_t c = 0; c < ch; c++) {
            const uint8_t *s = frame + d->off;
            d->off += 8;
            uint64_t slice = ((uint64_t)be32(s) << 32) | be32(s + 4);
            const int16_t *dq = k_dequant[slice >> 60];
            slice <<= 4;
            int32_t h0 = d->history[c][0], h1 = d->history[c][1];
            int32_t h2 = d->history[c][2], h3 = d->history[c][3];
            int32_t w0 = d->weights[c][0], w1 = d->weights[c][1];
            int32_t w2 = d->weights[c][2], w3 = d->weights[c][3];
            int16_t *o = out + n * ch + c;
            for (uint32_t i = 0; i < len; i++) {
                int64_t pred = ((int64_t)w0 * h0 + (int64_t)w1 * h1 +
                                (int64_t)w2 * h2 + (int64_t)w3 * h3) >> 13;
                if (pred > 0x3fffffff) pred = 0x3fffffff;
                if (pred < -0x40000000) pred = -0x40000000;
                int32_t deq = dq[slice >> 61];
                slice <<= 3;
                int32_t rec = clamp_s16((int32_t)pred + deq);
                *o = (int16_t)rec;
                o += ch;
                int32_t delta = deq >> 4;
                w0 += h0 < 0 ? -delta : delta;
                w1 += h1 < 0 ? -delta : delta;
                w2 += h2 < 0 ? -delta : delta;
                w3 += h3 < 0 ? -delta : delta;
                h0 = h1;
                h1 = h2;
                h2 = h3;
                h3 = rec;
            }
            d->history[c][0] = h0; d->history[c][1] = h1;
            d->history[c][2] = h2; d->history[c][3] = h3;
            d->weights[c][0] = w0; d->weights[c][1] = w1;
            d->weights[c][2] = w2; d->weights[c][3] = w3;
        }
        d->pos += len;
        n += len;
    }
    return n;
}
