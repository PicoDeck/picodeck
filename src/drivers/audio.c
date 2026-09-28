#include "audio.h"
#include "audio_mix.h"
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

// audio_init's idle setup of the slice; the output replaces it.
#define IDLE_PWM_WRAP 255

void audio_init(void) {
  gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, IDLE_PWM_WRAP);
  pwm_init(pwm_gpio_to_slice_num(AUDIO_PIN_L), &cfg, false);
  pwm_init(pwm_gpio_to_slice_num(AUDIO_PIN_R), &cfg, false);

  audio_mix_init();
  audio_set_volume(100);
}

void audio_core1_init(void) {
  // Hardware alarm 2 (default pool uses 3). 4 slots covers the Core 1
  // tick and spare.
  s_core1_alarm_pool = alarm_pool_create(2, 4);
  if (!s_core1_alarm_pool) {
    printf("[AUDIO] WARNING: failed to create Core 1 alarm pool\n");
  }
}

alarm_pool_t *audio_get_core1_alarm_pool(void) {
  return s_core1_alarm_pool;
}

// ── The output: the mix as PWM at AUDIO_OUT_RATE, fed by DMA ────────────────
//
// A DMA channel paced by the PWM wrap DREQ writes one compare value
// (R << 16 | L) per frame from two ping-pong buffers of OUT_DMA_FRAMES.
// Its completion interrupt (DMA_IRQ_0, registered on Core 1) starts the
// other buffer and renders the mix into the one that just played. The
// output starts with the first sound and runs, silent between sounds,
// until the app's teardown stops it.

// 1512 counts a frame: 44,091.7 Hz (-0.02%) at every supported clk_sys,
// with a divider exact in 8.4 fixed point at each: 1.875 at 125 MHz, 2.25
// at 150, 3 at 200, 3.75 at 250, 4.5 at 300. (A wrap of 1699 made a whole
// divider only at 150/300 MHz: 43,776 Hz at 200, 13 cents flat, and a pitch
// jump at the video player's boost.)
#define OUT_PWM_WRAP   1511
#define OUT_PWM_MID    ((OUT_PWM_WRAP + 1) / 2)
#define OUT_DMA_FRAMES 128
#define OUT_CHUNK      32    // frames per audio_mix_render call

static int           s_dma_chan = -1;
static uint32_t      s_dma_buf[2][OUT_DMA_FRAMES];
static volatile int  s_dma_active_buf = 0;
static volatile bool s_dma_active = false;
static volatile bool s_dma_start_pending = false;
static bool          s_irq_on_core1 = false;
static unsigned int  s_pwm_slice = 0;
static volatile bool s_output_on = false;
static int16_t       s_render[2 * OUT_CHUNK];

// The refill stats. The ISR (Core 1) writes them; Core 0 only asks for a
// reset, which the ISR carries out at its next refill (its read-modify-
// writes would put back counts read before a reset written from Core 0).
static volatile uint32_t s_isr_total;   // refills since boot
static volatile uint32_t s_isr_window;  // since the last stats reset
static volatile uint32_t s_isr_us;
static volatile uint32_t s_isr_max_us;
static volatile bool     s_isr_reset_req;

// [-32768, 32767] -> [0, OUT_PWM_WRAP] (65535 * 1512 >> 16 is 1511).
static inline __attribute__((always_inline)) uint32_t pwm_level(int16_t v) {
  return ((uint32_t)((int32_t)v + 32768) * (OUT_PWM_WRAP + 1)) >> 16;
}

static void __time_critical_func(output_fill)(uint32_t *buf) {
  for (int base = 0; base < OUT_DMA_FRAMES; base += OUT_CHUNK) {
    audio_mix_render(s_render, OUT_CHUNK);
    for (int i = 0; i < OUT_CHUNK; i++)
      buf[base + i] = (pwm_level(s_render[2 * i + 1]) << 16) |
                      pwm_level(s_render[2 * i]);
  }
}

static void __time_critical_func(output_isr)(void) {
  uint32_t t0 = time_us_32();
  if (s_isr_reset_req) {
    s_isr_window = 0;
    s_isr_us = 0;
    s_isr_max_us = 0;
    s_isr_reset_req = false;
  }
  s_isr_total++;
  s_isr_window++;
  dma_hw->ints0 = 1u << s_dma_chan;
  if (!s_dma_active || !s_output_on) {
    // Stopped: hold mid-scale (silence), don't restart the DMA.
    pwm_set_both_levels(s_pwm_slice, OUT_PWM_MID, OUT_PWM_MID);
    s_dma_active = false;
  } else {
    // Start the buffer rendered last time, then render the one that just
    // finished playing.
    int next = s_dma_active_buf ^ 1;
    dma_channel_set_read_addr(s_dma_chan, s_dma_buf[next], true);
    output_fill(s_dma_buf[s_dma_active_buf]);
    s_dma_active_buf = next;
  }
  uint32_t dt = time_us_32() - t0;
  s_isr_us += dt;
  if (dt > s_isr_max_us)
    s_isr_max_us = dt;
}

// The PWM divider (8.4 fixed point) for AUDIO_OUT_RATE frames a second at
// the current clk_sys.
static void output_divider(uint8_t *div_int, uint8_t *div_frac4) {
  uint32_t sys = clock_get_hz(clk_sys);
  uint32_t target = (uint32_t)AUDIO_OUT_RATE * (OUT_PWM_WRAP + 1);
  uint32_t whole = sys / target;
  uint32_t frac = ((sys - whole * target) * 16 + target / 2) / target;
  if (frac == 16) {
    whole++;
    frac = 0;
  }
  if (whole < 1) {
    whole = 1;
    frac = 0;
  }
  *div_int = (uint8_t)whole;
  *div_frac4 = (uint8_t)frac;
}

void audio_output_ensure_running(void) {
  if (s_output_on)
    return;
  gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
  s_pwm_slice = pwm_gpio_to_slice_num(AUDIO_PIN_L);

  uint8_t div_int, div_frac4;
  output_divider(&div_int, &div_frac4);
  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, OUT_PWM_WRAP);
  pwm_config_set_clkdiv_int_frac4(&cfg, div_int, div_frac4);
  pwm_init(s_pwm_slice, &cfg, true);

  if (s_dma_chan < 0)
    s_dma_chan = dma_claim_unused_channel(true);
  dma_channel_config dc = dma_channel_get_default_config(s_dma_chan);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, DREQ_PWM_WRAP0 + s_pwm_slice);
  dma_channel_configure(s_dma_chan, &dc, &pwm_hw->slice[s_pwm_slice].cc,
                        s_dma_buf[0], OUT_DMA_FRAMES, false);
  dma_channel_set_irq0_enabled(s_dma_chan, true);

  // Both buffers start silent; the ISR renders the mix into each as it
  // frees up, so the render only ever runs on Core 1.
  for (int b = 0; b < 2; b++)
    for (int i = 0; i < OUT_DMA_FRAMES; i++)
      s_dma_buf[b][i] = ((uint32_t)OUT_PWM_MID << 16) | OUT_PWM_MID;
  s_dma_active_buf = 0;
  s_output_on = true;
  s_dma_start_pending = true;  // audio_stream_poll starts it on Core 1
}

void audio_output_stop(void) {
  if (!s_output_on)
    return;
  s_output_on = false;
  s_dma_start_pending = false;
  if (s_dma_chan >= 0) {
    dma_channel_set_irq0_enabled(s_dma_chan, false);
    dma_channel_abort(s_dma_chan);
  }
  s_dma_active = false;
  // Mid-scale is silence for the AC-coupled output (no pop).
  pwm_set_both_levels(s_pwm_slice, OUT_PWM_MID, OUT_PWM_MID);
  pwm_set_enabled(s_pwm_slice, false);
}

bool audio_output_running(void) {
  return s_output_on;
}

uint32_t audio_output_isr_count(void) {
  return s_isr_total;
}

void audio_apply_clock(void) {
  if (!s_output_on)
    return;
  uint8_t div_int, div_frac4;
  output_divider(&div_int, &div_frac4);
  pwm_set_clkdiv_int_frac4(s_pwm_slice, div_int, div_frac4);
}

void audio_stream_poll(void) {
  if (!s_dma_start_pending)
    return;
  if (!s_irq_on_core1) {
    irq_set_exclusive_handler(DMA_IRQ_0, output_isr);
    irq_set_enabled(DMA_IRQ_0, true);
    s_irq_on_core1 = true;
  }
  s_dma_active = true;
  dma_channel_start(s_dma_chan);
  s_dma_start_pending = false;
}

void audio_output_get_stats(audio_output_stats_t *out) {
  out->running = s_output_on;
  // A reset the ISR has not carried out yet (it cannot while the output is
  // stopped) reads as the empty window it asked for.
  bool reset = s_isr_reset_req;
  out->isr_count = reset ? 0 : s_isr_window;
  out->isr_us = reset ? 0 : s_isr_us;
  out->isr_max_us = reset ? 0 : s_isr_max_us;
}

// Carried out by the refill ISR at its next refill (~2.9 ms while the
// output runs).
void audio_output_reset_stats(void) {
  s_isr_reset_req = true;
}
