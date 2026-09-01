#ifndef TTS_DRIVER_H
#define TTS_DRIVER_H

#include <Arduino.h>
#include "config.h"

#ifdef ENABLE_TTS_CLOUD

bool tts_driver_init(void);
void tts_driver_speak(const char *text, size_t len);
void tts_driver_play_progmem(const uint8_t *data, size_t len);
bool tts_driver_is_busy(void);
void tts_driver_stop(void);
void tts_driver_wait_playback_done(void);

#else

static inline bool tts_driver_init(void) { return true; }
static inline void tts_driver_speak(const char *text, size_t len) { (void)text; (void)len; }
static inline void tts_driver_play_progmem(const uint8_t *data, size_t len) { (void)data; (void)len; }
static inline bool tts_driver_is_busy(void) { return false; }
static inline void tts_driver_stop(void) {}
static inline void tts_driver_wait_playback_done(void) {}

#endif

#endif
