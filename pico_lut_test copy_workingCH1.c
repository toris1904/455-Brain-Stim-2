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
 * Pin assignments
 * -------------------------------------------------------------------------- */
#define PIN_TICK_PROBE      20u
#define PIN_BEAT_PROBE      21u

/* --------------------------------------------------------------------------
 * ISR-to-main-loop handoff
 *
 * The ISR computes the DAC code and stores it in g_pending_code, then
 * sets g_sample_ready to true. The main loop spins on g_sample_ready,
 * writes the code to the DAC via SPI, then clears the flag.
 *
 * Why not write SPI directly from the ISR?
 *   spi_write_blocking() polls the SPI TX FIFO status register in a
 *   busy loop. At 10 MHz SPI clock, a 24-bit transfer takes ~2.4 us
 *   of FIFO polling inside the ISR. Combined with the waveform_tick()
 *   computation (~1 us) and alarm handler overhead (~1 us), the total
 *   ISR time approaches 5 us. With a negative-period repeating timer
 *   (-10 us), the next alarm fires 10 us after the ISR returns, making
 *   the effective period 15 us (66 kHz) instead of 10 us (100 kHz).
 *
 *   Moving the SPI write to the main loop keeps the ISR under 2 us
 *   and preserves the intended 100 kHz sample rate. The main loop
 *   executes the SPI write within microseconds of the flag being set,
 *   so the additional latency is negligible compared to the 10 us
 *   sample period.
 *
 * Tradeoff: The main loop must spin (no sleep). This prevents the use
 * of low-power sleep modes. For a battery-powered brain stimulator
 * this matters at the system level, but for bench validation it is
 * the correct approach. Production firmware should use PIO+DMA instead,
 * which eliminates both the ISR overhead and the main-loop spin.
 * -------------------------------------------------------------------------- */
static volatile WaveformChannel g_channel_1;
static volatile WaveformChannel g_channel_2;
static volatile uint32_t g_tick_count = 0;
static volatile uint16_t g_pending_code = DAC_MIDPOINT;
static volatile bool     g_sample_ready = false;

/* --------------------------------------------------------------------------
 * Hardware timer ISR
 *
 * Compute only. No SPI, no blocking calls, no printf.
 * Target execution time: < 2 us.
 * -------------------------------------------------------------------------- */
static bool timer_isr(struct repeating_timer *rt)
{
    (void)rt;

    g_tick_count++;
    gpio_xor_mask(1u << PIN_TICK_PROBE);

    /* Compute the next DAC code. Cast away volatile for function call -
     * safe because the ISR is the sole writer of channel state. */
    g_pending_code = waveform_tick((WaveformChannel *)&g_channel_1);
    g_sample_ready = true;

    /* Channel 2 is computed but not written (single-DAC bench setup).
     * Remove this when a second DAC is connected. */
    waveform_tick((WaveformChannel *)&g_channel_2);

    return true;
}

/* --------------------------------------------------------------------------
 * TEST 1 - LUT sanity check
 * -------------------------------------------------------------------------- */
static void test_lut_values(void)
{
    printf("\r\n=== TEST 1: LUT Sanity Check ===\r\n");
    printf("Index   | DAC Code | Expected (approx)\r\n");
    printf("--------|----------|------------------\r\n");

    struct { uint32_t idx; const char *label; uint16_t expected; } checks[] = {
        {  0,   "sin(0)   = midpoint",  32768 },
        { 64,   "sin(90)  = +peak",     65535 },
        { 128,  "sin(180) = midpoint",  32768 },
        { 192,  "sin(270) = -trough",       1 },
    };

    bool all_pass = true;
    for (uint32_t i = 0; i < 4; i++) {
        uint16_t val = g_sine_lut[checks[i].idx];
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
 * -------------------------------------------------------------------------- */
static void test_phase_accumulator(void)
{
    printf("\r\n=== TEST 2: Phase Accumulator Simulation (2000 Hz, 50 ticks) ===\r\n");
    printf("Expected: index advances ~5 per tick, codes trace sine shape\r\n\r\n");

    WaveformChannel sim_ch;
    sim_ch.phase_accum     = 0;
    sim_ch.amplitude_scale = DAC_AMPLITUDE;  /* Full scale */
    sim_ch.enabled         = true;
    waveform_set_frequency(&sim_ch, 2000.0f);

    waveform_print_simulation(&sim_ch, 50);
}

/* --------------------------------------------------------------------------
 * TEST 3 - Dual channel beat frequency simulation
 * -------------------------------------------------------------------------- */
static void test_beat_frequency(void)
{
    printf("\r\n=== TEST 3: Beat Frequency Verification ===\r\n");
    printf("Ch1: 2000 Hz, Ch2: 2010 Hz, Beat: 10 Hz\r\n");
    printf("Simulating 10000 ticks (0.1 seconds at 100 kHz)\r\n\r\n");

    WaveformChannel ch1, ch2;
    ch1.phase_accum = ch2.phase_accum = 0;
    ch1.amplitude_scale = ch2.amplitude_scale = DAC_AMPLITUDE;
    ch1.enabled = ch2.enabled = true;
    waveform_set_frequency(&ch1, 2000.0f);
    waveform_set_frequency(&ch2, 2010.0f);

    for (uint32_t t = 0; t < 10000; t++) {
        waveform_tick(&ch1);
        waveform_tick(&ch2);
    }

    uint32_t idx1 = (ch1.phase_accum >> PHASE_FRAC_BITS) & (LUT_SIZE - 1);
    uint32_t idx2 = (ch2.phase_accum >> PHASE_FRAC_BITS) & (LUT_SIZE - 1);
    int32_t  diff = (int32_t)idx2 - (int32_t)idx1;

    bool pass = (abs((int)diff) < 3) || (abs((int)(diff - (int32_t)LUT_SIZE)) < 3);

    printf("Ch1 final LUT index: %lu\r\n", (unsigned long)idx1);
    printf("Ch2 final LUT index: %lu\r\n", (unsigned long)idx2);
    printf("Phase difference: %ld (expect ~0 or ~256)\r\n", (long)diff);
    printf("Result: %s\r\n", pass ? "PASS" : "FAIL");
}

/* --------------------------------------------------------------------------
 * TEST 4 - Hardware timer tick count
 * -------------------------------------------------------------------------- */
static void test_timer_accuracy(void)
{
    printf("\r\n=== TEST 4: Hardware Timer Accuracy (1 second) ===\r\n");
    printf("Running... (probe GP%u with scope, expect 50 kHz square wave)\r\n",
           PIN_TICK_PROBE);

    g_channel_1.enabled = true;
    g_channel_2.enabled = true;
    g_tick_count = 0;

    struct repeating_timer timer;
    add_repeating_timer_us(-10, timer_isr, NULL, &timer);

    /* Service the DAC writes from the main loop during the 1s test.
     * This exercises the full ISR -> main loop -> SPI pipeline. */
    absolute_time_t deadline = make_timeout_time_ms(1000);
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (g_sample_ready) {
            dac8411_write(g_pending_code);
            g_sample_ready = false;
        }
    }

    cancel_repeating_timer(&timer);
    g_channel_1.enabled = false;
    g_channel_2.enabled = false;

    uint32_t count = g_tick_count;
    bool pass = (count >= 99000 && count <= 101000);

    printf("Ticks in 1 second: %lu (expected ~100000)\r\n", (unsigned long)count);
    printf("Effective sample rate: %lu Hz\r\n", (unsigned long)count);
    printf("Result: %s\r\n", pass ? "PASS" : "FAIL");

    if (!pass) {
        printf("NOTE: Timer drift detected. Consider PIO+DMA for production.\r\n");
    }
}

/* --------------------------------------------------------------------------
 * TEST 5 - Frequency Accuracy Sweep
 * -------------------------------------------------------------------------- */
static void test_frequency_accuracy(void)
{
    printf("\r\n=== TEST 5: Frequency Accuracy Sweep ===\r\n");
    printf("Target(Hz) | Increment  | Actual(Hz)  | Error(Hz) | Error(%%) | Status\r\n");
    printf("-----------|------------|-------------|-----------|----------|-------\r\n");

    float test_frequencies[] = {
        10.0f, 40.0f, 100.0f, 500.0f, 1000.0f,
        2000.0f, 2010.0f, 2100.0f, 5000.0f, 10000.0f,
    };

    uint32_t num_frequencies = sizeof(test_frequencies) / sizeof(test_frequencies[0]);
    bool all_pass = true;

    const float ERROR_THRESHOLD_HZ      = 0.5f;
    const float ERROR_THRESHOLD_PERCENT = 0.05f;

    for (uint32_t f = 0; f < num_frequencies; f++) {
        float target_hz = test_frequencies[f];

        double increment_exact = ((double)target_hz / (double)SAMPLE_RATE_HZ)
                                 * (double)LUT_SIZE
                                 * (double)(1u << PHASE_FRAC_BITS);
        uint32_t increment_int = (uint32_t)(increment_exact + 0.5);

        double actual_hz = ((double)increment_int / (double)(1u << PHASE_FRAC_BITS))
                           * ((double)SAMPLE_RATE_HZ / (double)LUT_SIZE);

        double error_hz      = actual_hz - (double)target_hz;
        double error_percent = (error_hz / (double)target_hz) * 100.0;

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
}

/* --------------------------------------------------------------------------
 * TEST 6 - Continuous waveform output
 *
 * Runs the full signal chain (ISR -> main loop -> DAC) indefinitely.
 * Probe VOUT on the oscilloscope. Expected: 2000 Hz sine, 0V to 3.3V.
 * Press reset to stop.
 * -------------------------------------------------------------------------- */
static void test_continuous_waveform(void)
{
    printf("\r\n=== TEST 6: Continuous 2000 Hz Waveform Output ===\r\n");
    printf("Probe DAC VOUT with oscilloscope.\r\n");
    printf("Expected: 2000 Hz sine, ~0V to ~3.3V (full scale)\r\n");
    printf("Press RESET to stop.\r\n\r\n");

    g_channel_1.phase_accum = 0;
    g_channel_2.phase_accum = 0;
    g_channel_1.enabled = true;
    g_channel_2.enabled = true;
    g_sample_ready = false;

    struct repeating_timer timer;
    add_repeating_timer_us(-10, timer_isr, NULL, &timer);

    /* Main loop: spin and service DAC writes as fast as possible */
    while (true) {
        if (g_sample_ready) {
            dac8411_write(g_pending_code);
            g_sample_ready = false;
        }
    }
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
    printf("  Pico 1 W - DAC8411 Waveform Output\r\n");
    printf("========================================\r\n");

    /* Probe pin setup */
    gpio_init(PIN_TICK_PROBE);
    gpio_set_dir(PIN_TICK_PROBE, GPIO_OUT);
    gpio_init(PIN_BEAT_PROBE);
    gpio_set_dir(PIN_BEAT_PROBE, GPIO_OUT);

    /* DAC8411 init and static output test */
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

    /* Configure both channels */
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
    test_frequency_accuracy();

    printf("\r\n========================================\r\n");
    printf("  Validation complete.\r\n");
    printf("  Entering continuous waveform output.\r\n");
    printf("========================================\r\n");

    /* Enter continuous waveform mode (does not return) */
    test_continuous_waveform();

    return 0;
}