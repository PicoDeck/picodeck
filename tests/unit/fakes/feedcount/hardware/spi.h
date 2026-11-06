// hardware/spi.h for compiling sdcard.c on the host: its SPI setup does nothing
// here, the RAM disk under FatFS is the card. The test defines the baud rate
// the block reads back and sees every write of it.
#ifndef HARDWARE_SPI_H
#define HARDWARE_SPI_H

#include <stdint.h>

#define spi0 ((void *)0)
#ifndef GPIO_OUT
#define GPIO_OUT 1  // the stub gpio.h has no direction names
#endif
#define SPI_CPOL_0 0
#define SPI_CPHA_0 0
#define SPI_MSB_FIRST 0
static inline unsigned spi_init(void *spi, unsigned baud) { (void)spi; return baud; }
extern unsigned g_fake_spi_baud;
void fake_spi_baud_written(unsigned baud);
static inline unsigned spi_set_baudrate(void *spi, unsigned baud) {
  (void)spi;
  fake_spi_baud_written(baud);
  return baud;
}
static inline unsigned spi_get_baudrate(const void *spi) { (void)spi; return g_fake_spi_baud; }
static inline void spi_set_format(void *spi, int bits, int cpol, int cpha, int order) {
  (void)spi; (void)bits; (void)cpol; (void)cpha; (void)order;
}

#endif  // HARDWARE_SPI_H
