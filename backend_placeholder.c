
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include <math.h>

#define LUT_SIZE 65                    // 65-point lookup table
#define SAMPLE_RATE_HZ 100000          // 100 kHz sample rate
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE_HZ)  // 10 microseconds
#define DAC_RESOLUTION 65536           // 16-bit DAC (DAC8411)
#define DAC_CENTER_VALUE (DAC_RESOLUTION / 2)  // 1.65V center point

// Using SPI0 for Channel 1
#define CH1_SPI_INSTANCE spi0
#define CH1_PIN_MOSI     3             // GP3 - MOSI (Master Out Slave In)
#define CH1_PIN_SCK      2             // GP2 - SCLK (Serial Clock)
#define CH1_PIN_CS       5             // GP5 - CS1 (Chip Select Channel 1)


// Using SPI1 for Channel 2
#define CH2_SPI_INSTANCE spi1
#define CH2_PIN_MOSI     11            // GP11 - MOSI (Master Out Slave In)
#define CH2_PIN_SCK      10            // GP10 - SCLK (Serial Clock)
#define CH2_PIN_CS       13            // GP13 - CS2 (Chip Select Channel 2)

// SPI Configuration
#define SPI_BAUDRATE     10000000      // 10 MHz SPI clock (DAC8411 max is 50 MHz)


typedef struct {
    spi_inst_t *spi;                   // SPI instance pointer
    uint8_t pin_mosi;
    uint8_t pin_sck;
    uint8_t pin_cs;
    bool enabled;                       // Channel enable flag
    uint16_t amplitude;                 // Amplitude scaling (0-65535)
    float frequency_hz;                 // Output frequency in Hz
    uint32_t phase_accumulator;         // Phase accumulator for frequency synthesis
    uint32_t phase_increment;           // Phase increment per sample
} channel_config_t;



// Channel configurations
static channel_config_t channel1;
static channel_config_t channel2;

// 65-point sine wave lookup table (extern - defined elsewhere)
extern const uint16_t sine_lut[LUT_SIZE];

// Timer alarm handle
static alarm_id_t sample_timer_id;


void backend_init(void);
void backend_set_channel_frequency(uint8_t channel, float freq_hz);
void backend_set_channel_amplitude(uint8_t channel, uint16_t amplitude);
void backend_enable_channel(uint8_t channel, bool enable);
static void init_spi_channel(channel_config_t *ch);
static void send_dac_value(channel_config_t *ch, uint16_t value);
static int64_t sample_timer_callback(alarm_id_t id, void *user_data);
static uint16_t get_lut_sample(uint32_t phase_acc);

/**
 * Initialize the dual-channel signal generation backend
 */
void backend_init(void) {
    // Initialize Channel 1 configuration
    channel1.spi = CH1_SPI_INSTANCE;
    channel1.pin_mosi = CH1_PIN_MOSI;
    channel1.pin_sck = CH1_PIN_SCK;
    channel1.pin_cs = CH1_PIN_CS;
    channel1.enabled = false;
    channel1.amplitude = DAC_RESOLUTION - 1;  // Full scale by default
    channel1.frequency_hz = 100.0f;            // 100 Hz default
    channel1.phase_accumulator = 0;
    channel1.phase_increment = 0;
    
    // Initialize Channel 2 configuration
    channel2.spi = CH2_SPI_INSTANCE;
    channel2.pin_mosi = CH2_PIN_MOSI;
    channel2.pin_sck = CH2_PIN_SCK;
    channel2.pin_cs = CH2_PIN_CS;
    channel2.enabled = false;
    channel2.amplitude = DAC_RESOLUTION - 1;  // Full scale by default
    channel2.frequency_hz = 100.0f;            // 100 Hz default
    channel2.phase_accumulator = 0;
    channel2.phase_increment = 0;
    
    // Initialize SPI hardware for both channels
    init_spi_channel(&channel1);
    init_spi_channel(&channel2);
    
    // Set initial DACs to center value (1.65V)
    send_dac_value(&channel1, DAC_CENTER_VALUE);
    send_dac_value(&channel2, DAC_CENTER_VALUE);
    
    // Start the sample timer (100 kHz)
    sample_timer_id = add_alarm_in_us(SAMPLE_PERIOD_US, sample_timer_callback, NULL, true);
}

/**
 * Initialize SPI hardware for a channel
 */
static void init_spi_channel(channel_config_t *ch) {
    // Initialize SPI at specified baud rate
    spi_init(ch->spi, SPI_BAUDRATE);
    
    // Configure SPI pins
    gpio_set_function(ch->pin_mosi, GPIO_FUNC_SPI);
    gpio_set_function(ch->pin_sck, GPIO_FUNC_SPI);
    
    // Configure CS pin as output (manual control)
    gpio_init(ch->pin_cs);
    gpio_set_dir(ch->pin_cs, GPIO_OUT);
    gpio_put(ch->pin_cs, 1);  // CS high (inactive)
    
    // Configure SPI format: 16-bit data, CPOL=0, CPHA=0
    spi_set_format(ch->spi, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}


/**
 * Set channel output frequency
 * @param channel Channel number (1 or 2)
 * @param freq_hz Frequency in Hz (max 10 kHz for smooth output)
 */
void backend_set_channel_frequency(uint8_t channel, float freq_hz) {
    channel_config_t *ch = (channel == 1) ? &channel1 : &channel2;
    
    // Limit frequency to Nyquist limit with safety margin
    if (freq_hz > 10000.0f) freq_hz = 10000.0f;
    if (freq_hz < 0.1f) freq_hz = 0.1f;
    
    ch->frequency_hz = freq_hz;
    
    // Calculate phase increment using DDS (Direct Digital Synthesis) method
    // phase_increment = (freq_hz * LUT_SIZE * 2^32) / SAMPLE_RATE_HZ
    ch->phase_increment = (uint32_t)((freq_hz * LUT_SIZE * 4294967296.0) / SAMPLE_RATE_HZ);
}

/**
 * Set channel amplitude
 * @param channel Channel number (1 or 2)
 * @param amplitude Amplitude (0-65535, where 32768 = ±3.3V swing)
 */
void backend_set_channel_amplitude(uint8_t channel, uint16_t amplitude) {
    channel_config_t *ch = (channel == 1) ? &channel1 : &channel2;
    ch->amplitude = amplitude;
}

/**
 * Enable or disable a channel
 * @param channel Channel number (1 or 2)
 * @param enable true to enable, false to disable
 */
void backend_enable_channel(uint8_t channel, bool enable) {
    channel_config_t *ch = (channel == 1) ? &channel1 : &channel2;
    ch->enabled = enable;
    
    if (!enable) {
        // Set to center value when disabled
        send_dac_value(ch, DAC_CENTER_VALUE);
        ch->phase_accumulator = 0;
    }
}

/**
 * Timer callback - executes at 100 kHz sample rate
 * Generates waveform samples for both channels
 */
static int64_t sample_timer_callback(alarm_id_t id, void *user_data) {
    // Channel 1 signal generation
    if (channel1.enabled) {
        // Get sample from LUT
        uint16_t sample = get_lut_sample(channel1.phase_accumulator);
        
        // Apply amplitude scaling
        int32_t scaled = ((int32_t)sample * (int32_t)channel1.amplitude) >> 16;
        
        // Send to DAC
        send_dac_value(&channel1, (uint16_t)scaled);
        
        // Update phase accumulator
        channel1.phase_accumulator += channel1.phase_increment;
    }
    
    // Channel 2 signal generation
    if (channel2.enabled) {
        // Get sample from LUT
        uint16_t sample = get_lut_sample(channel2.phase_accumulator);
        
        // Apply amplitude scaling
        int32_t scaled = ((int32_t)sample * (int32_t)channel2.amplitude) >> 16;
        
        // Send to DAC
        send_dac_value(&channel2, (uint16_t)scaled);
        
        // Update phase accumulator
        channel2.phase_accumulator += channel2.phase_increment;
    }
    
    // Reschedule timer for next sample
    return SAMPLE_PERIOD_US;
}

/**
 * Get interpolated sample from lookup table
 * Uses upper 32 bits of phase accumulator as LUT index
 */
static uint16_t get_lut_sample(uint32_t phase_acc) {
    // Extract index from upper bits
    uint32_t index = phase_acc >> 26;  // Top 6 bits (0-63)
    
    // Handle wraparound (65th point = 0th point)
    if (index >= LUT_SIZE - 1) {
        index = 0;
    }
    
    return sine_lut[index];
}



/**
 * Send 16-bit value to DAC8411 via SPI
 * DAC8411 command format: 16 bits data
 * @param ch Channel configuration
 * @param value 16-bit DAC value (0-65535)
 */
static void send_dac_value(channel_config_t *ch, uint16_t value) {
    // CS low (active)
    gpio_put(ch->pin_cs, 0);
    
    // Send 16-bit value (MSB first)
    uint8_t data[2];
    data[0] = (value >> 8) & 0xFF;   // MSB
    data[1] = value & 0xFF;           // LSB
    
    spi_write_blocking(ch->spi, data, 2);
    
    // CS high (inactive)
    gpio_put(ch->pin_cs, 1);
}

// Default 65-point sine wave LUT (0-3.3V unipolar, centered at 1.65V)
// This should be defined in a separate file and marked 'extern' above
#ifndef EXTERNAL_LUT
const uint16_t sine_lut[LUT_SIZE] = {
    32768, 35990, 39145, 42165, 44984, 47547, 49807, 51723, 53261, 54394,
    55102, 55374, 55202, 54590, 53545, 52083, 50226, 48000, 45438, 42580,
    39465, 36138, 32646, 29037, 25361, 21667, 18005, 14423, 10965, 7675,
    4592, 1756, 512, 512, 1179, 2487, 4412, 6920, 9965, 13497,
    17455, 21768, 26357, 31133, 36002, 40865, 45621, 50165, 54397, 58221,
    61548, 64295, 65535, 65535, 65535, 65535, 65535, 65535, 62624, 59138,
    54881, 49901, 44255, 38011, 31249
};
#endif
