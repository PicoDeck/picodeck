// Host unit tests for src/drivers/qoa.c (QOA probing + streaming decode for
// the file player).  Fixtures are encoded in-process with the vendored
// reference encoder (third_party/qoa, built here without QOA_NO_ENCODER);
// QOA decode is deterministic, so the streaming decoder must match the
// reference decoder sample for sample.
#include "check.h"
#include "qoa.h"
#include "qoa/qoa.h"

#include <stdlib.h>
#include <string.h>

// A deterministic multi-tone signal, loud enough to exercise scalefactors.
static int16_t signal(uint32_t i, uint8_t ch) {
  int v = (int)((i * 37 + ch * 7919) % 4096) - 2048;       // ramp
  v += (int)((i * i / 7 + ch * 13) % 512) - 256;           // slow wobble
  v *= 8;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return (int16_t)v;
}

static int16_t *make_pcm(uint32_t frames, uint8_t channels) {
  int16_t *pcm = malloc((size_t)frames * channels * 2);
  for (uint32_t i = 0; i < frames; i++)
    for (uint8_t c = 0; c < channels; c++)
      pcm[i * channels + c] = signal(i, c);
  return pcm;
}

static uint8_t *encode(const int16_t *pcm, uint32_t frames, uint32_t rate,
                       uint8_t channels, unsigned *out_len) {
  qoa_desc desc;
  memset(&desc, 0, sizeof(desc));
  desc.channels = channels;
  desc.samplerate = rate;
  desc.samples = frames;
  return qoa_encode((const short *)pcm, &desc, out_len);
}

static void test_parse_roundtrip(void) {
  // 12000 content frames: two full 5120-sample frames + a 2560 tail.
  uint32_t frames = 12000;
  int16_t *pcm = make_pcm(frames, 2);
  unsigned len;
  uint8_t *q = encode(pcm, frames, 44100, 2, &len);
  CHECK(q != NULL);

  qoa_info_t info;
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_OK);
  CHECK_EQ_U32(info.channels, 2);
  CHECK_EQ_U32(info.sample_rate, 44100);
  CHECK_EQ_U32(info.samples, 12000);
  CHECK_EQ_U32(info.frame_samples, 5120);
  CHECK_EQ_U32(info.frame_size, 8 + 16 * 2 + 8 * 256 * 2);
  CHECK_EQ_U32(info.first_frame_offset, 8);
  // The whole file is consistent with the frame geometry (tail: 1760
  // samples = 88 slices per channel).
  CHECK_EQ_U32(len, 8 + 2 * info.frame_size + (8 + 16 * 2 + 8 * 88 * 2));
  free(q);
  free(pcm);
}

static void test_parse_mono(void) {
  uint32_t frames = 3000;  // one short frame
  int16_t *pcm = make_pcm(frames, 1);
  unsigned len;
  uint8_t *q = encode(pcm, frames, 22050, 1, &len);
  CHECK(q != NULL);
  qoa_info_t info;
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_OK);
  CHECK_EQ_U32(info.channels, 1);
  CHECK_EQ_U32(info.sample_rate, 22050);
  CHECK_EQ_U32(info.samples, 3000);
  // A single partial frame: frame_samples/fsz come from that frame's header.
  CHECK_EQ_U32(info.frame_samples, 3000);
  CHECK_EQ_U32(info.frame_size, 8 + 16 + 8 * 150);
  free(q);
  free(pcm);
}

static void test_malformed(void) {
  qoa_info_t info;
  CHECK_EQ_INT(qoa_parse(NULL, 64, &info), QOA_ERR_NOT_QOA);
  CHECK_EQ_INT(qoa_parse((const uint8_t *)"qoaf", 4, &info), QOA_ERR_NOT_QOA);

  uint32_t frames = 100;
  int16_t *pcm = make_pcm(frames, 1);
  unsigned len;
  uint8_t *q = encode(pcm, frames, 22050, 1, &len);
  CHECK(q != NULL);

  q[0] = 'X';  // bad magic
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_ERR_NOT_QOA);
  q[0] = 'q';

  memset(q + 4, 0, 4);  // zero samples
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_ERR_NOT_QOA);
  q[7] = (uint8_t)frames;

  q[8] = 0;  // no channels
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_ERR_NOT_QOA);
  q[8] = 1;
  memset(q + 9, 0xff, 3);  // 16.7 MHz: past what the stream accepts (as WAV)
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_ERR_UNSUPPORTED);

  // Three channels: a valid QOA file the player cannot stream.
  free(q);
  int16_t *pcm3 = make_pcm(frames, 3);
  q = encode(pcm3, frames, 22050, 3, &len);
  CHECK(q != NULL);
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_ERR_UNSUPPORTED);
  free(q);
  free(pcm3);
  free(pcm);
}

static void test_frame_offset(void) {
  uint32_t frames = 12000;
  int16_t *pcm = make_pcm(frames, 2);
  unsigned len;
  uint8_t *q = encode(pcm, frames, 44100, 2, &len);
  CHECK(q != NULL);
  qoa_info_t info;
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_OK);

  CHECK_EQ_U32(qoa_frame_offset(&info, 0), 8);
  CHECK_EQ_U32(qoa_frame_offset(&info, 5119), 8);
  CHECK_EQ_U32(qoa_frame_offset(&info, 5120), 8 + info.frame_size);
  CHECK_EQ_U32(qoa_frame_offset(&info, 11999), 8 + 2 * info.frame_size);
  free(q);
  free(pcm);
}

// A frame's own size field: the next frame starts that many bytes on (the
// last frame is shorter than a full one).
static void test_frame_bytes(void) {
  uint32_t frames = 6000;  // one full frame + an 880-sample tail frame
  int16_t *pcm = make_pcm(frames, 1);
  unsigned len;
  uint8_t *q = encode(pcm, frames, 22050, 1, &len);
  CHECK(q != NULL);
  qoa_info_t info;
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_OK);
  CHECK_EQ_U32(qoa_frame_bytes(q + 8, len - 8), info.frame_size);
  uint32_t tail = 8 + info.frame_size;
  CHECK_EQ_U32(qoa_frame_bytes(q + tail, len - tail), len - tail);
  CHECK_EQ_U32(len - tail, 8u + 16u + 8u * 44u);  // 880 samples = 44 slices
  CHECK_EQ_U32(qoa_frame_bytes(q + 8, 7), 0);     // no whole header
  free(q);
  free(pcm);
}

// The streaming decoder (a few slices per call, as Core 1 decodes only what
// the stream ring can take each tick) matches the reference decoder sample
// for sample at any chunk size, through full frames and a short tail frame
// that ends mid-slice.
static void check_stream(uint32_t frames, uint8_t ch, uint32_t rate,
                         uint32_t chunk) {
  int16_t *pcm = make_pcm(frames, ch);
  unsigned len;
  uint8_t *q = encode(pcm, frames, rate, ch, &len);
  CHECK(q != NULL);
  qoa_info_t info;
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_OK);
  qoa_desc desc;
  short *ref = qoa_decode(q, (int)len, &desc);
  CHECK(ref != NULL);
  int16_t *out = malloc((size_t)chunk * ch * 2);
  uint32_t done = 0, off = info.first_frame_offset;
  int mismatched = 0;
  while (done < frames) {
    qoa_dec_t d;
    uint32_t n = qoa_dec_begin(&d, &info, q + off, len - off);
    CHECK(n > 0);
    if (n == 0)
      break;
    uint32_t got = 0, k;
    while ((k = qoa_dec_run(&d, q + off, out, chunk)) > 0) {
      CHECK(k <= chunk);
      if (memcmp(out, ref + (size_t)(done + got) * ch, (size_t)k * ch * 2) != 0)
        mismatched++;
      got += k;
    }
    CHECK_EQ_U32(got, n);
    done += n;
    off += qoa_frame_bytes(q + off, len - off);
  }
  CHECK_EQ_U32(done, frames);
  CHECK_EQ_INT(mismatched, 0);
  CHECK_EQ_U32(off, len);
  free(out);
  free(ref);
  free(q);
  free(pcm);
}

static void test_stream_matches_reference(void) {
  check_stream(6010, 1, 22050, 20);     // tail frame ends 10 into a slice
  check_stream(6010, 1, 22050, 128);
  check_stream(12000, 2, 44100, 57);    // whole slices only: 40 at a time
  check_stream(12000, 2, 44100, 5120);  // a whole frame per call
}

// Whole slices only: a buffer smaller than a slice makes no progress, and the
// next call with room carries on where the frame stands.
static void test_stream_needs_a_slice_of_room(void) {
  int16_t *pcm = make_pcm(100, 1);
  unsigned len;
  uint8_t *q = encode(pcm, 100, 22050, 1, &len);
  qoa_info_t info;
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_OK);
  qoa_dec_t d;
  int16_t out[40];
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, q + 8, len - 8), 100);
  CHECK_EQ_U32(qoa_dec_run(&d, q + 8, out, 19), 0);
  CHECK_EQ_U32(qoa_dec_run(&d, q + 8, out, 40), 40);
  CHECK_EQ_U32(qoa_dec_run(&d, q + 8, out, 40), 40);
  CHECK_EQ_U32(qoa_dec_run(&d, q + 8, out, 40), 20);
  CHECK_EQ_U32(qoa_dec_run(&d, q + 8, out, 40), 0);  // the frame is done
  free(q);
  free(pcm);
}

// A frame whose header disagrees with the file, or that the buffer does not
// hold, is refused before any sample is decoded.
static void test_stream_bad_frames(void) {
  uint32_t frames = 6000;
  int16_t *pcm = make_pcm(frames, 2);
  unsigned len;
  uint8_t *q = encode(pcm, frames, 44100, 2, &len);
  qoa_info_t info;
  CHECK_EQ_INT(qoa_parse(q, len, &info), QOA_OK);
  qoa_dec_t d;
  uint8_t *f = q + 8;
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, f, info.frame_size - 1), 0);  // truncated
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, f, 4), 0);
  f[0] = 1;                                                            // channels
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, f, info.frame_size), 0);
  f[0] = 2;
  f[3] ^= 1;                                                           // sample rate
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, f, info.frame_size), 0);
  f[3] ^= 1;
  f[4] = 0x7f;                                                         // more samples
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, f, info.frame_size), 0);       // than slices
  f[4] = 0;
  f[5] = 0;                                                            // no samples
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, f, info.frame_size), 0);
  f[4] = (uint8_t)(info.frame_samples >> 8);
  f[5] = (uint8_t)(info.frame_samples & 0xff);
  f[6] = 0xff;                                                         // frame size
  f[7] = 0xff;
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, f, len - 8), 0);
  f[6] = (uint8_t)(info.frame_size >> 8);
  f[7] = (uint8_t)(info.frame_size & 0xff);
  CHECK_EQ_U32(qoa_dec_begin(&d, &info, f, info.frame_size), info.frame_samples);
  free(q);
  free(pcm);
}

int main(void) {
  test_stream_matches_reference();
  test_stream_needs_a_slice_of_room();
  test_stream_bad_frames();
  test_frame_bytes();
  test_parse_roundtrip();
  test_parse_mono();
  test_malformed();
  test_frame_offset();
  for (int e = QOA_OK; e <= QOA_ERR_TRUNCATED; e++)
    CHECK(qoa_strerror((qoa_err_t)e)[0] != '\0');
  return check_report("test_qoa");
}
