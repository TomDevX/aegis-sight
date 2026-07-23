#ifndef AI_PIPELINE_H
#define AI_PIPELINE_H

#include <Arduino.h>

void ai_pipeline_start(void);
void ai_pipeline_stop(void);
bool ai_pipeline_is_busy(void);

void ai_pipeline_net_task_start(void);    // Core 0: HTTP REST + Wi-Fi
void ai_pipeline_audio_task_start(void);  // Core 1: record mic + I2S playback

#endif
