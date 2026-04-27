#include "dac8411.h"

/* --------------------------------------------------------------------------
 * dac8411_init
 *
 * SPI mode 1 (CPOL=0, CPHA=1): clock idles low, data is latched on the
 * falling edge of SCLK.
 *
 * Datasheet reference (SBAS439C, Section 9.1.1.3):
 *   "The 68HC11 should be configured so that its CPOL bit is a 0 and
 *    its CPHA bit is a 1."
 * -------------------------------------------------------------------------- */
void dac8411_init(void)
{
    spi_init(DAC_SPI_PORT, DAC_SPI_BAUD);

    spi_set_format(DAC_SPI_PORT,
                   8,            /* 8 bits per transfer frame  */
                   SPI_CPOL_0,   /* Clock idles low            */
                   SPI_CPHA_1,   /* Data captured on falling edge */
                   SPI_MSB_FIRST);

    gpio_set_function(DAC_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(DAC_PIN_MOSI, GPIO_FUNC_SPI);

    /* CH1 SYNC - software-driven CS */
    gpio_init(DAC_PIN_SYNC_CH1);
    gpio_set_dir(DAC_PIN_SYNC_CH1, GPIO_OUT);
    gpio_put(DAC_PIN_SYNC_CH1, 1);  /* Deasserted - idles high */

    /* CH2 SYNC - software-driven CS */
    gpio_init(DAC_PIN_SYNC_CH2);
    gpio_set_dir(DAC_PIN_SYNC_CH2, GPIO_OUT);
    gpio_put(DAC_PIN_SYNC_CH2, 1);  /* Deasserted - idles high */
}

/* --------------------------------------------------------------------------
 * dac8411_write
 *
 * DAC8411 24-bit input shift register (Figure 79 in datasheet):
 *
 *   Bit:   23   22   21   20   19   18   ...  6    5    4    3    2    1    0
 *   Name:  PD1  PD0  D15  D14  D13  D12  ...  D0   X    X    X    X    X    X
 *
 * PD1=0, PD0=0 for normal operation (any other value = power-down mode,
 * which tristates VOUT and pulls it to GND via internal resistor).
 *
 * Mapping the 24-bit shift register to three bytes sent MSB-first:
 *
 *   Byte 0 [23:16]:  PD1  PD0  D15  D14  D13  D12  D11  D10
 *   Byte 1 [15:8]:   D9   D8   D7   D6   D5   D4   D3   D2
 *   Byte 2 [7:0]:    D1   D0   X    X    X    X    X    X
 *
 * With PD=00:
 *   byte0 = (code >> 10) & 0x3F
 *   byte1 = (code >> 2)  & 0xFF
 *   byte2 = (code << 6)  & 0xC0
 *
 * Verification with known codes:
 *
 *   code = 0x0000:  byte0=0x00, byte1=0x00, byte2=0x00  -> VOUT = 0V
 *   code = 0x8000:  byte0=0x20, byte1=0x00, byte2=0x00  -> VOUT = Vref/2
 *   code = 0xFFFF:  byte0=0x3F, byte1=0xFF, byte2=0xC0  -> VOUT = Vref
 *
 * Cross-check 0x8000 bit-by-bit:
 *   byte0 = 0x20 = 0b00100000
 *   DB23=0(PD1) DB22=0(PD0) DB21=1(D15) DB20=0(D14) ...
 *   Correct: D15=1, rest zero => midpoint code.
 *
 * SYNC must remain low for all 24 clock edges. Releasing early aborts
 * the write and the DAC ignores the data.
 * -------------------------------------------------------------------------- */
void dac8411_write(uint8_t channel, uint16_t code)
{
    uint8_t frame[3];

    frame[0] = (uint8_t)((code >> 10) & 0x3F);   /* PD1=0, PD0=0, D15..D10 */
    frame[1] = (uint8_t)((code >> 2)  & 0xFF);    /* D9..D2                  */
    frame[2] = (uint8_t)((code << 6)  & 0xC0);    /* D1..D0, 6 don't-care    */

    uint sync_pin = (channel == 0) ? DAC_PIN_SYNC_CH1 : DAC_PIN_SYNC_CH2;

    gpio_put(sync_pin, 0);
    spi_write_blocking(DAC_SPI_PORT, frame, 3);
    gpio_put(sync_pin, 1);
}