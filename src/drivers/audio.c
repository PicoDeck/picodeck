#include "audio.h"
#include "sound.h"
#include "audio_ring.h"
#include "../hardware.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#include <stdio.h>

// Alarm pool created on Core 1 — all audio timer ISRs fire on Core 1,
// keeping Core 0 free for the app/game loop.
static alarm_pool_t *s_core1_alarm_pool = NULL;

// --- Tone generation (square wave via PWM frequency modulation) -------------
#define MIN_FREQ 20
#define MAX_FREQ 20000
#define TONE_PWM_WRAP 255

static unsigned int s_pwm_slice_l = 0;
static unsigned int s_pwm_slice_r = 0;
static uint8_t s_volume = 100;
static uint32_t s_volume_scale = 256; // 256 = 100%, precomputed for fast scaling
static bool s_playing = false;

static uint64_t s_end_time_us = 0;
static uint32_t s_tone_freq = 440;   // square synth frequency (mixed into stream)
static uint32_t s_tone_phase = 0;    // synth phase accumulator
static uint8_t  s_tone_level = 0;    // synth amplitude (0..128 → ±(level<<7))

void audio_init(void) {
  gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

  s_pwm_slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
  s_pwm_slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, TONE_PWM_WRAP);
  pwm_init(s_pwm_slice_l, &cfg, false);
  pwm_init(s_pwm_slice_r, &cfg, false);

  audio_set_volume(100);
}

void audio_core1_init(void) {
  // Hardware alarm 2 (default pool uses 3). 4 slots covers all audio
  // timers: tone, sound sample, fileplayer.
  s_core1_alarm_pool = alarm_pool_create(2, 4);
  if (!s_core1_alarm_pool) {
    printf("[AUDIO] WARNING: failed to create Core 1 alarm pool\n");
  }
}

alarm_pool_t *audio_get_core1_alarm_pool(void) {
  return s_core1_alarm_pool;
}

/* audio_pwm_setup() was removed with the mixer refactor: nothing drives
 * the PWM slices directly anymore — tone, samples and the PCM stream are
 * all mixed into the single 44.1 kHz stream path. */

// Logarithmic volume curve: lut[i] = round((10^(i/100) - 1) / 9 * 128), i=0..100
// Replaces runtime exp()/log() with a compile-time table (~5 cycles vs ~100+).
// Values are 0..128 (half of TONE_PWM_WRAP+1=256), matching max_level = (TONE_PWM_WRAP+1)/2.
static const uint8_t s_log_volume_lut[101] = {
    0,   0,   1,   1,   1,   2,   2,   2,   3,   3,   //  0-  9
    4,   4,   5,   5,   5,   6,   6,   7,   7,   8,   // 10- 19
    8,   9,   9,  10,  10,  11,  12,  12,  13,  14,   // 20- 29
   14,  15,  15,  16,  17,  18,  18,  19,  20,  21,   // 30- 39
   22,  22,  23,  24,  25,  26,  27,  28,  29,  30,   // 40- 49
   31,  32,  33,  34,  35,  36,  37,  39,  40,  41,   // 50- 59
   42,  44,  45,  46,  48,  49,  51,  52,  54,  55,   // 60- 69
   57,  59,  60,  62,  64,  66,  68,  70,  71,  73,   // 70- 79
   76,  78,  80,  82,  84,  86,  89,  91,  94,  96,   // 80- 89
   99, 101, 104, 107, 110, 113, 115, 119, 122, 125,   // 90- 99
  128                                                   // 100
};

static void audio_apply_volume(void) {
  if (s_volume == 0) {
    s_tone_level = 0;
  } else {
    s_tone_level = (uint8_t)s_log_volume_lut[s_volume];
  }
}

void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
  if (freq_hz < MIN_FREQ)
    freq_hz = MIN_FREQ;
  if (freq_hz > MAX_FREQ)
    freq_hz = MAX_FREQ;

  s_tone_freq = freq_hz;
  s_tone_phase = 0;
  audio_apply_volume();

  s_playing = true;
  s_end_time_us = duration_ms > 0 ? time_us_64() + (duration_ms * 1000) : 0;

  // Tones are mixed into the PCM stream by the DMA refill hook — no
  // exclusive PWM takeover, no timer. Stream must be running.
  audio_stream_ensure_running();
}

void audio_stop_tone(void) {
  s_end_time_us = 0;
  s_playing = false;
  s_tone_level = 0;
}

void audio_set_volume(uint8_t volume) {
  if (volume > 100)
    volume = 100;
  s_volume = volume;
  s_volume_scale = (uint32_t)volume * 256 / 100;
  audio_apply_volume();
}

// --- PCM sample streaming via DMA paced by PWM DREQ -------------------------
//
// Replaces the old timer-based approach (one ISR per sample = 22k ISR/sec)
// with hardware-paced DMA (one ISR per 256-sample buffer = ~85 ISR/sec).
// The DMA controller autonomously transfers samples to the PWM CC register
// at the exact PWM cycle rate — zero jitter, zero CPU involvement per sample.

#define STREAM_PWM_WRAP   1699
#define STREAM_PWM_MID    ((STREAM_PWM_WRAP + 1) / 2)  // 850
#define STREAM_DMA_SAMPLES 128

// AUDIO_OUT_RATE is defined in audio.h (shared with sound.c's mixer).

// PCM stream ring (audio_ring.h): 4096 frames, uint8 per channel.
static audio_ring_t s_ring;
static bool s_streaming = false;
static uint32_t s_stream_content_rate = AUDIO_OUT_RATE;  // rate of ring data

static int          s_stream_dma_chan = -1;
static uint32_t     s_stream_dma_buf[2][STREAM_DMA_SAMPLES];
static volatile int s_stream_dma_active_buf = 0;
static volatile bool s_stream_dma_active = false;
static volatile bool s_stream_dma_start_pending = false;
static bool          s_stream_irq_on_core1 = false;
static unsigned int  s_stream_pwm_slice = 0;

static volatile uint32_t s_stream_dma_isr_count = 0;
static volatile uint32_t s_stream_underrun_count = 0;

// Fill one DMA buffer from the ring buffer (called from DMA ISR on Core 1)
// This is THE mixer: PCM stream (ring) + sound.c sample players + tone synth
// are summed per frame, so all three play simultaneously through one path.
// Mix chunks of 32 frames to keep SRAM buffers small (RAM is ~full).
#define MIX_CHUNK 32
static int32_t s_mix_l[MIX_CHUNK];
static int32_t s_mix_r[MIX_CHUNK];

static void __time_critical_func(audio_fill_dma_buffer)(uint32_t *buf, int count) {
  uint32_t vol = s_volume_scale;

  // Tone end-time check once per buffer (ISR-safe read)
  if (s_playing && s_end_time_us > 0 && time_us_64() >= s_end_time_us) {
    s_playing = false;
    s_tone_level = 0;
  }
  uint32_t tone_phase = s_tone_phase;

  for (int base_i = 0; base_i < count; base_i += MIX_CHUNK) {
    int chunk = count - base_i < MIX_CHUNK ? count - base_i : MIX_CHUNK;

    // Sample players (sound.c) — zero-fills when nothing is playing
    sound_mixer_process(s_mix_l, s_mix_r, chunk);

    for (int i = 0; i < chunk; i++) {
      int32_t ml, mr;
      if (!audio_ring_pop(&s_ring, s_stream_content_rate, AUDIO_OUT_RATE,
                          &ml, &mr)) {
        // Stream underrun: base is silent (samples/tone may still sound)
        s_stream_underrun_count++;
      }

      // + sound.c sample players
      ml += s_mix_l[i];
      mr += s_mix_r[i];

      // + tone (square synth at the configured frequency)
      if (s_playing) {
        tone_phase += s_tone_freq;
        if (tone_phase >= AUDIO_OUT_RATE) tone_phase -= AUDIO_OUT_RATE;
        int32_t t = (tone_phase < AUDIO_OUT_RATE / 2) ? s_tone_level : -(int32_t)s_tone_level;
        ml += t << 7;
        mr += t << 7;
      }

      // clip to int16
      if (ml > 32767) ml = 32767; else if (ml < -32768) ml = -32768;
      if (mr > 32767) mr = 32767; else if (mr < -32768) mr = -32768;

      // Master volume scales the signed mix, i.e. about mid-scale (scaling
      // the unsigned PWM level pulled silence toward 0: a DC step, heard
      // as a pop, on every volume change).
      ml = ml * (int32_t)vol / 256;
      mr = mr * (int32_t)vol / 256;

      // int16 -> PWM range [0,STREAM_PWM_WRAP]
      uint32_t lv = ((uint32_t)(ml + 32768) * (STREAM_PWM_WRAP + 1)) >> 16;
      uint32_t rv = ((uint32_t)(mr + 32768) * (STREAM_PWM_WRAP + 1)) >> 16;
      if (lv > STREAM_PWM_WRAP) lv = STREAM_PWM_WRAP;
      if (rv > STREAM_PWM_WRAP) rv = STREAM_PWM_WRAP;
      buf[base_i + i] = (rv << 16) | lv;
    }
  }
  s_tone_phase = tone_phase;
}

// DMA completion ISR: swap ping-pong buffers and refill
static inline __attribute__((always_inline)) void stream_dma_isr_body(void) {
  s_stream_dma_isr_count++;
  dma_hw->ints0 = 1u << s_stream_dma_chan;

  if (!s_stream_dma_active || !s_streaming) {
    // Stopped: silence outputs, don't restart DMA
    pwm_set_both_levels(s_stream_pwm_slice, STREAM_PWM_MID, STREAM_PWM_MID);
    s_stream_dma_active = false;
    return;
  }

  // Swap to the pre-filled buffer and start DMA immediately
  int next = s_stream_dma_active_buf ^ 1;
  dma_channel_set_read_addr(s_stream_dma_chan, s_stream_dma_buf[next], true);

  // Refill the buffer that just finished playing
  audio_fill_dma_buffer(s_stream_dma_buf[s_stream_dma_active_buf], STREAM_DMA_SAMPLES);
  s_stream_dma_active_buf = next;
}

// Times stream_dma_isr_body for the `audiostat` dev command.
static volatile uint32_t s_isr_window, s_isr_us, s_isr_max_us;

static void __time_critical_func(audio_stream_dma_isr)(void) {
  uint32_t t0 = time_us_32();
  stream_dma_isr_body();
  uint32_t dt = time_us_32() - t0;
  s_isr_window++;
  s_isr_us += dt;
  if (dt > s_isr_max_us)
    s_isr_max_us = dt;
}

void audio_output_get_stats(audio_output_stats_t *out) {
  out->running = s_streaming;
  out->isr_count = s_isr_window;
  out->isr_us = s_isr_us;
  out->isr_max_us = s_isr_max_us;
}

void audio_output_reset_stats(void) {
  s_isr_window = 0;
  s_isr_us = 0;
  s_isr_max_us = 0;
}

void audio_stream_reset_underruns(void) {
  s_stream_underrun_count = 0;
}

void audio_start_stream(uint32_t sample_rate) {
  if (s_streaming)
    audio_stop_stream();

  // Configure PWM at the FIXED ultrasonic output rate; content rate only
  // drives the resample accumulator in audio_fill_dma_buffer.
  gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
  s_stream_pwm_slice = pwm_gpio_to_slice_num(AUDIO_PIN_L);

  s_stream_content_rate = sample_rate;
  s_ring.phase = 0;

  uint32_t sys_clk = clock_get_hz(clk_sys);
  uint32_t target = (uint32_t)AUDIO_OUT_RATE * (uint32_t)(STREAM_PWM_WRAP + 1);
  uint32_t div_int = sys_clk / target;
  uint32_t remainder = sys_clk - div_int * target;
  uint32_t div_frac = (remainder * 16 + target / 2) / target;
  if (div_int < 1) { div_int = 1; div_frac = 0; }

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, STREAM_PWM_WRAP);
  pwm_config_set_clkdiv_int_frac(&cfg, div_int, div_frac);
  pwm_init(s_stream_pwm_slice, &cfg, true);

  // Set up DMA channel paced by PWM DREQ
  if (s_stream_dma_chan < 0)
    s_stream_dma_chan = dma_claim_unused_channel(true);

  dma_channel_config dc = dma_channel_get_default_config(s_stream_dma_chan);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, DREQ_PWM_WRAP0 + s_stream_pwm_slice);

  dma_channel_configure(s_stream_dma_chan, &dc,
      &pwm_hw->slice[s_stream_pwm_slice].cc,
      s_stream_dma_buf[0],
      STREAM_DMA_SAMPLES,
      false);  // don't start yet

  // Use DMA_IRQ_0 (mp3_player uses DMA_IRQ_1 — no conflict)
  dma_channel_set_irq0_enabled(s_stream_dma_chan, true);

  // Reset ring buffer and pre-fill DMA buffers with silence
  audio_ring_clear(&s_ring);
  s_streaming = true;

  audio_fill_dma_buffer(s_stream_dma_buf[0], STREAM_DMA_SAMPLES);
  audio_fill_dma_buffer(s_stream_dma_buf[1], STREAM_DMA_SAMPLES);
  s_stream_dma_active_buf = 0;

  // Signal Core 1 to register IRQ handler and start DMA
  s_stream_dma_start_pending = true;
}

/* Start the stream only if it isn't already running (ring untouched).
 * Used by tone/sample playback so they can mix in without disturbing an
 * active stream (e.g. BGM fileplayer). */
void audio_stream_ensure_running(void) {
  if (!s_streaming)
    audio_start_stream(AUDIO_OUT_RATE);
}

void audio_stop_stream(void) {
  if (!s_streaming)
    return;

  s_stream_dma_start_pending = false;
  if (s_stream_dma_chan >= 0) {
    dma_channel_set_irq0_enabled(s_stream_dma_chan, false);
    dma_channel_abort(s_stream_dma_chan);
  }
  s_stream_dma_active = false;

  // Midpoint is true silence for AC-coupled output (no pop)
  pwm_set_both_levels(s_stream_pwm_slice, STREAM_PWM_MID, STREAM_PWM_MID);
  pwm_set_enabled(s_stream_pwm_slice, false);
  s_streaming = false;
}

void audio_stream_poll(void) {
  if (!s_stream_dma_start_pending)
    return;

  if (!s_stream_irq_on_core1) {
    irq_set_exclusive_handler(DMA_IRQ_0, audio_stream_dma_isr);
    irq_set_enabled(DMA_IRQ_0, true);
    s_stream_irq_on_core1 = true;
  }

  s_stream_dma_active = true;
  dma_channel_start(s_stream_dma_chan);
  s_stream_dma_start_pending = false;
}

void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns, uint32_t *ring_used) {
  if (isr_count) *isr_count = s_stream_dma_isr_count;
  if (underruns) *underruns = s_stream_underrun_count;
  if (ring_used) *ring_used = audio_ring_used(&s_ring);
}

uint32_t audio_ring_free(void) {
  return audio_ring_space(&s_ring);
}

void audio_push_samples(const int16_t *samples, int count) {
  audio_ring_push(&s_ring, samples, count);
}
