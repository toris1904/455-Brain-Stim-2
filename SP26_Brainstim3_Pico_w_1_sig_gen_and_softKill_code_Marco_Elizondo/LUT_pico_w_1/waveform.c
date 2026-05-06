#include "waveform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Lookup table storage
 * -------------------------------------------------------------------------- */

uint16_t g_sine_lut[LUT_SIZE];

/* --------------------------------------------------------------------------
 * LUT initialisation
 *
 * Generates a full-scale sine wave spanning [0, 65535] with the
 * midpoint at 32768 (index 0, where sin(0) = 0).
 *
 * Formula: code = 32768 + 32767 * sin(angle)
 *   - At sin(0)     = 0:    code = 32768 (midpoint)
 *   - At sin(pi/2)  = +1:   code = 65535 (full scale)
 *   - At sin(pi)    = 0:    code = 32768 (midpoint)
 *   - At sin(3pi/2) = -1:   code = 1     (near zero)
 *
 * Previous version used 32767 + 32767*sin(), which:
 *   1. Made the midpoint 32767 instead of 32768 (off by 1 from DAC_MIDPOINT)
 *   2. Capped the peak at 65534, losing 1 LSB at full scale
 *   3. Created asymmetric swing: +32766 above midpoint, -32767 below
 *
 * This version uses 32768 + 32767*sin(), which:
 *   1. Centers exactly on DAC_MIDPOINT (32768)
 *   2. Reaches 65535 at the positive peak (full scale)
 *   3. Reaches 1 at the negative trough (symmetric swing of 32767 both ways)
 *
 * Tradeoff: the negative trough is code=1, not code=0. This means VOUT
 * never quite reaches 0V (off by 1 LSB = 50 uV at 3.3V reference). This
 * is negligible and the symmetry benefit is far more important for a
 * brain stimulator where DC offset in the current waveform matters.
 * -------------------------------------------------------------------------- */
void waveform_init_lut(void)
{
    for (uint32_t i = 0; i < LUT_SIZE; i++) {
        double angle = 2.0 * M_PI * (double)i / (double)LUT_SIZE;
        double value = 32768.0 + 32767.0 * sin(angle);

        /* Clamp to valid DAC range (defensive - should not be needed
         * given the formula, but protects against floating point edge cases) */
        if (value < 0.0)     value = 0.0;
        if (value > 65535.0) value = 65535.0;

        g_sine_lut[i] = (uint16_t)(value + 0.5);  /* Round, not truncate */
    }
}

/* --------------------------------------------------------------------------
 * Channel configuration
 * -------------------------------------------------------------------------- */

void waveform_set_frequency(WaveformChannel *ch, float frequency_hz)
{
    double increment = ((double)frequency_hz / (double)SAMPLE_RATE_HZ)
                       * (double)LUT_SIZE
                       * (double)(1u << PHASE_FRAC_BITS);

    ch->phase_increment = (uint32_t)(increment + 0.5);  /* Round */
}

/* --------------------------------------------------------------------------
 * waveform_set_amplitude
 *
 * amplitude_scale is now in the range [0, DAC_AMPLITUDE] (i.e. [0, 32767])
 * rather than [0, 65535]. This simplifies the tick() scaling math:
 *   scaled = (raw * amplitude_scale) / DAC_AMPLITUDE
 *
 * Using a 15-bit scale factor (>> 15) instead of 16-bit (>> 16) avoids
 * the off-by-one that caused the old code to never reach full swing.
 *
 * Tradeoff: amplitude resolution is 15 bits (~0.003%) instead of 16 bits.
 * For a brain stimulator where amplitude is controlled in 1% steps from
 * a WiFi GUI, 15 bits of resolution is vastly more than needed.
 * -------------------------------------------------------------------------- */
void waveform_set_amplitude(WaveformChannel *ch, float amplitude)
{
    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;
    ch->amplitude_scale = (uint16_t)(amplitude * (float)DAC_AMPLITUDE + 0.5f);
}

void waveform_set_enabled(WaveformChannel *ch, bool enabled)
{
    ch->enabled = enabled;
}

void waveform_set_shape(WaveformChannel *ch, WaveformShape shape)
{
    ch->shape = shape;
}


/* --------------------------------------------------------------------------
 * Tick - hot path
 *
 * Called from hardware timer ISR at 100 kHz. Returns the 16-bit DAC code.
 * The caller is responsible for writing the code to the DAC via SPI.
 *
 * Amplitude scaling:
 *   raw    = LUT value - midpoint, range [-32767, +32767]
 *   scaled = (raw * amplitude_scale) >> 15
 *
 * At full scale (amplitude_scale = 32767):
 *   positive peak: (32767 * 32767) >> 15 = 1073676289 >> 15 = 32766
 *   negative trough: (-32767 * 32767) >> 15 = -32766
 *   output codes: 32768 + 32766 = 65534, 32768 - 32766 = 2
 *
 * This gives essentially full rail-to-rail swing (65534 out of 65535),
 * losing only 1 LSB at each extreme due to integer arithmetic. At 3.3V
 * reference that is 50 uV - completely negligible.
 * -------------------------------------------------------------------------- */

 /*
 OLD Tick
uint16_t waveform_tick(WaveformChannel *ch)
{
    if (!ch->enabled) {
        return (uint16_t)DAC_MIDPOINT;
    }

    ch->phase_accum += ch->phase_increment;

    uint32_t lut_index = (ch->phase_accum >> PHASE_FRAC_BITS) & (LUT_SIZE - 1);

    int32_t raw       = (int32_t)g_sine_lut[lut_index] - (int32_t)DAC_MIDPOINT;
    int32_t scaled    = (raw * (int32_t)ch->amplitude_scale) >> 15;
    int32_t dac_code  = (int32_t)DAC_MIDPOINT + scaled;

    if (dac_code < 0)              dac_code = 0;
    if (dac_code > (int32_t)DAC_MAX_CODE)  dac_code = (int32_t)DAC_MAX_CODE;

    return (uint16_t)dac_code;
}
    */


/* --------------------------------------------------------------------------
 * Tick - hot path
 *
 * Called from hardware timer ISR at 100 kHz. Returns the 16-bit DAC code.
 *
 * Sine:
 *   Uses the pre-computed 256-entry LUT.
 *
 * Triangle:
 *   Computed directly from the upper 16 bits of the phase accumulator,
 *   giving 16-bit phase resolution vs the 8-bit LUT index. The accumulator
 *   upper word is folded: rising on [0, 0x7FFF], falling on [0x8000, 0xFFFF].
 *
 *     tri_phase in [0, 32767]
 *     raw = tri_phase * 2 - DAC_AMPLITUDE
 *
 * Square:
 *   Sign of the upper bit of the accumulator word selects +/- peak directly.
 *
 * Triangle harmonics fall off as 1/n^2 (vs 1/n for square), making it a
 * much friendlier load for the downstream RC filter and Howland bandwidth.
 * -------------------------------------------------------------------------- */
uint16_t waveform_tick(WaveformChannel *ch)
{
    if (!ch->enabled) {
        return (uint16_t)DAC_MIDPOINT;
    }

    ch->phase_accum += ch->phase_increment;

    int32_t raw;

    switch (ch->shape) {

        case WAVEFORM_SHAPE_TRIANGLE: {
            uint32_t lut_index = (ch->phase_accum >> PHASE_FRAC_BITS)
                                 & (LUT_SIZE - 1);
            int32_t half   = (int32_t)(LUT_SIZE / 2);  /* 128 */

            /* Fold lut_index into triangle:
             *   [0,   half-1] => rising slope
             *   [half, LUT_SIZE-1] => falling slope
             * folded is in [0, half-1] in both cases */
            int32_t folded = ((int32_t)lut_index < half)
                             ? (int32_t)lut_index
                             : (int32_t)(LUT_SIZE - 1) - (int32_t)lut_index;

            /* Scale folded [0, half-1] -> raw [-DAC_AMPLITUDE, +DAC_AMPLITUDE] */
            raw = ((folded * 2 * (int32_t)DAC_AMPLITUDE) / (half - 1))
                  - (int32_t)DAC_AMPLITUDE;
            break;
        }

        case WAVEFORM_SHAPE_SQUARE: {
            uint32_t lut_index = (ch->phase_accum >> PHASE_FRAC_BITS)
                                 & (LUT_SIZE - 1);
            raw = (lut_index < LUT_SIZE / 2) ? (int32_t)DAC_AMPLITUDE
                                              : -(int32_t)DAC_AMPLITUDE;
            break;
        }

        case WAVEFORM_SHAPE_SINE:
        default: {
            uint32_t lut_index = (ch->phase_accum >> PHASE_FRAC_BITS)
                                 & (LUT_SIZE - 1);
            raw = (int32_t)g_sine_lut[lut_index] - (int32_t)DAC_MIDPOINT;
            break;
        }
    }

    int32_t scaled   = (raw * (int32_t)ch->amplitude_scale) >> 15;
    int32_t dac_code = (int32_t)DAC_MIDPOINT + scaled;

    if (dac_code < 0)                        dac_code = 0;
    if (dac_code > (int32_t)DAC_MAX_CODE)    dac_code = (int32_t)DAC_MAX_CODE;

    return (uint16_t)dac_code;
}

/* --------------------------------------------------------------------------
 * Simulation helper - validation only, not called in production
 * -------------------------------------------------------------------------- */

void waveform_print_simulation(WaveformChannel *ch, uint32_t num_ticks)
{
    WaveformChannel snapshot = *ch;

    printf("tick | lut_idx | dac_code\r\n");
    printf("-----|---------|----------\r\n");

    for (uint32_t t = 0; t < num_ticks; t++) {
        uint32_t lut_index = (snapshot.phase_accum >> PHASE_FRAC_BITS)
                             & (LUT_SIZE - 1);
        uint16_t code      = waveform_tick(&snapshot);
        printf("%4lu |     %3lu |     %5u\r\n", (unsigned long)t,
               (unsigned long)lut_index, code);
    }
}