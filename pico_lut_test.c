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

#include "pico/cyw43_arch.h"

/* --------------------------------------------------------------------------
 * Pin assignments
 * -------------------------------------------------------------------------- */

#define PIN_TICK_PROBE        20u
#define PIN_BEAT_PROBE        21u
#define PIN_KILL_BTN          9u
#define PIN_ON_BTN            10u

/* --------------------------------------------------------------------------
 * Ramp configuration
 *
 * A 5-second linear ramp is implemented by stepping amplitude every
 * RAMP_STEP_INTERVAL_MS milliseconds. At 10 ms per step over 5000 ms
 * this yields 500 steps, each advancing amplitude by 1/500 = 0.2%.
 *
 * Tradeoff: a finer step interval (e.g. 1 ms / 1000 steps) would produce
 * a smoother ramp but increases the frequency at which set_amplitude_both()
 * is called from the main loop, competing with DAC write servicing.
 * At 10 ms intervals the ramp update consumes a negligible fraction of
 * main loop cycles relative to the 10 us DAC write cadence.
 * -------------------------------------------------------------------------- */
#define RAMP_DURATION_MS      5000u
#define RAMP_STEP_INTERVAL_MS 10u
#define RAMP_STEPS            (RAMP_DURATION_MS / RAMP_STEP_INTERVAL_MS)
#define RAMP_STEP_DELTA       (1.0f / (float)RAMP_STEPS)
#define BTN_DEBOUNCE_US       20000u


/* --------------------------------------------------------------------------
 * Device state machine
 *
 * OFF          + ON button   -> RAMPING_UP
 * RAMPING_UP   + ramp done   -> RUNNING
 * RAMPING_UP   + KILL button -> RAMPING_DOWN   (kill always wins)
 * RUNNING      + KILL button -> RAMPING_DOWN
 * RAMPING_DOWN + ramp done   -> OFF
 *
 * The ON button is only acted on in OFF. The KILL button is only acted
 * on in RUNNING and RAMPING_UP. Pressing either button outside its valid
 * states is silently ignored.
 * -------------------------------------------------------------------------- */
typedef enum {
    DEVICE_STATE_OFF,
    DEVICE_STATE_RAMPING_UP,
    DEVICE_STATE_RUNNING,
    DEVICE_STATE_RAMPING_DOWN
} DeviceState;

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
static volatile uint16_t g_pending_code_ch1 = DAC_MIDPOINT;
static volatile uint16_t g_pending_code_ch2 = DAC_MIDPOINT;
static volatile bool     g_sample_ready     = false;

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

    /* Compute DAC codes for both channels.
     * Cast away volatile - safe because ISR is sole writer. */
    g_pending_code_ch1 = waveform_tick((WaveformChannel *)&g_channel_1);
    g_pending_code_ch2 = waveform_tick((WaveformChannel *)&g_channel_2);
    g_sample_ready = true;

    return true;
}



/* --------------------------------------------------------------------------
 * Button debounce
 *
 * Buttons are wired active-low with internal pull-ups enabled on GP9/GP10.
 * button_poll() returns true exactly once per confirmed press, after which
 * the consumed flag prevents re-triggering until the button is released.
 *
 * Tradeoff: edge-detect + confirm (implemented here) is simpler than a
 * shift-register integrator but requires the button to stay low for the
 * full debounce window. For mechanical tactile buttons this is always met
 * at 20 ms. Increase BTN_DEBOUNCE_US if noisy presses are observed.
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t pin;
    uint64_t last_fall_us;
    bool     pending;
    bool     consumed;
} ButtonState;

static bool button_poll(ButtonState *btn)
{
    bool     pin_low = !gpio_get(btn->pin);
    uint64_t now     = time_us_64();

    if (!pin_low) {
        btn->pending  = false;
        btn->consumed = false;
        return false;
    }

    if (!btn->pending && !btn->consumed) {
        btn->pending      = true;
        btn->last_fall_us = now;
        return false;
    }

    if (btn->pending && !btn->consumed &&
        (now - btn->last_fall_us) >= (uint64_t)BTN_DEBOUNCE_US) {
        btn->consumed = true;
        btn->pending  = false;
        return true;
    }

    return false;
}

/* --------------------------------------------------------------------------
 * set_amplitude_both
 *
 * Applies the same normalized amplitude [0.0, 1.0] to both channels
 * atomically within a single loop iteration, ensuring both channels always
 * carry the same scale factor during a ramp. If they were updated in
 * separate calls across different iterations, a brief asymmetry in beat
 * envelope amplitude would occur at each step.
 *
 * Atomicity note: amplitude_scale is volatile uint16_t. On Cortex-M0+
 * (RP2040), an aligned 16-bit store is a single STR.H instruction and
 * is therefore atomic with respect to the ISR read. No critical section
 * is required.
 * -------------------------------------------------------------------------- */
static void set_amplitude_both(float amplitude)
{
    waveform_set_amplitude((WaveformChannel *)&g_channel_1, amplitude);
    waveform_set_amplitude((WaveformChannel *)&g_channel_2, amplitude);
}


/* --------------------------------------------------------------------------
 * set_led
 *
 * On Pico W variants the onboard LED is routed through the CYW43 wireless
 * chip rather than a direct GPIO. cyw43_arch_gpio_put() is the correct
 * API. cyw43_arch_init() must be called once in main() before this is used.
 * -------------------------------------------------------------------------- */
static void set_led(bool on)
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
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
            dac8411_write(0, g_pending_code_ch1);
            dac8411_write(1, g_pending_code_ch2);
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
    printf("\r\n=== TEST 6: Continuous Dual-Channel Waveform Output ===\r\n");
    printf("Probe both DAC VOUTs with oscilloscope.\r\n");
    printf("Expected: CH1 = 2000 Hz sine, CH2 = 2010 Hz sine\r\n");
    printf("Beat envelope at 10 Hz visible when signals are summed.\r\n");
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
            dac8411_write(0, g_pending_code_ch1);
            dac8411_write(1, g_pending_code_ch2);
            g_sample_ready = false;
        }
    }
}



/* --------------------------------------------------------------------------
 * device_run
 *
 * Production entry point. Replaces test_continuous_waveform() once
 * validation is complete. Runs the full ISR -> DAC write pipeline with
 * GP9 (KILL) and GP10 (ON) button control and 5-second amplitude ramps.
 *
 * Phase behaviour on restart: waveform_tick() does not advance the phase
 * accumulator when enabled == false, so the accumulators hold their last
 * position during OFF state. On the next RAMPING_UP, both channels resume
 * from their held phase, preserving inter-channel phase coherence.
 *
 * Does not return.
 * -------------------------------------------------------------------------- */
static void device_run(void)
{
    printf("\r\n========================================\r\n");
    printf("  Device ready.\r\n");
    printf("  Press ON  (GP%u) to start.\r\n", PIN_ON_BTN);
    printf("  Press KILL (GP%u) to ramp down.\r\n", PIN_KILL_BTN);
    printf("========================================\r\n\r\n");

    ButtonState kill_btn  = { PIN_KILL_BTN, 0u, false, false };
    ButtonState on_btn    = { PIN_ON_BTN,   0u, false, false };

    DeviceState state        = DEVICE_STATE_OFF;
    float       amplitude    = 0.0f;
    uint64_t    last_ramp_us = 0u;

    /* Channels start enabled at zero amplitude so phase accumulators
     * advance continuously. This preserves phase coherence from the
     * moment the ramp begins, regardless of how long the device sat in OFF. */
    set_amplitude_both(0.0f);
    g_channel_1.enabled     = true;
    g_channel_2.enabled     = true;
    g_channel_1.phase_accum = 0u;
    g_channel_2.phase_accum = 0u;

    struct repeating_timer timer;
    add_repeating_timer_us(-10, timer_isr, NULL, &timer);

    while (true) {

        /* Service DAC writes — highest priority, every iteration */
        if (g_sample_ready) {
            dac8411_write(0, g_pending_code_ch1);
            dac8411_write(1, g_pending_code_ch2);
            g_sample_ready = false;
        }

        bool     kill_pressed = button_poll(&kill_btn);
        bool     on_pressed   = button_poll(&on_btn);
        uint64_t now          = time_us_64();

        switch (state) {

            case DEVICE_STATE_OFF:
                if (on_pressed) {
                    printf("ON pressed — ramping up over %u s.\r\n",
                           (unsigned)(RAMP_DURATION_MS / 1000u));
                    waveform_set_enabled((WaveformChannel *)&g_channel_1, true);
                    waveform_set_enabled((WaveformChannel *)&g_channel_2, true);
                    last_ramp_us = now;
                    state        = DEVICE_STATE_RAMPING_UP;
                }
                break;

            case DEVICE_STATE_RAMPING_UP:
                if (kill_pressed) {
                    set_led(false);
                    printf("KILL pressed during ramp-up — reversing to ramp-down.\r\n");
                    last_ramp_us = now;
                    state        = DEVICE_STATE_RAMPING_DOWN;
                    break;
                }

                if ((now - last_ramp_us) >= (uint64_t)RAMP_STEP_INTERVAL_MS * 1000u) {
                    last_ramp_us  = now;
                    amplitude    += RAMP_STEP_DELTA;

                    if (amplitude >= 1.0f) {
                        amplitude = 1.0f;
                        set_amplitude_both(amplitude);
                        set_led(true);
                        printf("Ramp-up complete — running at full amplitude.\r\n");
                        state = DEVICE_STATE_RUNNING;
                    } else {
                        set_amplitude_both(amplitude);
                    }
                }
                break;

            case DEVICE_STATE_RUNNING:
                if (kill_pressed) {
                    set_led(false);
                    printf("KILL pressed — ramping down over %u s.\r\n",
                           (unsigned)(RAMP_DURATION_MS / 1000u));
                    last_ramp_us = now;
                    state        = DEVICE_STATE_RAMPING_DOWN;
                }
                break;

            case DEVICE_STATE_RAMPING_DOWN:
                if ((now - last_ramp_us) >= (uint64_t)RAMP_STEP_INTERVAL_MS * 1000u) {
                    last_ramp_us  = now;
                    amplitude    -= RAMP_STEP_DELTA;

                    if (amplitude <= 0.0f) {
                        amplitude = 0.0f;
                        set_amplitude_both(amplitude);
                        waveform_set_enabled((WaveformChannel *)&g_channel_1, false);
                        waveform_set_enabled((WaveformChannel *)&g_channel_2, false);
                        printf("Ramp-down complete — device is OFF.\r\n");
                        printf("Press ON (GP%u) to restart.\r\n", PIN_ON_BTN);
                        state = DEVICE_STATE_OFF;
                    } else {
                        set_amplitude_both(amplitude);
                    }
                }
                break;
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

    /* Button pin setup — active-low, internal pull-ups */
    gpio_init(PIN_KILL_BTN);
    gpio_set_dir(PIN_KILL_BTN, GPIO_IN);
    gpio_pull_up(PIN_KILL_BTN);

    gpio_init(PIN_ON_BTN);
    gpio_set_dir(PIN_ON_BTN, GPIO_IN);
    gpio_pull_up(PIN_ON_BTN);


    /* DAC8411 init and static output test */
    dac8411_init();

    printf("\r\n=== DAC8411 Static Output Test ===\r\n");
    printf("Probe both CH1 and CH2 VOUT with DMM.\r\n");
    printf("Expected: both channels track 0V -> 1.65V -> 3.3V\r\n\r\n");

    printf("Writing 0x0000 - expect ~0V on both VOUT...\r\n");
    dac8411_write(0, 0x0000);
    dac8411_write(1, 0x0000);
    sleep_ms(2000);

    printf("Writing 0x8000 - expect ~1.65V on both VOUT...\r\n");
    dac8411_write(0, 0x8000);
    dac8411_write(1, 0x8000);
    sleep_ms(2000);

    printf("Writing 0xFFFF - expect ~3.3V on both VOUT...\r\n");
    dac8411_write(0, 0xFFFF);
    dac8411_write(1, 0xFFFF);
    sleep_ms(2000);

    printf("Static test complete. Check DMM readings on both channels.\r\n");

    /* Initialise LUT */
    waveform_init_lut();

    /* Configure both channels */
    waveform_set_frequency((WaveformChannel *)&g_channel_1, 2000.0f);
    waveform_set_frequency((WaveformChannel *)&g_channel_2, 2010.0f);
    waveform_set_amplitude((WaveformChannel *)&g_channel_1, 1.0f);
    waveform_set_amplitude((WaveformChannel *)&g_channel_2, 1.0f);

    //sine example
    //waveform_set_shape((WaveformChannel *)&g_channel_1, WAVEFORM_SHAPE_SINE); 
    //waveform_set_shape((WaveformChannel *)&g_channel_2, WAVEFORM_SHAPE_SINE);

    //triangle example
    //waveform_set_shape((WaveformChannel *)&g_channel_1, WAVEFORM_SHAPE_TRIANGLE); 
    //waveform_set_shape((WaveformChannel *)&g_channel_2, WAVEFORM_SHAPE_TRIANGLE);

    //square example
    waveform_set_shape((WaveformChannel *)&g_channel_1, WAVEFORM_SHAPE_SQUARE); 
    waveform_set_shape((WaveformChannel *)&g_channel_2, WAVEFORM_SHAPE_SQUARE);

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
    //printf("  Entering continuous waveform output.\r\n");

    printf("  Entering production device mode.\r\n");

    printf("========================================\r\n");

    /* Enter continuous waveform mode (does not return) */
    //test_continuous_waveform();

    cyw43_arch_init();

    /* Enter production device mode (does not return) */
    device_run();

    return 0;
}