#ifndef WAVEFORM_H
#define WAVEFORM_H

#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Configuration constants
 * -------------------------------------------------------------------------- */

#define LUT_SIZE          256u          /* Sine table entries                   */
#define SAMPLE_RATE_HZ    100000u       /* DAC update rate in Hz                */
#define PHASE_FRAC_BITS   16u           /* Fractional bits in phase accumulator */
#define DAC_MIDPOINT      32768u        /* 16-bit midpoint (AVDD / 2)          */
#define DAC_MAX_CODE      65535u        /* Maximum 16-bit DAC code             */
#define DAC_AMPLITUDE     32767u        /* Maximum swing from midpoint          */

/* --------------------------------------------------------------------------
 * Waveform shape selection
 * -------------------------------------------------------------------------- */

typedef enum {
    WAVEFORM_SHAPE_SINE     = 0,
    WAVEFORM_SHAPE_TRIANGLE = 1,
    WAVEFORM_SHAPE_SQUARE   = 2
} WaveformShape;


/* --------------------------------------------------------------------------
 * Waveform channel state
 *
 * One instance per isolated channel. Each channel has a completely
 * independent phase accumulator so frequencies can differ freely.
 * This frequency difference is what produces the TI beat envelope.
 * -------------------------------------------------------------------------- */

typedef struct {
    uint32_t phase_accum;       /* Current phase, fixed-point Q16.16          */
    uint32_t phase_increment;   /* Added to phase_accum each tick             */
    uint16_t amplitude_scale;   /* 0 = off, 32767 = full scale               */
    WaveformShape shape;             /* Output waveform shape                      */
    bool     enabled;           /* Must be true for tick() to produce output  */
} WaveformChannel;

/* --------------------------------------------------------------------------
 * Sine lookup table - populated once at startup by waveform_init_lut()
 * Exposed so main.c can print it for validation.
 * -------------------------------------------------------------------------- */
extern uint16_t g_sine_lut[LUT_SIZE];





/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

void waveform_init_lut(void);
void waveform_set_frequency(WaveformChannel *ch, float frequency_hz);
void waveform_set_amplitude(WaveformChannel *ch, float amplitude);
void waveform_set_enabled(WaveformChannel *ch, bool enabled);
uint16_t waveform_tick(WaveformChannel *ch);
void waveform_print_simulation(WaveformChannel *ch, uint32_t num_ticks);
void waveform_set_shape(WaveformChannel *ch, WaveformShape shape);

#endif /* WAVEFORM_H */