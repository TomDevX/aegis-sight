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
            // Rơi tự do: Cần 3 mẫu liên tiếp < 0.45G (60ms rơi tự do liên tục)
            // Tuyệt đối không bị kích hoạt ảo khi để yên trên bàn hoặc nhấc tay nhẹ
            if (a_total < 0.45f) {
                freeFallSamples++;
                if (freeFallSamples >= 3) {
                    fallPhase = FALL_FREE_FALL;
                    phaseTimer = now;
                    freeFallSamples = 0;
                    movingSamples = 0;
                    Serial.printf("\n[FALL] >>> Phase 1: Free Fall CONFIRMED (SV=%.2fG) <<<\n", a_total);
                }
            } else {
                freeFallSamples = 0;
            }
            break;

        case FALL_FREE_FALL:
            // Pha 2: Chờ va chạm mạnh (> 2.20G) trong cửa sổ 600ms sau cú rơi tự do
            if (a_total > 2.20f) {
                fallPhase = FALL_IMPACT;
                phaseTimer = now;
                freeFallSamples = 0;
                movingSamples = 0;
                Serial.printf("[FALL] >>> Phase 2: Impact CONFIRMED (SV=%.2fG) <<<\n", a_total);
            } else if (now - phaseTimer > 600) { 
                fallPhase = FALL_IDLE;
                freeFallSamples = 0;
            }
            break;

        case FALL_IMPACT:
            // Đợi 400ms để hết rung lắc nảy ban đầu
            if (now - phaseTimer > 400) {
                fallPhase = FALL_WAITING_STILL;
                phaseTimer = now;
                lastStillPrintMs = now;
                movingSamples = 0;
                Serial.println("[FALL] >>> Phase 3: Post-impact stillness check (2.0s)...");
            }
            break;

        case FALL_WAITING_STILL: {
            float diffFrom1G = fabsf(a_total - 1.0f);

            // Bất động: a_total nằm quanh 1.0G (0.65G đến 1.35G)
            if (diffFrom1G > 0.45f) {
                movingSamples++;
                // Cử động liên tục > 6 mẫu (~120ms) -> Hủy vì người đó vẫn đang cử động bình thường
                if (movingSamples > 6) {
                    Serial.printf("[FALL] Motion detected after impact (SV=%.2fG) -> Cancelled\n", a_total);
                    fallPhase = FALL_IDLE;
                    movingSamples = 0;
                }
            } else {
                if (movingSamples > 0) movingSamples--;

                if (now - lastStillPrintMs >= 500) {
                    lastStillPrintMs = now;
                    Serial.printf("[FALL] Stillness: %.1fs / 2.0s (SV=%.2fG)\n", (float)(now - phaseTimer) / 1000.0f, a_total);
                }

                if (now - phaseTimer >= FALL_INACTIVITY_MS) {
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

static void fall_detection_task(void *pv) {
    if (!mpu_manager_init()) {
        Serial.println("[FALL] Init MPU6050 failed!");
        vTaskDelete(NULL);
        return;
    }

    Serial.println("[FALL] Fall detection task started (Core 1)");

    while (true) {
        float ax = 0, ay = 0, az = 0, svG = 1.0f;
        if (mpu_manager_read_accel_g(&ax, &ay, &az, &svG)) {
            evaluate_fall_state(ax, ay, az, svG);
        }

        // Khi báo động SOS đang kích hoạt: Hú còi liên tục tuần hoàn mỗi 800ms cho đến khi bấm nút tắt
        if (alarmActive) {
            uint32_t now = millis();
            if (now - lastSosAlarmBeepMs >= 800) {
                lastSosAlarmBeepMs = now;
                tone_driver_play(TONE_ALARM, 500, 21);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void fall_detection_task_start(void) {
    xTaskCreatePinnedToCore(fall_detection_task, "fall_task", 4096, NULL, 3, NULL, 1);
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