// Pico SDK Stubs
// Minimal implementations of Pico SDK functions for PC simulator

#ifndef PICO_SDK_STUBS_H
#define PICO_SDK_STUBS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Version
#define PICO_SDK_VERSION "simulator"

// Types
typedef uint32_t uint;

// Stub hardware types
struct io_bank0_status_ctrl_hw {
    uint32_t status;
    uint32_t ctrl;
};

struct io_bank0_status_ctrl_hw_ctrl;

// GPIO functions (stubs)
static inline void gpio_init(uint gpio) { (void)gpio; }
static inline void gpio_set_dir(uint gpio, bool out) { (void)gpio; (void)out; }
static inline void gpio_put(uint gpio, bool value) { (void)gpio; (void)value; }
static inline bool gpio_get(uint gpio) { (void)gpio; return false; }
static inline void gpio_set_function(uint gpio, uint32_t function) { (void)gpio; (void)function; }
static inline void gpio_pull_up(uint gpio) { (void)gpio; }
static inline void gpio_pull_down(uint gpio) { (void)gpio; }
static inline void gpio_disable_pulls(uint gpio) { (void)gpio; }
static inline void gpio_set_input_enabled(uint gpio, bool enabled) { (void)gpio; (void)enabled; }
static inline void gpio_set_irq_enabled(uint gpio, uint32_t events, bool enabled) { (void)gpio; (void)events; (void)enabled; }
static inline void gpio_set_irq_callback(void (*callback)(uint gpio, uint32_t events)) { (void)callback; }
static inline void gpio_acknowledge_irq(uint gpio, uint32_t events) { (void)gpio; (void)events; }
static inline uint32_t gpio_get_all(void) { return 0; }
static inline void gpio_set_mask(uint32_t mask) { (void)mask; }
static inline void gpio_clr_mask(uint32_t mask) { (void)mask; }
static inline void gpio_xor_mask(uint32_t mask) { (void)mask; }

// Constants
#define GPIO_OUT 1
#define GPIO_IN 0
#define GPIO_FUNC_SIO 5
#define GPIO_IRQ_EDGE_FALL 0x04
#define GPIO_IRQ_EDGE_RISE 0x08

// PWM stubs
static inline void pwm_init(uint slice_num, void* cfg, bool start) { (void)slice_num; (void)cfg; (void)start; }
static inline void pwm_set_gpio_level(uint gpio, uint16_t level) { (void)gpio; (void)level; }
static inline void pwm_set_enabled(uint slice_num, bool enabled) { (void)slice_num; (void)enabled; }
static inline uint pwm_gpio_to_slice_num(uint gpio) { (void)gpio; return 0; }
static inline uint pwm_gpio_to_channel(uint gpio) { (void)gpio; return 0; }
static inline void pwm_set_wrap(uint slice_num, uint16_t wrap) { (void)slice_num; (void)wrap; }

// I2C stubs
struct i2c_inst;
typedef struct i2c_inst i2c_inst_t;
extern i2c_inst_t* i2c0;
extern i2c_inst_t* i2c1;

static inline void i2c_init(i2c_inst_t* i2c, uint32_t baudrate) { (void)i2c; (void)baudrate; }
static inline void i2c_deinit(i2c_inst_t* i2c) { (void)i2c; }
static inline int i2c_read_blocking(i2c_inst_t* i2c, uint8_t addr, uint8_t* dst, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)dst; (void)len; (void)nostop;
    return -1; // Not implemented
}
static inline int i2c_write_blocking(i2c_inst_t* i2c, uint8_t addr, const uint8_t* src, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)src; (void)len; (void)nostop;
    return -1; // Not implemented
}
static inline uint i2c_get_index(i2c_inst_t* i2c) { (void)i2c; return 0; }

// SPI stubs
struct spi_inst;
typedef struct spi_inst spi_inst_t;
extern spi_inst_t* spi0;
extern spi_inst_t* spi1;

static inline void spi_init(spi_inst_t* spi, uint32_t baudrate) { (void)spi; (void)baudrate; }
static inline void spi_deinit(spi_inst_t* spi) { (void)spi; }
static inline int spi_write_blocking(spi_inst_t* spi, const uint8_t* src, size_t len) {
    (void)spi; (void)src; (void)len;
    return (int)len;
}
static inline int spi_read_blocking(spi_inst_t* spi, uint8_t repeated_tx_data, uint8_t* dst, size_t len) {
    (void)spi; (void)repeated_tx_data; (void)dst; (void)len;
    return (int)len;
}
static inline int spi_write_read_blocking(spi_inst_t* spi, const uint8_t* src, uint8_t* dst, size_t len) {
    (void)spi; (void)src; (void)dst; (void)len;
    return (int)len;
}
static inline uint spi_get_index(spi_inst_t* spi) { (void)spi; return 0; }
static inline void spi_set_format(spi_inst_t* spi, uint data_bits, uint cpol, uint cpha, uint order) {
    (void)spi; (void)data_bits; (void)cpol; (void)cpha; (void)order;
}

// DMA stubs
struct dma_channel_config;
typedef struct dma_channel_config dma_channel_config;

static inline int dma_claim_unused_channel(bool required) { (void)required; return 0; }
static inline void dma_channel_unclaim(int channel) { (void)channel; }
static inline void dma_channel_configure(int channel, const dma_channel_config* config, volatile void* write_addr, const volatile void* read_addr, uint transfer_count, bool trigger) {
    (void)channel; (void)config; (void)write_addr; (void)read_addr; (void)transfer_count; (void)trigger;
}
static inline void dma_channel_set_read_addr(int channel, const volatile void* addr, bool trigger) {
    (void)channel; (void)addr; (void)trigger;
}
static inline void dma_channel_set_write_addr(int channel, volatile void* addr, bool trigger) {
    (void)channel; (void)addr; (void)trigger;
}
static inline void dma_channel_set_trans_count(int channel, uint32_t count, bool trigger) {
    (void)channel; (void)count; (void)trigger;
}
static inline void dma_channel_start(int channel) { (void)channel; }
static inline void dma_channel_abort(int channel) { (void)channel; }
static inline bool dma_channel_is_busy(int channel) { (void)channel; return false; }
static inline void dma_channel_wait_for_finish_blocking(int channel) { (void)channel; }
static inline void dma_channel_set_irq0_enabled(int channel, bool enabled) { (void)channel; (void)enabled; }
static inline void dma_irqn_acknowledge_channel(uint irq_index, int channel) { (void)irq_index; (void)channel; }

// Clock stubs
static inline uint32_t clock_get_hz(uint clock_index) { (void)clock_index; return 125000000; }
static inline void sleep_ms(uint32_t ms) { 
    extern void hal_sleep_ms(uint32_t);
    hal_sleep_ms(ms);
}
static inline void sleep_us(uint64_t us) {
    extern void hal_sleep_us(uint64_t);
    hal_sleep_us(us);
}

#define clk_sys 0
#define clk_peri 1

// Watchdog stubs
#ifdef __EMSCRIPTEN__
// Called from the Lua hook every 256 opcodes: the web build's escape hatch for
// Lua loops that never sleep or flush.
#include "../hal/web_platform.h"
static inline void watchdog_update(void) { web_yield_if_due(); }
#else
static inline void watchdog_update(void) {}
#endif
static inline void watchdog_enable(uint32_t delay_ms, bool pause_on_debug) { (void)delay_ms; (void)pause_on_debug; }
static inline void watchdog_disable(void) {}
static inline uint32_t watchdog_get_count(void) { return 0; }

// Multicore stubs
static inline void multicore_launch_core1(void (*entry)(void)) { (void)entry; }
static inline void multicore_reset_core1(void) {}
static inline void multicore_fifo_push_blocking(uint32_t data) { (void)data; }
static inline uint32_t multicore_fifo_pop_blocking(void) { return 0; }
static inline bool multicore_fifo_rvalid(void) { return false; }
static inline bool multicore_fifo_wready(void) { return true; }
static inline void multicore_fifo_drain(void) {}
static inline void multicore_fifo_clear_irq(void) {}

// XIP cache stubs
static inline void xip_cache_clean_all(void) {}
static inline void xip_cache_invalidate_all(void) {}
static inline void xip_cache_clean_range(uint32_t start, uint32_t len) { (void)start; (void)len; }
static inline void xip_cache_invalidate_range(uint32_t start, uint32_t len) { (void)start; (void)len; }

// PSRAM stubs (will be implemented properly in hal_psram.c)
void* psram_malloc(size_t size);
void psram_free(void* ptr);

#endif // PICO_SDK_STUBS_H
