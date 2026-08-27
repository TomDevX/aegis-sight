#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "fall_detection.h"
#include "tone_driver.h"
#include "ai_pipeline.h"
#include "mpu_manager.h"

#ifdef ENABLE_MPU6050_FALL_DETECTION

enum FallPhase {
    FALL_IDLE,
    FALL_FREE_FALL,
    FALL_IMPACT,
    FALL_WAITING_STILL
};

static FallPhase fallPhase = FALL_IDLE;
static uint32_t phaseTimer = 0;
static volatile bool alarmActive = false;
static uint32_t cancelCooldown = 0;
static uint32_t lastStillPrintMs = 0;
static uint32_t lastSosAlarmBeepMs = 0;
static uint8_t freeFallSamples = 0;
static uint8_t movingSamples = 0;

static void evaluate_fall_state(float ax, float ay, float az, float a_total) {
    if (ai_pipeline_is_busy() || (millis() - cancelCooldown < FALL_DEBOUNCE_MS)) {
        fallPhase = FALL_IDLE;
        freeFallSamples = 0;
        movingSamples = 0;
        return;
    }

    uint32_t now = millis();

    switch (fallPhase) {
        case FALL_IDLE:
            // 1. Nhánh 1: Rơi tự do / thả rơi trong không gian (0.15G <= a_total < 0.65G)
            if (a_total >= 0.15f && a_total < 0.65f) {
                fallPhase = FALL_FREE_FALL;
                phaseTimer = now;
                freeFallSamples = 0;
                movingSamples = 0;
                Serial.printf("\n[FALL] >>> Phase 1: Free Fall DETECTED (SV=%.2fG) <<<\n", a_total);
            }
            // 2. Nhánh 2: Ném mạnh xuống / Quật ngã / Va đập chấn động dứt khoát (a_total > 2.20G)
            else if (a_total > 2.20f) {
                fallPhase = FALL_IMPACT;
                phaseTimer = now;
                freeFallSamples = 0;
                movingSamples = 0;
                Serial.printf("\n[FALL] >>> Direct Throw / High-G Impact DETECTED (SV=%.2fG) <<<\n", a_total);
            }
            break;

        case FALL_FREE_FALL:
            // Pha 2: Chờ va chạm tiếp đất (> 1.30G) trong cửa sổ 1000ms sau cú rơi
            if (a_total > 1.30f) {
                fallPhase = FALL_IMPACT;
                phaseTimer = now;
                freeFallSamples = 0;
                movingSamples = 0;
                Serial.printf("[FALL] >>> Phase 2: Landing Impact DETECTED (SV=%.2fG) <<<\n", a_total);
            } else if (now - phaseTimer > 1000) { 
                fallPhase = FALL_IDLE;
                freeFallSamples = 0;
            }
            break;

        case FALL_IMPACT:
            // Đợi 200ms để triệt tiêu dao động nảy ban đầu sau cú va đập
            if (now - phaseTimer > 200) {
                fallPhase = FALL_WAITING_STILL;
                phaseTimer = now;
                lastStillPrintMs = now;
                movingSamples = 0;
                Serial.println("[FALL] >>> Phase 3: Monitoring post-fall stillness (3.0s)...");
            }
            break;

        case FALL_WAITING_STILL: {
            float diffFrom1G = fabsf(a_total - 1.0f);

            // Nhạy hơn: Bất động khi |SV - 1.0G| <= 0.35G (0.65G đến 1.35G)
            // Nếu có cử động hoặc đứng dậy (diffFrom1G > 0.35G trong 4 mẫu = 100ms) -> Hủy ngay báo ngã!
            if (diffFrom1G > 0.35f) {
                movingSamples++;
                if (movingSamples >= 4) {
                    Serial.printf("[FALL] Active movement detected (SV=%.2fG) -> Fall Cancelled!\n", a_total);
                    fallPhase = FALL_IDLE;
                    movingSamples = 0;
                }
            } else {
                if (movingSamples > 0) movingSamples--;

                if (now - lastStillPrintMs >= 350) {
                    lastStillPrintMs = now;
                    Serial.printf("[FALL] Stillness: %.1fs / 3.0s (SV=%.2fG)\n", (float)(now - phaseTimer) / 1000.0f, a_total);
                }

                // Nằm bất động đủ 3.0 giây sau cú rơi & va chạm -> XÁC NHẬN TÉ NGÃ VÀ HÚ CÒI SOS!
                if (now - phaseTimer >= 3000) {
                    Serial.println("\n[FALL] ========================================");
                    Serial.println("[FALL] >>> XAC NHAN NGA! KICH HOAT COI SOS <<<");
                    Serial.println("[FALL] ========================================\n");
                    alarmActive = true;
                    lastSosAlarmBeepMs = 0;
                    fallPhase = FALL_IDLE;
                    movingSamples = 0;
                }
            }
            break;
        }
    }
}

void fall_detection_process_sample(float ax, float ay, float az, float svG) {
    evaluate_fall_state(ax, ay, az, svG);
}

void fall_detection_alarm_tick(void) {
    if (alarmActive) {
        uint32_t now = millis();
        static bool sirenHigh = false;
        if (now - lastSosAlarmBeepMs >= 300) {
            lastSosAlarmBeepMs = now;
            sirenHigh = !sirenHigh;
            uint16_t sirenFreq = sirenHigh ? 1320 : 880;
            tone_driver_play(sirenFreq, 280, 21); // Max volume 21
        }
    }
}

void fall_detection_task_start(void) {
    // Được tích hợp trực tiếp vào chu kỳ lấy mẫu tuần tự của Sensor Task để triệt tiêu 100% xung đột I2C / Siêu âm
}

bool fall_alarm_busy(void) { return alarmActive; }
bool fall_alarm_was_cancelled_recently(void) { return (millis() - cancelCooldown < 2000); }

void fall_alarm_dismiss(void) {
    alarmActive = false;
    cancelCooldown = millis();
    tone_driver_stop();
    Serial.println("[FALL] Fall alarm dismissed by user");
}

#endif