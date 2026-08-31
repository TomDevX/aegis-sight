#ifndef TONE_DRIVER_H
#define TONE_DRIVER_H

#include <Arduino.h>
#include "config.h"
#include "esp_timer.h"

#define TONE_SAMPLE_RATE  24000   // 24kHz — Khớp 100% Native MP3 từ Google TTS (không cần resample)
#define TONE_BITS         16
#define TONE_BUF_SAMPLES  512

typedef struct {
    uint16_t freqHz;
    uint32_t durationMs;
    uint8_t  volume;
} tone_request_t;

bool    tone_driver_init(void);
void    tone_driver_start_task(void);
bool    tone_driver_play(uint16_t freqHz, uint32_t durationMs, uint8_t volume);
void    tone_driver_stop(void);
bool    tone_driver_is_playing(void);
void    tone_driver_set_volume(uint8_t vol);
uint8_t tone_driver_get_volume(void);
void    tone_driver_set_sample_rate(uint32_t rate);

// --- Sound Effects & Airplane Chimes ---
void    tone_driver_play_startup(void);
void    tone_driver_play_captain_chime(void);
void    tone_driver_play_quick_beep(void);
void    tone_driver_play_double_beep(void);
void    tone_driver_play_release_chime(void);

void    tone_driver_stream_init(void);
bool    tone_driver_stream_write(const int16_t *data, size_t samples);
void    tone_driver_stream_set_active(bool active);
bool    tone_driver_stream_is_active(void);
size_t  tone_driver_stream_available(void);

// --- Zero-CPU Pre-decoded PSRAM Waiting Music ---
void    tone_driver_predecode_waiting_music(const uint8_t *mp3Data, size_t mp3Len);
void    tone_driver_waiting_music_set(bool active);
bool    tone_driver_waiting_music_is_active(void);

#endif
