// Host unit test for libmad's granule-at-a-time decoding (issue #28):
// mad_frame_decode_begin / _granule / _end, each granule synthesised
// (mad_synth_granule) before the next is decoded, give the PCM libmad gave
// before the split, sample for sample: whole frames (mad_frame_decode),
// then mad_synth_frame into a 1152-sample buffer. The fixtures' FNV-1a
// hashes below were taken from that code (git show ecedf34:third_party/
// libmad), built as libmad_host is. mp3_player.c decodes a 44.1 kHz stereo
// frame in two halves so that each fits an idle window of a paced app's
// Core 0, and the frame's subband samples need room for one granule.
//
// Fixtures: chord128.mp3 (44.1 kHz stereo MPEG-1, two granules a frame),
// chord.mp3 (the same at 96 kbps, as an AVI holds it) and chord22m.mp3
// (22.05 kHz mono MPEG-2, one). An MP3 named on the command line is
// decoded too (real music: short blocks, joint stereo), its hash printed.
#include "check.h"
#define FPM_DEFAULT  // as mp3_player.c (the host libmad is built FPM_64BIT)
#include "mad.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define FIX PICODECK_ROOT "/tests/unit/fixtures/mp3/"

typedef struct {
  int16_t *pcm;      // interleaved by channel
  size_t len, cap;   // samples (per channel x channels)
  unsigned frames, errors, channels;
} out_t;

static void out_push(out_t *o, const struct mad_pcm *pcm) {
  size_t n = (size_t)pcm->length * pcm->channels;
  if (o->len + n > o->cap) {
    o->cap = (o->len + n) * 2;
    o->pcm = realloc(o->pcm, o->cap * sizeof(int16_t));
    if (!o->pcm) exit(1);
  }
  for (unsigned i = 0; i < pcm->length; i++)
    for (unsigned ch = 0; ch < pcm->channels; ch++)
      o->pcm[o->len++] = pcm->samplesX[i][ch];
  o->channels = pcm->channels;
}

static uint8_t *load(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = calloc(1, (size_t)n + MAD_BUFFER_GUARD);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(1);
  fclose(f);
  *len = (size_t)n;
  return b;
}

typedef struct {
  struct mad_stream stream;
  struct mad_frame frame;
  struct mad_frame_mem frame_mem;
  struct mad_synth synth;
} dec_t;

static dec_t *dec_new(const uint8_t *data, size_t len) {
  dec_t *d = calloc(1, sizeof *d);
  if (!d) exit(1);
  mad_stream_init(&d->stream);
  mad_frame_bind(&d->frame, &d->frame_mem);
  mad_frame_init(&d->frame);
  mad_synth_init(&d->synth);
  mad_stream_buffer(&d->stream, data, len + MAD_BUFFER_GUARD);
  return d;
}

// As mp3_player.c drives it: each granule decoded, synthesised and taken
// before the next one is decoded.
static void decode_split(const uint8_t *data, size_t len, out_t *o) {
  dec_t *d = dec_new(data, len);
  for (;;) {
    if (mad_frame_decode_begin(&d->frame, &d->stream) != 0) {
      if (d->stream.error == MAD_ERROR_BUFLEN) break;
      o->errors++;
      if (!MAD_RECOVERABLE(d->stream.error)) break;
      continue;
    }
    unsigned ngr = mad_frame_granules(&d->frame);
    bool ok = true;
    for (unsigned gr = 0; gr < ngr && ok; gr++) {
      ok = mad_frame_decode_granule(&d->frame, &d->stream, gr) == 0;
      if (ok) {
        mad_synth_granule(&d->synth, &d->frame, gr);
        out_push(o, &d->synth.pcm);
      }
    }
    if (mad_frame_decode_end(&d->frame, &d->stream) != 0) {
      o->errors++;
      continue;
    }
    o->frames++;
  }
  free(d);
}

static uint32_t fnv1a(const out_t *o) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < o->len; i++) {
    uint16_t v = (uint16_t)o->pcm[i];
    h = (h ^ (v & 0xffu)) * 16777619u;
    h = (h ^ (v >> 8)) * 16777619u;
  }
  return h;
}

static void check_file(const char *path, unsigned want_ch, uint32_t want_fnv) {
  size_t len = 0;
  uint8_t *data = load(path, &len);
  if (!data) return;
  out_t split = {0};
  decode_split(data, len, &split);
  printf("%s: %u frames, %u errors, %zu samples, %u ch, fnv 0x%08x\n", path,
         split.frames, split.errors, split.len, split.channels, fnv1a(&split));
  CHECK(split.frames > 10);
  if (want_ch) CHECK_EQ_INT(split.channels, want_ch);
  // Not silence: the chord is there.
  int16_t peak = 0;
  for (size_t i = 0; i < split.len; i++) {
    int16_t v = split.pcm[i] < 0 ? (int16_t)-split.pcm[i] : split.pcm[i];
    if (v > peak) peak = v;
  }
  CHECK(peak > 1000);
  if (want_fnv)
    CHECK_EQ_U32(fnv1a(&split), want_fnv);
  free(split.pcm);
  free(data);
}

int main(int argc, char **argv) {
  check_file(FIX "chord128.mp3", 2, 0x1418bb19u);
  check_file(FIX "chord.mp3", 2, 0xb5f64579u);
  check_file(FIX "chord22m.mp3", 1, 0x2e5f0c70u);
  for (int i = 1; i < argc; i++)
    check_file(argv[i], 0, 0);
  return check_report("test_mp3_granule");
}
