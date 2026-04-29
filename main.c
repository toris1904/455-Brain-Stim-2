/* main.c
 *
 * Combined entry point: launches the LUT-based dual-channel waveform engine
 * and the Wi-Fi hotspot simultaneously.
 *
 * Architecture:
 *   - A repeating hardware timer ISR fires every 10 µs (100 kHz sample rate),
 *     computing DAC codes for both channels and setting g_sample_ready.
 *   - The main loop services DAC SPI writes and calls cyw43_arch_poll() on
 *     every iteration to keep the Wi-Fi/lwIP stack running.
 *   - g_channel_1 and g_channel_2 are shared globals; wifi_hotspot.c calls
 *     waveform_set_frequency/amplitude/enabled on them directly.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/timer.h"
#include "hardware/gpio.h"

#include "waveform.h"
#include "dac8411.h"
#include "wifi_hotspot.h"

/* --------------------------------------------------------------------------
 * Pin assignments
 * -------------------------------------------------------------------------- */
#define PIN_TICK_PROBE  20u
#define PIN_BEAT_PROBE  21u
#define PIN_TOGGLE_BTN  9u
#define PIN_HARD_KILL_BTN 10u
#define PIN_ON 11u
#define PIN_OFF 12u


/* --------------------------------------------------------------------------
 * Pulse helper — drives a GPIO high for 1 ms then returns it low.
 * Only call from main-loop context (not ISR).
 * -------------------------------------------------------------------------- */
static void send_pulse(uint pin)
{
    gpio_put(pin, 1);
    sleep_us(10000);  /* 10 ms pulse */
    gpio_put(pin, 0);
}

/* Tracks which output pin was pulsed most recently (PIN_ON or PIN_OFF).
 * Starts as PIN_OFF so the first hard-kill press sends a PIN_ON pulse. */
static uint g_last_pin_sent = PIN_OFF;

/* --------------------------------------------------------------------------
 * Ramp configuration  (5-second linear amplitude ramp, 10 ms step)
 * -------------------------------------------------------------------------- */
#define RAMP_DURATION_MS      5000u
#define RAMP_STEP_INTERVAL_MS 10u
#define RAMP_STEPS            (RAMP_DURATION_MS / RAMP_STEP_INTERVAL_MS)
#define RAMP_STEP_DELTA       (1.0f / (float)RAMP_STEPS)

/* --------------------------------------------------------------------------
 * Button debounce
 *
 * Active-low buttons with internal pull-ups on PIN_ON_BTN / PIN_KILL_BTN.
 * button_poll() returns true exactly once per confirmed press.
 * -------------------------------------------------------------------------- */
#define BTN_DEBOUNCE_US 20000u

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
 * Waveform state  (written by ISR, read by main loop)
 * -------------------------------------------------------------------------- */
volatile WaveformChannel g_channel_1;
volatile WaveformChannel g_channel_2;
static volatile uint16_t g_pending_code_ch1 = DAC_MIDPOINT;
static volatile uint16_t g_pending_code_ch2 = DAC_MIDPOINT;
static volatile bool     g_sample_ready     = false;

/* --------------------------------------------------------------------------
 * Device state machine  (written by wifi_hotspot callbacks, read by main loop)
 * -------------------------------------------------------------------------- */
volatile DeviceState g_device_state = DEVICE_STATE_OFF;
static float         g_amplitude    = 0.0f;
static uint64_t      g_last_ramp_us = 0u;

void device_request_start(void)
{
    if (g_device_state == DEVICE_STATE_OFF) {
        g_amplitude    = 0.0f;
        waveform_set_amplitude((WaveformChannel *)&g_channel_1, 0.0f);
        waveform_set_amplitude((WaveformChannel *)&g_channel_2, 0.0f);
        waveform_set_enabled((WaveformChannel *)&g_channel_1, true);
        waveform_set_enabled((WaveformChannel *)&g_channel_2, true);
        g_last_ramp_us = time_us_64();
        g_device_state = DEVICE_STATE_RAMPING_UP;
        send_pulse(PIN_ON);
        g_last_pin_sent = PIN_ON;
    }
}

void device_request_stop(void)
{
    if (g_device_state == DEVICE_STATE_RUNNING ||
        g_device_state == DEVICE_STATE_RAMPING_UP) {
        g_last_ramp_us = time_us_64();
        g_device_state = DEVICE_STATE_RAMPING_DOWN;
        send_pulse(PIN_OFF);
        g_last_pin_sent = PIN_OFF;
    }
}

/* --------------------------------------------------------------------------
 * Hardware timer ISR — compute only, no SPI, no blocking calls.
 * Target execution time: < 2 µs.
 * -------------------------------------------------------------------------- */
static bool timer_isr(struct repeating_timer *rt)
{
    (void)rt;
    gpio_xor_mask(1u << PIN_TICK_PROBE);
    g_pending_code_ch1 = waveform_tick((WaveformChannel *)&g_channel_1);
    g_pending_code_ch2 = waveform_tick((WaveformChannel *)&g_channel_2);
    g_sample_ready = true;
    return true;
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(void)
{
    stdio_init_all();
    sleep_ms(2000);

    printf("\r\n========================================\r\n");
    printf("  Pico W-2 Brain Stimulator\r\n");
    printf("  Dual-Channel Waveform + Wi-Fi Hotspot\r\n");
    printf("========================================\r\n\r\n");

    /* Probe GPIO setup */
    gpio_init(PIN_TICK_PROBE);
    gpio_set_dir(PIN_TICK_PROBE, GPIO_OUT);
    gpio_init(PIN_BEAT_PROBE);
    gpio_set_dir(PIN_BEAT_PROBE, GPIO_OUT);

    /* Button GPIO setup — active-low, internal pull-up */
    gpio_init(PIN_TOGGLE_BTN);
    gpio_set_dir(PIN_TOGGLE_BTN, GPIO_IN);
    gpio_pull_up(PIN_TOGGLE_BTN);
    gpio_init(PIN_HARD_KILL_BTN);
    gpio_set_dir(PIN_HARD_KILL_BTN, GPIO_IN);
    gpio_pull_up(PIN_HARD_KILL_BTN);

    /* Output pulse pins — idle low */
    gpio_init(PIN_ON);
    gpio_set_dir(PIN_ON, GPIO_OUT);
    gpio_put(PIN_ON, 0);
    gpio_init(PIN_OFF);
    gpio_set_dir(PIN_OFF, GPIO_OUT);
    gpio_put(PIN_OFF, 0);

    /* Initialise the CYW43 Wi-Fi chip (also needed for the onboard LED) */
    if (cyw43_arch_init()) {
        printf("ERROR: Failed to initialise cyw43\r\n");
        return 1;
    }
    printf("CYW43 initialised.\r\n");

    /* Initialise DAC and LUT waveform engine */
    dac8411_init();
    waveform_init_lut();
    waveform_set_frequency((WaveformChannel *)&g_channel_1, 100.0f);
    waveform_set_frequency((WaveformChannel *)&g_channel_2, 100.0f);
    waveform_set_amplitude((WaveformChannel *)&g_channel_1, 1.0f);
    waveform_set_amplitude((WaveformChannel *)&g_channel_2, 1.0f);
    waveform_set_shape((WaveformChannel *)&g_channel_1, WAVEFORM_SHAPE_SINE);
    waveform_set_shape((WaveformChannel *)&g_channel_2, WAVEFORM_SHAPE_SINE);
    g_channel_1.enabled     = false;
    g_channel_2.enabled     = false;
    g_channel_1.phase_accum = 0;
    g_channel_2.phase_accum = 0;
    printf("DAC and waveform LUT initialised.\r\n");

    /* Start hardware timer ISR at 100 kHz */
    struct repeating_timer timer;
    add_repeating_timer_us(-10, timer_isr, NULL, &timer);
    printf("100 kHz sample timer started.\r\n");

    /* Start the Wi-Fi access point, DHCP/DNS servers, and TCP web server */
    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
    if (!state) {
        printf("ERROR: Failed to allocate TCP server state\r\n");
        return 1;
    }

    if (!wifi_hotspot_start(state)) {
        printf("ERROR: Failed to start Wi-Fi hotspot\r\n");
        free(state);
        return 1;
    }
    printf("Wi-Fi hotspot running. Connect to 'picow_test' (password: password)\r\n");
    printf("Browse to http://192.168.4.1/ to control the device.\r\n\r\n");

    /* Combined main loop:
     *   1. Service DAC writes immediately when the ISR signals g_sample_ready.
     *   2. Poll the CYW43 Wi-Fi/lwIP stack on every iteration so HTTP
     *      requests are processed without a dedicated Wi-Fi task or RTOS.
     *
     * cyw43_arch_poll() returns quickly when there is nothing to do, so it
     * does not meaningfully delay DAC servicing between ISR wakeups. */
    while (!state->complete) {
        /* 1. Service DAC writes — highest priority */
        if (g_sample_ready) {
            dac8411_write(0, g_pending_code_ch1);
            dac8411_write(1, g_pending_code_ch2);
            g_sample_ready = false;
        }

        /* 2. Physical button polling — GP9 toggles start/stop.
         *    Ignored while ramping to prevent mid-ramp state changes.
         *    GP10 hard-kill: immediately stops waveform and pulses PIN_OFF. */
        static ButtonState toggle_btn    = { PIN_TOGGLE_BTN,    0u, false, false };
        static ButtonState hard_kill_btn = { PIN_HARD_KILL_BTN, 0u, false, false };
        if (button_poll(&toggle_btn)) {
            if (g_device_state == DEVICE_STATE_OFF) {
                device_request_start();
            } else if (g_device_state == DEVICE_STATE_RUNNING) {
                device_request_stop();
            }
        }
        if (button_poll(&hard_kill_btn)) {
            /* Immediate hard stop — no ramp */
            g_amplitude = 0.0f;
            waveform_set_amplitude((WaveformChannel *)&g_channel_1, 0.0f);
            waveform_set_amplitude((WaveformChannel *)&g_channel_2, 0.0f);
            waveform_set_enabled((WaveformChannel *)&g_channel_1, false);
            waveform_set_enabled((WaveformChannel *)&g_channel_2, false);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            g_device_state = DEVICE_STATE_OFF;
            send_pulse(PIN_OFF);
            g_last_pin_sent = PIN_OFF;
        }

        /* 3. Non-blocking ramp state machine */
        uint64_t now = time_us_64();
        switch (g_device_state) {

            case DEVICE_STATE_RAMPING_UP:
                if ((now - g_last_ramp_us) >=
                        (uint64_t)RAMP_STEP_INTERVAL_MS * 1000u) {
                    g_last_ramp_us = now;
                    g_amplitude   += RAMP_STEP_DELTA;
                    if (g_amplitude >= 1.0f) {
                        g_amplitude = 1.0f;
                        waveform_set_amplitude((WaveformChannel *)&g_channel_1, g_amplitude);
                        waveform_set_amplitude((WaveformChannel *)&g_channel_2, g_amplitude);
                        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
                        g_device_state = DEVICE_STATE_RUNNING;
                        printf("Ramp-up complete — running at full amplitude.\r\n");
                    } else {
                        waveform_set_amplitude((WaveformChannel *)&g_channel_1, g_amplitude);
                        waveform_set_amplitude((WaveformChannel *)&g_channel_2, g_amplitude);
                    }
                }
                break;

            case DEVICE_STATE_RAMPING_DOWN:
                if ((now - g_last_ramp_us) >=
                        (uint64_t)RAMP_STEP_INTERVAL_MS * 1000u) {
                    g_last_ramp_us = now;
                    g_amplitude   -= RAMP_STEP_DELTA;
                    if (g_amplitude <= 0.0f) {
                        g_amplitude = 0.0f;
                        waveform_set_amplitude((WaveformChannel *)&g_channel_1, g_amplitude);
                        waveform_set_amplitude((WaveformChannel *)&g_channel_2, g_amplitude);
                        waveform_set_enabled((WaveformChannel *)&g_channel_1, false);
                        waveform_set_enabled((WaveformChannel *)&g_channel_2, false);
                        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
                        g_device_state = DEVICE_STATE_OFF;
                        printf("Ramp-down complete — device is OFF.\r\n");
                    } else {
                        waveform_set_amplitude((WaveformChannel *)&g_channel_1, g_amplitude);
                        waveform_set_amplitude((WaveformChannel *)&g_channel_2, g_amplitude);
                    }
                }
                break;

            default:
                break;
        }

        /* 3. Poll Wi-Fi/lwIP stack */
        cyw43_arch_poll();
    }

    /* Cleanup (reached when user presses 'd' in the serial terminal) */
    cancel_repeating_timer(&timer);
    wifi_hotspot_stop(state);
    cyw43_arch_deinit();
    free(state);

    printf("Shutdown complete.\r\n");
    return 0;
}
