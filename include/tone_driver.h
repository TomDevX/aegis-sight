#ifndef TONE_DRIVER_H
#define TONE_DRIVER_H

#include <Arduino.h>
#include "config.h"
#include "driver/i2s.h"

// ============================================================
// Tone Driver - Shared I2S TX PCM5102A for Beep/SOS/AI Audio
// Manages I2S Channel 1 (TX) exclusively. All features that
// produce sound go through this driver.
// ============================================================

#define TONE_SAMPLE_RATE  16000
#define TONE_BITS         16
#define TONE_BUF_SAMPLES  512

// Tone request structure - queued by sensor tasks
typedef struct {
    uint16_t freqHz;       // Frequency in Hz (0 = silence)
    uint32_t durationMs;   // Total duration in ms
    uint8_t  volume;       // 0-21 (maps to ESP32-audioI2S range)
} tone_request_t;

// Initialize I2S TX channel + allocate PSRAM buffer
bool tone_driver_init(void);

// Start tone generation task (runs on Core 1)
void tone_driver_start_task(void);

// Non-blocking tone request: fills a queue, task picks up
// Returns true if queued, false if queue full
bool tone_driver_play(uint16_t freqHz, uint32_t durationMs, uint8_t volume);

// Emergency stop (used by fall cancel button)
void tone_driver_stop(void);

// Check if tone is currently playing
bool tone_driver_is_playing(void);

// Direct volume change (for auto-volume adjust)
void tone_driver_set_volume(uint8_t vol);

// Get current volume level
uint8_t tone_driver_get_volume(void);

#endif // TONE_DRIVER_H
