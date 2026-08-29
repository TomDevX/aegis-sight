#include <Arduino.h>
#include <algorithm>
#include "config.h"
#include "tone_driver.h"
#include "ai_pipeline.h"
#include "mpu_manager.h"
#include "motion_gate.h"
#include "fall_detection.h"

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

static float measure_distance_cm(void) {
    if (digitalRead(ULTRASONIC_ECHO) == HIGH) {
        unsigned long tStart = micros();
        while (digitalRead(ULTRASONIC_ECHO) == HIGH && (micros() - tStart < 1000)) {
            delayMicroseconds(5);
        }
    }

    digitalWrite(ULTRASONIC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(ULTRASONIC_TRIG, LOW);

    // Timeout 8000us (~135cm) - đủ cho dải cảnh báo 60cm, nhường CPU
    unsigned long durationUs = pulseIn(ULTRASONIC_ECHO, HIGH, 8000);
    if (durationUs == 0) {
        return -1.0f;
    }

    float dist = (float)durationUs * US_SPEED_CM_US / 2.0f;
    if (dist < 2.0f) dist = 2.0f;
    if (dist > 120.0f) return -1.0f;
    return dist;
}

// 3-sample median + EMA filter with Sample-and-Hold
static float distance_filter(float new_dist, float *history, int *idx, float *ema) {
    history[*idx] = new_dist;
    *idx = (*idx + 1) % 3;

    float a = history[0], b = history[1], c = history[2];
    float med = a;
    if ((a <= b && b <= c) || (c <= b && b <= a)) med = b;
    else if ((b <= a && a <= c) || (c <= a && a <= b)) med = a;
    else med = c;

    if (*ema <= 0) *ema = med;
    else *ema = 0.5f * (*ema) + 0.5f * med;

    return *ema;
}

// Zone enum
typedef enum {
    ZONE_NONE = 0,
    ZONE_SAFE_ZONE,     // 32-45cm
    ZONE_WARN_ZONE,     // 18-32cm
    ZONE_DANGER_ZONE    // <=18cm
} zone_t;

static zone_t get_zone(float dist, zone_t prev_zone) {
    if (dist <= 0 || dist > 48.0f) return ZONE_NONE;
    
    switch (prev_zone) {
        case ZONE_DANGER_ZONE:
            if (dist > ZONE_DANGER + 2) return ZONE_WARN_ZONE;
            break;
        case ZONE_WARN_ZONE:
            if (dist > ZONE_WARN + 2) return ZONE_SAFE_ZONE;
            if (dist <= ZONE_DANGER - 2) return ZONE_DANGER_ZONE;
            break;
        case ZONE_SAFE_ZONE:
            if (dist <= ZONE_WARN - 2) return ZONE_WARN_ZONE;
            if (dist > ZONE_SAFE + 2) return ZONE_NONE;
            break;
        case ZONE_NONE:
            if (dist <= ZONE_DANGER) return ZONE_DANGER_ZONE;
            if (dist <= ZONE_WARN) return ZONE_WARN_ZONE;
            if (dist <= ZONE_SAFE) return ZONE_SAFE_ZONE;
            break;
    }
    return prev_zone;
}

static uint32_t get_beep_interval(zone_t zone) {
    switch (zone) {
        case ZONE_DANGER_ZONE: return INTERVAL_FAST; // 180ms
        case ZONE_WARN_ZONE:   return INTERVAL_MED;  // 320ms
        case ZONE_SAFE_ZONE:   return INTERVAL_SLOW; // 550ms
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

// ============================================================
// Core 1: Unified Real-Time Sensor Task (Interleaved Execution)
// Tuần tự: 1. Đọc MPU6050 -> 2. Đo Siêu Âm -> 3. Phát Tone
// Triệt tiêu 100% hiện tượng xung đột I2C và sóng siêu âm!
// ============================================================
static void ultrasonic_task(void *pvParameters) {
    unsigned long lastDebugMs = 0;
    unsigned long lastBeep = 0;
    unsigned long lastValidDistMs = 0;
    float lastDistance = -1;
    float filteredDistance = -1;
    float dist_history[3] = {0, 0, 0};
    int hist_idx = 0;
    zone_t current_zone = ZONE_NONE;
    zone_t last_printed_zone = ZONE_NONE;

    if (!initUltrasonic()) return;
    mpu_manager_init();

    Serial.println("[SENSOR] ===== Unified Real-Time Sensor Task Running (Core 1) =====");

    while (true) {
        unsigned long now = millis();

        // 1. Bước 1: Đọc MPU6050 (Lúc này chân Trig siêu âm hoàn toàn im lặng, I2C đọc mượt 100%)
        float ax = 0, ay = 0, az = 0, svG = 1.0f;
        bool mpuOk = mpu_manager_read_accel_g(&ax, &ay, &az, &svG);
        if (mpuOk) {
            fall_detection_process_sample(ax, ay, az, svG);
        }

        // Duy trì còi hú cứu hộ SOS nếu đang có cảnh báo ngã
        fall_detection_alarm_tick();

        // 2. Bước 2: Đo khoảng cách siêu âm (Lúc này bus I2C hoàn toàn tĩnh lặng)
        float raw_dist = measure_distance_cm();
        if (raw_dist > 0 && raw_dist <= 100.0f) {
            lastDistance = distance_filter(raw_dist, dist_history, &hist_idx, &filteredDistance);
            lastValidDistMs = now;
        } else {
            if (now - lastValidDistMs > 200) {
                lastDistance = -1;
                filteredDistance = -1;
            }
        }

        // 3. Debug log định kỳ mỗi 2 giây
        #if US_DEBUG
        if (now - lastDebugMs >= US_DEBUG_INTERVAL) {
            lastDebugMs = now;
            String distStr = (lastDistance > 0) ? (String(lastDistance, 1) + "cm") : "CLEAR (>45cm)";
            
            if (mpuOk) {
                Serial.printf("[%6.2fs][SENSOR] Dist=%s | Zone=%s | MPU: SV=%.2fG (ax=%.2f ay=%.2f az=%.2f)\n",
                              now / 1000.0f, distStr.c_str(), zone_name(current_zone), svG, ax, ay, az);
            } else {
                Serial.printf("[%6.2fs][SENSOR] Dist=%s | Zone=%s | MPU: [DISCONNECTED / Check Wire]\n",
                              now / 1000.0f, distStr.c_str(), zone_name(current_zone));
            }

            if (current_zone != last_printed_zone) {
                Serial.printf("[%6.2fs][US] *** ZONE CHANGE: %s -> %s ***\n",
                              now / 1000.0f, zone_name(last_printed_zone), zone_name(current_zone));
                last_printed_zone = current_zone;
            }
        }
        #endif

        if (lastDistance > 0) {
            current_zone = get_zone(lastDistance, current_zone);
            uint32_t beepInterval = get_beep_interval(current_zone);

            if (beepInterval > 0 && (now - lastBeep >= beepInterval)) {
                lastBeep = now;
                uint8_t vol = tone_driver_get_volume();
                if (vol < 18) vol = 20;
                
                if (current_zone == ZONE_DANGER_ZONE) {
                    tone_driver_play(TONE_DANGER, 120, 21); // 880Hz, 120ms (nốt còi ngã tròn đầy)
                } else if (current_zone == ZONE_WARN_ZONE) {
                    tone_driver_play(TONE_WARNING, 110, vol); // 740Hz, 110ms
                } else if (current_zone == ZONE_SAFE_ZONE) {
                    tone_driver_play(TONE_SLOW, 100, vol); // 587Hz, 100ms
                }
            }
        } else {
            current_zone = ZONE_NONE;
        }

        // 4. Bước 4: Nghỉ 25ms (tần số lấy mẫu 40Hz siêu mượt và nhẹ tải CPU)
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void ultrasonic_task_start(void) {
    xTaskCreatePinnedToCore(ultrasonic_task, "unified_sensors", US_TASK_STACK,
                            NULL, US_TASK_PRIO, NULL, 1);
}

#endif