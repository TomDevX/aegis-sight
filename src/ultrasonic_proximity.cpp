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

    // Timeout 10000us (~171cm) - đo nhạy tầm xa đến 85-120cm
    unsigned long durationUs = pulseIn(ULTRASONIC_ECHO, HIGH, 10000);
    if (durationUs == 0) {
        return -1.0f;
    }

    float dist = (float)durationUs * US_SPEED_CM_US / 2.0f;
    if (dist < 2.0f) dist = 2.0f;
    if (dist > 140.0f) return -1.0f;
    return dist;
}

// 3-sample median + EMA filter with Sample-and-Hold (phản hồi tức thì)
static float distance_filter(float new_dist, float *history, int *idx, float *ema) {
    if (*ema <= 0) {
        // Lần đầu bắt được phản xạ sau khoảng trống: nạp ngay giá trị mới để phản hồi tức thì
        history[0] = new_dist;
        history[1] = new_dist;
        history[2] = new_dist;
        *idx = 0;
        *ema = new_dist;
        return new_dist;
    }

    history[*idx] = new_dist;
    *idx = (*idx + 1) % 3;

    float a = history[0], b = history[1], c = history[2];
    float med = a;
    if ((a <= b && b <= c) || (c <= b && b <= a)) med = b;
    else if ((b <= a && a <= c) || (c <= a && a <= b)) med = a;
    else med = c;

    // EMA phản hồi nhanh 70% mẫu mới, 30% mẫu cũ
    *ema = 0.7f * med + 0.3f * (*ema);

    return *ema;
}

// Zone enum
typedef enum {
    ZONE_NONE = 0,
    ZONE_SAFE_ZONE,     // 50-85cm (báo sớm từ xa)
    ZONE_WARN_ZONE,     // 25-50cm (chuẩn bị né)
    ZONE_DANGER_ZONE    // <=25cm (rất gần)
} zone_t;

static zone_t get_zone(float dist, zone_t prev_zone) {
    if (dist <= 0 || dist > (ZONE_SAFE + ZONE_HYSTERESIS)) return ZONE_NONE;
    
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
    zone_t prevDistanceZone = ZONE_NONE;
    float stationaryRefDist = -1.0f;
    unsigned long stationarySinceMs = 0;
    bool isStationaryMuted = false;

    if (!initUltrasonic()) return;
    mpu_manager_init();

    Serial.println("[SENSOR] ===== Unified Real-Time Sensor Task Running (Core 1) =====");

    while (true) {
        unsigned long now = millis();

        // 1. Bước 1: Đọc MPU6050 (Phục vụ Fall Detection - không dùng cho siêu âm nữa)
        float ax = 0, ay = 0, az = 0, svG = 1.0f;
        float gx = 0, gy = 0, gz = 0, gyroDegS = 0.0f;
        bool mpuOk = mpu_manager_read_motion(&ax, &ay, &az, &svG, &gx, &gy, &gz, &gyroDegS);
        if (mpuOk) {
            fall_detection_process_sample(ax, ay, az, svG, gyroDegS);
        }

        // Duy trì còi hú cứu hộ SOS nếu đang có cảnh báo ngã
        fall_detection_alarm_tick();

        // 2. Bước 2: Đo khoảng cách siêu âm (tối đa 130cm để đón đầu từ 85cm)
        float raw_dist = measure_distance_cm();
        if (raw_dist > 0 && raw_dist <= 130.0f) {
            lastDistance = distance_filter(raw_dist, dist_history, &hist_idx, &filteredDistance);
            lastValidDistMs = now;
        } else {
            if (now - lastValidDistMs > 180) {
                lastDistance = -1;
                filteredDistance = -1;
            }
        }

        // 3. Debug log định kỳ mỗi 2 giây
        #if US_DEBUG
        if (now - lastDebugMs >= US_DEBUG_INTERVAL) {
            lastDebugMs = now;
            String distStr = (lastDistance > 0) ? (String(lastDistance, 1) + "cm") : "CLEAR (>85cm)";
            const char* beepStatus = isStationaryMuted ? "MUTED(STILL)" : ((current_zone != ZONE_NONE) ? "BEEPING" : "CLEAR");
            
            if (mpuOk) {
                Serial.printf("[%6.2fs][SENSOR] Dist=%s | Zone=%s | UltraBeep=%s | MPU: SV=%.2fG (ax=%.2f ay=%.2f az=%.2f)\n",
                              now / 1000.0f, distStr.c_str(), zone_name(current_zone),
                              beepStatus, svG, ax, ay, az);
            } else {
                Serial.printf("[%6.2fs][SENSOR] Dist=%s | Zone=%s | UltraBeep=%s | MPU: [DISCONNECTED / Check Wire]\n",
                              now / 1000.0f, distStr.c_str(), zone_name(current_zone),
                              beepStatus);
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

            // Xử lý tự động ngắt tiếng khi đứng lại (khoảng cách chỉ dao động trong tầm <= 5cm):
            if (current_zone != ZONE_NONE) {
                if (stationaryRefDist < 0) {
                    // Mới phát hiện vật cản trong zone lần đầu -> Bíp ngay lập tức không trễ!
                    stationaryRefDist = lastDistance;
                    stationarySinceMs = now;
                    isStationaryMuted = false;
                    lastBeep = now - beepInterval;
                } else {
                    // Nếu người dùng di chuyển lại gần hơn (khoảng cách giảm >= US_APPROACH_DELTA_CM)
                    if (lastDistance <= (stationaryRefDist - US_APPROACH_DELTA_CM)) {
                        if (isStationaryMuted) {
                            Serial.printf("[%6.2fs][US] Moving closer (%.1fcm -> %.1fcm) -> Resuming beep!\n",
                                          now / 1000.0f, stationaryRefDist, lastDistance);
                            lastBeep = now - beepInterval; // Bíp ngay lập tức khi tiến gần hơn
                        }
                        isStationaryMuted = false;
                        stationaryRefDist = lastDistance;
                        stationarySinceMs = now;
                    }
                    // Nếu lùi ra xa hoặc đổi hướng (khoảng cách tăng >= US_STATIONARY_DELTA_CM)
                    else if (lastDistance >= (stationaryRefDist + US_STATIONARY_DELTA_CM)) {
                        stationaryRefDist = lastDistance;
                        stationarySinceMs = now;
                        isStationaryMuted = false;
                    }
                    // Nếu khoảng cách chỉ dao động trong phạm vi <= 5cm (đứng yên trước vật cản)
                    else {
                        if (!isStationaryMuted && (now - stationarySinceMs >= US_STATIONARY_MUTE_MS)) {
                            isStationaryMuted = true;
                            Serial.printf("[%6.2fs][US] User stationary at %.1fcm (delta <= %.1fcm for %ums) -> Beep paused\n",
                                          now / 1000.0f, lastDistance, US_STATIONARY_DELTA_CM, US_STATIONARY_MUTE_MS);
                        }
                    }

                    // Nếu chuyển sang zone nguy hiểm hơn (ví dụ SAFE -> WARN, hoặc WARN -> DANGER) thì kêu lại ngay
                    if (current_zone > prevDistanceZone && isStationaryMuted) {
                        isStationaryMuted = false;
                        stationaryRefDist = lastDistance;
                        stationarySinceMs = now;
                        lastBeep = now - beepInterval; // Bíp ngay lập tức
                        Serial.printf("[%6.2fs][US] Zone escalated to %s -> Resuming beep!\n",
                                      now / 1000.0f, zone_name(current_zone));
                    }
                }
            } else {
                stationaryRefDist = -1.0f;
                stationarySinceMs = 0;
                isStationaryMuted = false;
            }
            prevDistanceZone = current_zone;

            // PHÁT TIẾNG BÍP KHI:
            // 1. Không bận AI (!ai_pipeline_is_busy())
            // 2. Không bị tạm ngắt do đứng yên (!isStationaryMuted)
            // 3. Đang có vật cản trong zone (current_zone != ZONE_NONE)
            if (!ai_pipeline_is_busy() && !isStationaryMuted && current_zone != ZONE_NONE && beepInterval > 0 && (now - lastBeep >= beepInterval)) {
                lastBeep = now;
                
                // Âm lượng bíp siêu âm nhỏ gọn, êm dịu, thoải mái cho người nghe
                if (current_zone == ZONE_DANGER_ZONE) {
                    tone_driver_play(TONE_DANGER, 80, US_VOL_DANGER); // 880Hz, 80ms
                } else if (current_zone == ZONE_WARN_ZONE) {
                    tone_driver_play(TONE_WARNING, 70, US_VOL_WARN);  // 740Hz, 70ms
                } else if (current_zone == ZONE_SAFE_ZONE) {
                    tone_driver_play(TONE_SLOW, 60, US_VOL_SAFE);     // 587Hz, 60ms
                }
            }
        } else {
            current_zone = ZONE_NONE;
            prevDistanceZone = ZONE_NONE;
            stationaryRefDist = -1.0f;
            stationarySinceMs = 0;
            isStationaryMuted = false;
        }

        // 4. Bước 4: Nghỉ 25ms (tần số lấy mẫu 40Hz bảo đảm độ nhạy phát hiện té ngã)
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void ultrasonic_task_start(void) {
    xTaskCreatePinnedToCore(ultrasonic_task, "unified_sensors", US_TASK_STACK,
                            NULL, US_TASK_PRIO, NULL, 1);
}

#endif