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

static void evaluate_fall_state(float ax, float ay, float az, float a_total, float gyroDegS) {
    // Không bao giờ chặn fall detection khi AI đang chạy — té ngã vẫn phải phát hiện được
    if (millis() - cancelCooldown < FALL_DEBOUNCE_MS) {
        fallPhase = FALL_IDLE;
        freeFallSamples = 0;
        movingSamples = 0;
        return;
    }

    uint32_t now = millis();

    switch (fallPhase) {
        case FALL_IDLE:
            // 1. Nhánh 1: Rơi tự do + Cơ thể xoay lộn (0.10G <= a_total < 0.60G VÀ Gyro >= 120 deg/s)
            // Lực mô-men quay góc bắt buộc phải có khi con người bị trượt chân / ngã nhào!
            if (a_total >= 0.10f && a_total < 0.60f && gyroDegS >= 120.0f) {
                freeFallSamples++;
                if (freeFallSamples >= 2) { // Yêu cầu ít nhất 2 mẫu liên tiếp (50ms) chống nhiễu đơn lẻ
                    fallPhase = FALL_FREE_FALL;
                    phaseTimer = now;
                    freeFallSamples = 0;
                    movingSamples = 0;
                    Serial.printf("\n[%6.2fs][FALL] >>> Phase 1: Rotational Free Fall DETECTED (SV=%.2fG, Gyro=%.0f dps) <<<\n", now / 1000.0f, a_total, gyroDegS);
                }
            } else if (a_total >= 0.10f && a_total < 0.60f) {
                // Rơi tự do nhưng không có quay góc (chỉ là rung lắc / giật tay thẳng) -> bỏ qua
                freeFallSamples = 0;
            }
            // 2. Nhánh 2: Quật ngã mạnh chấn động + xoay người dứt khoát (a_total > 2.60G VÀ Gyro >= 160 deg/s)
            else if (a_total > 2.60f && gyroDegS >= 160.0f) {
                fallPhase = FALL_IMPACT;
                phaseTimer = now;
                freeFallSamples = 0;
                movingSamples = 0;
                Serial.printf("\n[%6.2fs][FALL] >>> High-G Rotational Impact DETECTED (SV=%.2fG, Gyro=%.0f dps) <<<\n", now / 1000.0f, a_total, gyroDegS);
            } else {
                freeFallSamples = 0;
            }
            break;

        case FALL_FREE_FALL:
            // Pha 2: Chờ va chạm tiếp đất (> 1.40G) trong cửa sổ 1000ms sau cú rơi
            if (a_total > 1.40f) {
                fallPhase = FALL_IMPACT;
                phaseTimer = now;
                freeFallSamples = 0;
                movingSamples = 0;
                Serial.printf("[%6.2fs][FALL] >>> Phase 2: Landing Impact DETECTED (SV=%.2fG) <<<\n", now / 1000.0f, a_total);
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
                Serial.printf("[%6.2fs][FALL] >>> Phase 3: Monitoring post-fall stillness (3.0s)...\n", now / 1000.0f);
            }
            break;

        case FALL_WAITING_STILL: {
            float diffFrom1G = fabsf(a_total - 1.0f);

            // Bất động: Gia tốc nằm trong dải 0.65G..1.35G VÀ Vận tốc góc Gyro < 40 deg/s (không cử động)
            // Nếu có cử động hoặc đứng dậy (diffFrom1G > 0.35G hoặc Gyro > 45 deg/s) -> Hủy ngay báo ngã!
            if (diffFrom1G > 0.35f || gyroDegS > 45.0f) {
                movingSamples++;
                if (movingSamples >= 4) {
                    Serial.printf("[%6.2fs][FALL] Active movement detected (SV=%.2fG, Gyro=%.0f dps) -> Fall Cancelled!\n", now / 1000.0f, a_total, gyroDegS);
                    fallPhase = FALL_IDLE;
                    movingSamples = 0;
                }
            } else {
                if (movingSamples > 0) movingSamples--;

                if (now - lastStillPrintMs >= 350) {
                    lastStillPrintMs = now;
                    Serial.printf("[%6.2fs][FALL] Stillness: %.1fs / 3.0s (SV=%.2fG, Gyro=%.0f dps)\n", now / 1000.0f, (float)(now - phaseTimer) / 1000.0f, a_total, gyroDegS);
                }

                // Nằm bất động đủ 3.0 giây sau cú rơi & va chạm -> XÁC NHẬN TÉ NGÃ VÀ HÚ CÒI SOS!
                if (now - phaseTimer >= 3000) {
                    Serial.println("\n[FALL] ========================================");
                    Serial.printf("[%6.2fs][FALL] >>> XAC NHAN NGA! KICH HOAT COI SOS <<<\n", now / 1000.0f);
                    Serial.println("[FALL] ========================================\n");
                    // Dừng mọi luồng AI/TTS ngay lập tức
                    ai_pipeline_stop();
                    tone_driver_stream_set_active(false);
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

void fall_detection_process_sample(float ax, float ay, float az, float svG, float gyroDegS) {
    evaluate_fall_state(ax, ay, az, svG, gyroDegS);
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