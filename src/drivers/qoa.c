#include "qoa.h"

#include <string.h>

#include "qoa/qoa.h"

// Big-endian read of a frame-header field (the vendored qoa_decode_header
// peeks only channels/rate; the player also needs fsamples/fsize).
static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

qoa_err_t qoa_parse(const uint8_t *buf, size_t len, qoa_info_t *out) {
    if (!buf || !out || len < QOA_HEADER_WINDOW)
        return QOA_ERR_NOT_QOA;

    qoa_desc desc;
    memset(&desc, 0, sizeof(desc));
    if (!qoa_decode_header(buf, (int)len, &desc))
        return QOA_ERR_NOT_QOA;
    if (desc.channels > 2)
        return QOA_ERR_UNSUPPORTED;

    // First-frame header: fsamples and fsize, so the player knows a full
    // frame's geometry without reading past the header window.
    const uint8_t *fh = buf + 8;  // first frame follows the file header
    uint32_t fsamples = be16(fh + 4);
    uint32_t fsize = be16(fh + 6);
    if (fsamples == 0 || fsamples > 256u * 20u)
        return QOA_ERR_BAD_FRAME;
    uint32_t slices = (fsamples + 19u) / 20u;
    if (fsize != 8u + 16u * desc.channels + 8u * slices * desc.channels)
        return QOA_ERR_BAD_FRAME;
    if (fsize > QOA_MAX_FRAME_BYTES)
        return QOA_ERR_BAD_FRAME;

    out->channels = (uint8_t)desc.channels;
    out->sample_rate = desc.samplerate;
    out->samples = desc.samples;
    out->frame_samples = fsamples;
    out->frame_size = fsize;
    out->first_frame_offset = 8;
    return QOA_OK;
}

const char *qoa_strerror(qoa_err_t err) {
    switch (err) {
    case QOA_OK:             return "ok";
    case QOA_ERR_NOT_QOA:    return "not a QOA file";
    case QOA_ERR_UNSUPPORTED:return "unsupported QOA file (more than 2 channels)";
    case QOA_ERR_BAD_FRAME:  return "bad QOA frame header";
    case QOA_ERR_TRUNCATED:  return "truncated QOA frame";
    }
    return "unknown QOA error";
}

uint32_t qoa_frame_offset(const qoa_info_t *info, uint32_t index) {
    return info->first_frame_offset + (index / info->frame_samples) * info->frame_size;
}

uint32_t qoa_frame_decode(const qoa_info_t *info, const uint8_t *buf,
                          size_t len, int16_t *out) {
    if (!info || !buf || !out)
        return 0;
    qoa_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.channels = info->channels;
    desc.samplerate = info->sample_rate;
    desc.samples = info->samples;
    unsigned frame_len = 0;
    if (!qoa_decode_frame(buf, (unsigned)len, &desc, (short *)out, &frame_len))
        return 0;
    return frame_len;
}
