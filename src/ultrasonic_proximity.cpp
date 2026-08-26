#include <Arduino.h>
#include <algorithm>
#include "config.h"
#include "tone_driver.h"
#include "ai_pipeline.h"
#include "mpu_manager.h"
#include "motion_gate.h"

#ifdef ENABLE_ULTRASONIC_HC_SR04

#ifndef US_MEASURE_MS
#define US_MEASURE_MS        60
#endif
#ifndef US_TIMEOUT_US
#define US_TIMEOUT_US        15000
#endif
#ifndef US_SPEED_CM_US
#define US_SPEED_CM_US       0.0343f
#endif
#define US_TASK_STACK        4096
#define US_TASK_PRIO         2

#define G_TO_MS2          9.80665f
#define MPU_READ_MS       20

static bool initUltrasonic(void) {
    pinMode(ULTRASONIC_TRIG, OUTPUT);
    pinMode(ULTRASONIC_ECHO, INPUT);
    digitalWrite(ULTRASONIC_TRIG, LOW);
    Serial.println("[US] Ultrasonic sensor initialized (Trig=" + String(ULTRASONIC_TRIG) + ", Echo=" + String(ULTRASONIC_ECHO) + ")");
    return true;
}

static bool initMPU6050(void) {
    return mpu_manager_init();
}

static float measure_distance_cm(void) {
    // Wait briefly if echo line is stuck HIGH
    if (digitalRead(ULTRASONIC_ECHO) == HIGH) {
        unsigned long tStart = micros();
        while (digitalRead(ULTRASONIC_ECHO) == HIGH && (micros() - tStart < 2000)) {
            delayMicroseconds(10);
        }
    }

    digitalWrite(ULTRASONIC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(ULTRASONIC_TRIG, LOW);

    unsigned long durationUs = pulseIn(ULTRASONIC_ECHO, HIGH, US_TIMEOUT_US);
    if (durationUs == 0) {
        return -1.0f;
    }

    float dist = (float)durationUs * US_SPEED_CM_US / 2.0f;
    
    // Clamp to reasonable range
    if (dist < 2.0f) dist = 2.0f;
    if (dist > 400.0f) return -1.0f;
    
    return dist;
}

static float read_sv_g(void) {
    float svG;
    if (mpu_manager_read_sv_g(&svG)) {
        return svG;
    }
    return mpu_manager_get_last_sv_g();
}

// 3-sample median + EMA filter to eliminate transient spikes
static float distance_filter(float new_dist, float *history, int *idx, float *ema) {
    history[*idx] = new_dist;
    *idx = (*idx + 1) % 3;

    float a = history[0], b = history[1], c = history[2];
    float med = a;
    if ((a <= b && b <= c) || (c <= b && b <= a)) med = b;
    else if ((b <= a && a <= c) || (c <= a && a <= b)) med = a;
    else med = c;

    if (*ema <= 0) *ema = med;
    else *ema = 0.6f * (*ema) + 0.4f * med;

    return *ema;
}

// Zone enum
typedef enum {
    ZONE_NONE = 0,      // > ZONE_SAFE or invalid
    ZONE_SAFE_ZONE,     // ZONE_WARN < D <= ZONE_SAFE
    ZONE_WARN_ZONE,     // ZONE_DANGER < D <= ZONE_WARN
    ZONE_DANGER_ZONE    // D <= ZONE_DANGER
} zone_t;

static zone_t get_zone(float dist, zone_t prev_zone) {
    if (dist < 0) return ZONE_NONE;
    
    // Apply hysteresis
    switch (prev_zone) {
        case ZONE_DANGER_ZONE:
            if (dist > ZONE_DANGER + ZONE_HYSTERESIS) return ZONE_WARN_ZONE;
            break;
        case ZONE_WARN_ZONE:
            if (dist > ZONE_WARN + ZONE_HYSTERESIS) return ZONE_SAFE_ZONE;
            if (dist <= ZONE_DANGER - ZONE_HYSTERESIS) return ZONE_DANGER_ZONE;
            break;
        case ZONE_SAFE_ZONE:
            if (dist <= ZONE_WARN - ZONE_HYSTERESIS) return ZONE_WARN_ZONE;
            if (dist > ZONE_SAFE + ZONE_HYSTERESIS) return ZONE_NONE;
            break;
        case ZONE_NONE:
            if (dist <= ZONE_SAFE - ZONE_HYSTERESIS) return ZONE_SAFE_ZONE;
            break;
    }
    return prev_zone;
}

static uint32_t get_beep_interval(zone_t zone) {
    switch (zone) {
        case ZONE_DANGER_ZONE: return INTERVAL_FAST;
        case ZONE_WARN_ZONE:   return INTERVAL_MED;
        case ZONE_SAFE_ZONE:   return INTERVAL_SLOW;
        default:               return 0;
    }
}

static const char* zone_name(zone_t zone) {
    switch (zone) {
        case ZONE_DANGER_ZONE: return "DANGER";
        case ZONE_WARN_ZONE:   return "WARN";
        case ZONE_SAFE_ZONE:   return "SAFE";
        default:               return "NONE";
    }
}

static void ultrasonic_task(void *pvParameters) {
    bool mpuOk = false;
    unsigned long lastMpuReadMs = 0;
    unsigned long lastDebugMs = 0;
    unsigned long lastBeep = 0;
    unsigned long lastMeasure = 0;
    float lastDistance = -1;
    float filteredDistance = -1;
    float dist_history[3] = {0, 0, 0};
    int hist_idx = 0;
    zone_t current_zone = ZONE_NONE;
    zone_t last_printed_zone = ZONE_NONE;

    if (!initUltrasonic()) return;
    mpuOk = initMPU6050();
    if (!mpuOk) {
        Serial.println("[US] MPU6050 init failed - motion gate disabled (always beeps)");
    }

    motion_gate_reset();

    Serial.println("[US] ===== Ultrasonic Proximity Task Running (Core 1) =====");
    Serial.printf("[US] Zones: DANGER<=%dcm | WARN<=%dcm | SAFE<=%dcm | Hyst=%dcm\n",
                  ZONE_DANGER, ZONE_WARN, ZONE_SAFE, ZONE_HYSTERESIS);
    Serial.printf("[US] Intervals: FAST=%dms | MED=%dms | SLOW=%dms\n",
                  INTERVAL_FAST, INTERVAL_MED, INTERVAL_SLOW);

    while (true) {
        unsigned long now = millis();

        // Read MPU6050 for motion gate (via shared manager)
        if (mpuOk && (now - lastMpuReadMs >= MPU_READ_MS)) {
            lastMpuReadMs = now;
            float svG = read_sv_g();
            motion_gate_update(svG);
        }

        // Measure distance
        if (now - lastMeasure >= US_MEASURE_MS) {
            lastMeasure = now;
            float raw_dist = measure_distance_cm();
            if (raw_dist > 0) {
                lastDistance = distance_filter(raw_dist, dist_history, &hist_idx, &filteredDistance);
            } else {
                lastDistance = -1;
                filteredDistance = -1;
            }
        }

        // Debug output
        #if US_DEBUG
        if (now - lastDebugMs >= US_DEBUG_INTERVAL) {
            lastDebugMs = now;
            bool gate = motion_gate_enabled();
            uint32_t interval = get_beep_interval(current_zone);
            Serial.printf("[US] Dist=%.1fcm | Zone=%s | Gate=%s | Interval=%dms | Vol=%d\n",
                          lastDistance, zone_name(current_zone),
                          gate ? "ON" : "OFF", interval, tone_driver_get_volume());
            if (current_zone != last_printed_zone) {
                Serial.printf("[US] *** ZONE CHANGE: %s -> %s ***\n",
                              zone_name(last_printed_zone), zone_name(current_zone));
                last_printed_zone = current_zone;
            }
        }
        #endif

        if (lastDistance < 0) {
            current_zone = ZONE_NONE;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Determine zone with hysteresis
        current_zone = get_zone(lastDistance, current_zone);
        uint32_t beepInterval = get_beep_interval(current_zone);

        // Motion gate check:
        // - mpuOk=true  → chỉ bíp khi gate=ON (đang di chuyển)
        // - mpuOk=false → KHÔNG bíp (an toàn hơn là spam liên tục khi không có MPU)
        bool gate = mpuOk && motion_gate_enabled();
        bool should_beep = (beepInterval > 0) && gate;

        if (should_beep && (now - lastBeep >= beepInterval)) {
            lastBeep = now;
            uint8_t vol = tone_driver_get_volume();
            
            if (current_zone == ZONE_DANGER_ZONE) {
                tone_driver_play(TONE_DANGER, 40, vol < 18 ? 18 : vol);
            } else if (current_zone == ZONE_WARN_ZONE) {
                tone_driver_play(TONE_WARNING, 35, vol);
            } else if (current_zone == ZONE_SAFE_ZONE) {
                tone_driver_play(TONE_SLOW, 30, vol);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ultrasonic_task_start(void) {
    xTaskCreatePinnedToCore(ultrasonic_task, "us_prox", US_TASK_STACK,
                            NULL, US_TASK_PRIO, NULL, 1);
}

#endif