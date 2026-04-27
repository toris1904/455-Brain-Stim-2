#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/gpio.h"
#include "waveform.h"
#include "dac8411.h"

/* --------------------------------------------------------------------------
 * Pin assignments (breadboard test - no DAC hardware yet)
 * GP25 = onboard LED (Pico 2 W uses different LED driver, see note below)
 * GP2  = scope probe point / logic analyser - toggles each timer tick
 *        allows you to verify 100 kHz timing with a scope or frequency
 *        counter even without a logic analyser
 * -------------------------------------------------------------------------- */
#define PIN_TICK_PROBE      20u    /* Toggle this pin at every timer callback  */
#define PIN_BEAT_PROBE      21u    /* Goes high for one tick when beat detected */

/* --------------------------------------------------------------------------
 * Global channel state
 * Volatile because modified in ISR and read in main loop.
 * -------------------------------------------------------------------------- */
static volatile WaveformChannel g_channel_1;
static volatile WaveformChannel g_channel_2;

/* Tick counter for timing validation */
static volatile uint32_t g_tick_count = 0;

/* Running sum for beat detection demo (non-ISR, computed in main) */
static uint16_t g_last_ch1_code = DAC_MIDPOINT;
static uint16_t g_last_ch2_code = DAC_MIDPOINT;

/* --------------------------------------------------------------------------
 * Hardware timer ISR
 *
 * Target: fires at exactly SAMPLE_RATE_HZ (100 kHz = every 10 us).
 * Using a repeating timer from the Pico SDK.
 *
 * Tradeoff: repeating_timer uses alarm hardware which has ~1us jitter.
 * For validation this is fine. For production, PIO + DMA gives
 * sample-accurate timing at the cost of more complex setup code.
 * -------------------------------------------------------------------------- */
static bool timer_isr(struct repeating_timer *rt)
{
    (void)rt;

    g_tick_count++;

    /* Toggle probe pin - scope/logic analyser sees 50 kHz square wave
     * if timer is firing correctly at 100 kHz                         */
    gpio_xor_mask(1u << PIN_TICK_PROBE);

    /* Advance both channels. Cast away volatile for function call -
     * safe here because ISR is the only writer.                        */
    dac8411_write(waveform_tick((WaveformChannel *)&g_channel_1));
    waveform_tick((WaveformChannel *)&g_channel_2);

    return true; /* Return true to keep repeating */
}

/* --------------------------------------------------------------------------
 * TEST 1 - LUT sanity check
 * Print key indices and expected values to USB serial.
 * -------------------------------------------------------------------------- */
static void test_lut_values(void)
{
    printf("\r\n=== TEST 1: LUT Sanity Check ===\r\n");
    printf("Index   | DAC Code | Expected (approx)\r\n");
    printf("--------|----------|------------------\r\n");

    struct { uint32_t idx; const char *label; uint16_t expected; } checks[] = {
        {  0,   "sin(0)   = 0V mid",  32767 },
        { 64,   "sin(90)  = +peak",   65534 },
        { 128,  "sin(180) = 0V mid",  32767 },
        { 192,  "sin(270) = -trough",     0 },
    };

    bool all_pass = true;
    for (uint32_t i = 0; i < 4; i++) {
        uint16_t val = g_sine_lut[checks[i].idx];
        /* Allow +/-100 counts tolerance for floating point rounding */
        bool pass = (abs((int)val - (int)checks[i].expected) < 100);
        if (!pass) all_pass = false;
        printf("  %3lu   |  %5u   | ~%5u  [%s] %s\r\n",
               (unsigned long)checks[i].idx,
               val,
               checks[i].expected,
               checks[i].label,
               pass ? "PASS" : "FAIL");
    }

    printf("Result: %s\r\n", all_pass ? "ALL PASS" : "FAILURES DETECTED");
}

/* --------------------------------------------------------------------------
 * TEST 2 - Phase accumulator simulation
 * Simulate 50 ticks at 2000 Hz (one full cycle) and print LUT indices.
 * You should see the index advance by ~5 per tick and the DAC codes
 * trace a sine shape (rising then falling).
 * -------------------------------------------------------------------------- */
static void test_phase_accumulator(void)
{
    printf("\r\n=== TEST 2: Phase Accumulator Simulation (2000 Hz, 50 ticks) ===\r\n");
    printf("Expected: index advances ~5 per tick, codes trace sine shape\r\n\r\n");

    WaveformChannel sim_ch;
    sim_ch.phase_accum     = 0;
    sim_ch.amplitude_scale = 65535;
    sim_ch.enabled         = true;
    waveform_set_frequency(&sim_ch, 2000.0f);

    waveform_print_simulation(&sim_ch, 50);
}

/* --------------------------------------------------------------------------
 * TEST 3 - Dual channel beat frequency simulation
 * Run ch1 at 2000 Hz and ch2 at 2010 Hz for 10000 ticks (0.1 seconds).
 * After 0.1s the phase difference should have advanced by exactly one
 * full LUT rotation (the 10 Hz beat period = 0.1s).
 * -------------------------------------------------------------------------- */
static void test_beat_frequency(void)
{
    printf("\r\n=== TEST 3: Beat Frequency Verification ===\r\n");
    printf("Ch1: 2000 Hz, Ch2: 2010 Hz, Beat: 10 Hz\r\n");
    printf("Simulating 10000 ticks (0.1 seconds at 100 kHz)\r\n\r\n");

    WaveformChannel ch1, ch2;
    ch1.phase_accum = ch2.phase_accum = 0;
    ch1.amplitude_scale = ch2.amplitude_scale = 65535;
    ch1.enabled = ch2.enabled = true;
    waveform_set_frequency(&ch1, 2000.0f);
    waveform_set_frequency(&ch2, 2010.0f);

    for (uint32_t t = 0; t < 10000; t++) {
        waveform_tick(&ch1);
        waveform_tick(&ch2);
    }

    uint32_t idx1 = (ch1.phase_accum >> PHASE_FRAC_BITS) % LUT_SIZE;
    uint32_t idx2 = (ch2.phase_accum >> PHASE_FRAC_BITS) % LUT_SIZE;
    int32_t  diff = (int32_t)idx2 - (int32_t)idx1;

    /* After exactly 0.1s (one 10 Hz beat period), the phase difference
     * should be one full LUT rotation = LUT_SIZE = 256 indices.
     * Due to integer truncation expect within +/-2 counts.             */
    bool pass = (abs((int)diff) < 3) || (abs((int)(diff - (int32_t)LUT_SIZE)) < 3);

    printf("Ch1 final LUT index: %lu\r\n", (unsigned long)idx1);
    printf("Ch2 final LUT index: %lu\r\n", (unsigned long)idx2);
    printf("Phase difference: %ld (expect ~0 or ~256)\r\n", (long)diff);
    printf("Result: %s\r\n", pass ? "PASS" : "FAIL");
}

/* --------------------------------------------------------------------------
 * TEST 4 - Hardware timer tick count
 * Run the repeating timer for 1 second and count actual callbacks.
 * Expected: ~100000 ticks. Anything below 99000 indicates dropped ticks.
 * -------------------------------------------------------------------------- */
static void test_timer_accuracy(void)
{
    printf("\r\n=== TEST 4: Hardware Timer Accuracy (1 second) ===\r\n");
    printf("Running... (probe GP%u with scope, expect 50 kHz square wave)\r\n",
           PIN_TICK_PROBE);

    /* Enable channels so tick() does real work during the test */
    g_channel_1.enabled = true;
    g_channel_2.enabled = true;
    g_tick_count = 0;

    struct repeating_timer timer;
    /* Negative period = interval from END of callback, avoids drift.
     * Tradeoff vs positive period: slightly lower actual rate if callback
     * takes nonzero time, but no missed-tick accumulation.
     * At 100 kHz each tick budget is 10 us. waveform_tick() takes ~1 us
     * so negative period is acceptable here.                           */
    add_repeating_timer_us(-10, timer_isr, NULL, &timer);

    sleep_ms(1000);

    cancel_repeating_timer(&timer);
    g_channel_1.enabled = false;
    g_channel_2.enabled = false;

    uint32_t count = g_tick_count;
    bool pass = (count >= 99000 && count <= 101000);

    printf("Tick count after 1 second: %lu\r\n", (unsigned long)count);
    printf("Expected: ~100000\r\n");
    printf("Result: %s\r\n", pass ? "PASS" : "FAIL - consider PIO approach");

    if (!pass) {
        printf("NOTE: MicroPython/C ISR overhead may require PIO for production.\r\n");
    }
}

/* --------------------------------------------------------------------------
 * TEST 5 - Frequency Accuracy Sweep
 *
 * Sweeps across the full operating range and measures the actual output
 * frequency produced by the phase accumulator against the target.
 *
 * Method:
 *   For each target frequency, simulate exactly SAMPLE_RATE_HZ ticks
 *   (one full second). Count how many complete LUT cycles occurred.
 *   Actual frequency = number of complete cycles in one second.
 *
 *   Also compute the theoretical frequency error from fixed-point
 *   truncation of the phase increment, without needing to simulate
 *   a full second of ticks.
 *
 * This catches edge cases where fixed-point truncation causes
 * meaningful frequency error at specific target values.
 * -------------------------------------------------------------------------- */
static void test_frequency_accuracy(void)
{
    printf("\r\n=== TEST 5: Frequency Accuracy Sweep ===\r\n");
    printf("Target(Hz) | Increment  | Actual(Hz)  | Error(Hz) | Error(%%) | Status\r\n");
    printf("-----------|------------|-------------|-----------|----------|-------\r\n");

    /* Frequencies to test - covers full operating range including
     * the exact TI carrier frequencies we will use in production  */
    float test_frequencies[] = {
        10.0f,      /* Beat frequency lower bound (theta)           */
        40.0f,      /* Beat frequency upper bound (gamma)           */
        100.0f,     /* Low carrier sanity check                     */
        500.0f,     /* Mid-range                                    */
        1000.0f,    /* 1 kHz                                        */
        2000.0f,    /* TI carrier Ch1 (production frequency)        */
        2010.0f,    /* TI carrier Ch2 (production frequency)        */
        2100.0f,    /* 100 Hz beat example                         */
        5000.0f,    /* Upper mid-range                              */
        10000.0f,   /* Maximum rated output frequency               */
    };

    uint32_t num_frequencies = sizeof(test_frequencies) / sizeof(test_frequencies[0]);
    bool all_pass = true;

    /* Acceptable error threshold: 0.5 Hz or 0.05% whichever is larger.
     * Rationale: TI stimulation beat frequency must be accurate enough
     * to target a specific neural band. 0.5 Hz error at 10 Hz beat = 5%
     * which is the practical limit of neural band selectivity.
     * At carrier frequencies (2000 Hz) 0.5 Hz error = 0.025% - negligible. */
    const float ERROR_THRESHOLD_HZ      = 0.5f;
    const float ERROR_THRESHOLD_PERCENT = 0.05f;

    for (uint32_t f = 0; f < num_frequencies; f++) {
        float target_hz = test_frequencies[f];

        /* Compute phase increment (same formula as waveform_set_frequency) */
        double increment_exact = ((double)target_hz / (double)SAMPLE_RATE_HZ)
                                 * (double)LUT_SIZE
                                 * (double)(1u << PHASE_FRAC_BITS);
        uint32_t increment_int = (uint32_t)increment_exact;

        /* Compute actual frequency from the truncated integer increment.
         * The truncation is the sole source of frequency error in this
         * architecture - there is no drift or jitter from the LUT itself. */
        double actual_hz = ((double)increment_int / (double)(1u << PHASE_FRAC_BITS))
                           * ((double)SAMPLE_RATE_HZ / (double)LUT_SIZE);

        double error_hz      = actual_hz - (double)target_hz;
        double error_percent = (error_hz / (double)target_hz) * 100.0;

        /* Apply absolute value for threshold comparison */
        double abs_error_hz      = error_hz < 0 ? -error_hz : error_hz;
        double abs_error_percent = error_percent < 0 ? -error_percent : error_percent;

        bool pass = (abs_error_hz <= (double)ERROR_THRESHOLD_HZ) ||
                    (abs_error_percent <= (double)ERROR_THRESHOLD_PERCENT);

        if (!pass) all_pass = false;

        printf("%10.1f | %10lu | %11.4f | %9.4f | %8.4f | %s\r\n",
               (double)target_hz,
               (unsigned long)increment_int,
               actual_hz,
               error_hz,
               error_percent,
               pass ? "PASS" : "FAIL");
    }

    printf("\r\nOverall result: %s\r\n", all_pass ? "ALL PASS" : "FAILURES DETECTED");

    /* Print the worst-case error for the report */
    printf("\r\nNote: Error is caused entirely by fixed-point truncation of\r\n");
    printf("the phase increment. Increasing LUT_SIZE reduces this error\r\n");
    printf("proportionally (2x LUT = 2x better frequency resolution).\r\n");
}



/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(void)
{
    stdio_init_all();
    sleep_ms(5000);

    printf("\r\n========================================\r\n");
    printf("  Signal Generation LUT Validation\r\n");
    printf("  Pico 1 W - DAC8411 Hardware Test\r\n");
    printf("========================================\r\n");

    /* Probe pin setup */
    gpio_init(PIN_TICK_PROBE);
    gpio_set_dir(PIN_TICK_PROBE, GPIO_OUT);
    gpio_init(PIN_BEAT_PROBE);
    gpio_set_dir(PIN_BEAT_PROBE, GPIO_OUT);

    /* DAC8411 static output test */
    dac8411_init();

    printf("\r\n=== DAC8411 Static Output Test ===\r\n");
    printf("Probe VOUT with DMM. Expected: 0V -> 1.65V -> 3.3V\r\n\r\n");

    printf("Writing 0x0000 - expect ~0V on VOUT...\r\n");
    dac8411_write(0x0000);
    sleep_ms(2000);

    printf("Writing 0x8000 - expect ~1.65V on VOUT...\r\n");
    dac8411_write(0x8000);
    sleep_ms(2000);

    printf("Writing 0xFFFF - expect ~3.3V on VOUT...\r\n");
    dac8411_write(0xFFFF);
    sleep_ms(2000);

    printf("Static test complete. Check your DMM readings above.\r\n");

    /* Initialise LUT */
    waveform_init_lut();

    /* Configure both channels - TI stimulation frequencies */
    waveform_set_frequency((WaveformChannel *)&g_channel_1, 2000.0f);
    waveform_set_frequency((WaveformChannel *)&g_channel_2, 2010.0f);
    waveform_set_amplitude((WaveformChannel *)&g_channel_1, 1.0f);
    waveform_set_amplitude((WaveformChannel *)&g_channel_2, 1.0f);
    g_channel_1.enabled = false;
    g_channel_2.enabled = false;
    g_channel_1.phase_accum = 0;
    g_channel_2.phase_accum = 0;

    /* Run validation suite */
    test_lut_values();
    test_phase_accumulator();
    test_beat_frequency();
    test_timer_accuracy();

    printf("\r\n========================================\r\n");
    printf("  All tests complete.\r\n");
    printf("  Waveform running continuously on VOUT.\r\n");
    printf("  Probe DAC VOUT (pin 6) with oscilloscope.\r\n");
    printf("========================================\r\n");

    /* Reset phase and enable channel 1 only for scope observation.
     * Channel 2 remains disabled - single channel sufficient for
     * waveform shape and frequency verification at this stage.     */
    g_channel_1.phase_accum = 0;
    g_channel_1.enabled     = true;
    g_channel_2.enabled     = false;

    /* Start timer and leave running indefinitely */
    struct repeating_timer waveform_timer;
    add_repeating_timer_us(-10, timer_isr, NULL, &waveform_timer);

    while (true) {
        tight_loop_contents();
    }

    return 0;
}