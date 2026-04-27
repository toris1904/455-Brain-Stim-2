#ifndef DAC8411_H
#define DAC8411_H

#include <stdint.h>
#include "hardware/spi.h"
#include "hardware/gpio.h"

/* --------------------------------------------------------------------------
 * Hardware assignment
 *
 * 
 * SPI0 peripheral, 10 MHz clock.
 *
 * CRITICAL: The DAC8411 datasheet (SBAS439C, Table 7.7) specifies a
 * maximum SCLK frequency of 20 MHz when AVDD is in the 2.0V-3.6V range.
 * However, the RP2040 SPI peripheral derives its clock by dividing the
 * 125 MHz system clock by an even integer. Requesting 20 MHz yields
 * 125/6 = 20.833 MHz, which violates the timing spec:
 *   - t1 (cycle time) = 48 ns < 50 ns minimum
 *   - t2 (high time)  = 24 ns < 25 ns minimum
 *   - t3 (low time)   = 24 ns < 25 ns minimum
 *
 * This marginal timing violation causes unreliable bit latching during
 * high-speed continuous writes (waveform mode), even though static DC
 * writes may appear to work because the DAC has time to settle between
 * writes.
 *
 * 10 MHz (125/12 = 10.417 MHz, cycle time 96 ns) provides a comfortable
 * 2x margin. At 10 MHz, a 24-bit frame takes 2.4 us, leaving 7.6 us
 * of the 10 us sample period for computation and overhead.
 *
 * Tradeoff: 10 MHz vs 20 MHz
 *   Pro:  Guaranteed reliable latching at any temperature within the
 *         -40C to 125C operating range. No bit errors under dynamic load.
 *   Con:  SPI transfer takes 2.4 us instead of 1.2 us. Still well within
 *         the 10 us sample budget at 100 kHz. Not a meaningful penalty.
 *
 * SYNC is driven as a software GPIO rather than hardware CS because
 * the hardware CS deasserts between bytes, which would abort the
 * 24-bit frame after the first 8 bits.
 * -------------------------------------------------------------------------- */
#define DAC_SPI_PORT    spi0
#define DAC_SPI_BAUD    10000000u
#define DAC_PIN_SCK     2u
#define DAC_PIN_MOSI    3u
#define DAC_PIN_SYNC_CH1  5u
#define DAC_PIN_SYNC_CH2  6u


/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * Initialise SPI0 and configure GPIO pins for both DAC8411 channels.
 * Sets up shared SCLK/MOSI and per-channel SYNC (CS) lines.
 * Must be called once before any dac8411_write() calls.
 */
void dac8411_init(void);

/**
 * Write a 16-bit DAC code to the specified channel's DAC8411.
 *
 * @param channel  0 for CH1 (GP5), 1 for CH2 (GP6)
 * @param code     16-bit DAC code (0x0000 = 0V, 0xFFFF = AVDD)
 *
 * Both channels share SPI0 (SCLK/MOSI). Only the SYNC pin differs.
 * Writes are sequential, not concurrent — the SPI bus is single-master.
 *
 * NOT safe to call from a hardware timer ISR.
 */
void dac8411_write(uint8_t channel, uint16_t code);

#endif /* DAC8411_H */