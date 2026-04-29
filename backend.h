#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialise the signal generation backend.
 * Calls dac8411_init(), waveform_init_lut(), and sets default
 * frequencies, amplitudes, and shapes on both channels.
 * Must be called once before any other backend_* function.
 */
void backend_init(void);

/**
 * Set the output frequency for a channel.
 * @param channel  1 or 2
 * @param freq_hz  Frequency in Hz (e.g. 100.0 – 10000.0)
 */
void backend_set_channel_frequency(uint8_t channel, float freq_hz);

/**
 * Set the output amplitude for a channel.
 * @param channel    1 or 2
 * @param amplitude  0–65535 (65535 = full scale)
 */
void backend_set_channel_amplitude(uint8_t channel, uint16_t amplitude);

/**
 * Enable or disable a channel's waveform output.
 * @param channel  1 or 2
 * @param enable   true to enable, false to disable (holds last phase)
 */
void backend_enable_channel(uint8_t channel, bool enable);

#endif /* BACKEND_H */
