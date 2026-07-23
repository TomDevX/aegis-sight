#include <Arduino.h>
#include "config.h"
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "tone_driver.h"
#include "ai_pipeline.h"

// ============================================================
// Fall Detection - MPU6050 3-Phase Algorithm on Core 1
// Phase 1: Free-fall (SV < 0.5g)
// Phase 2: Impact (SV > 2.5g within 300ms after free-fall)
// Phase 3: Inactivity (SV ≈ 1g for 2s after impact)
// Action: Confirm fall -> SOS alarm after 10s cancel window
// ============================================================

#define G_TO_MS2          9.80665f
#define G_NEUTRAL         1.0f
#define MPU_READ_MS       20
#define FALL_TASK_STACK   4096
#define FALL_TASK_PRIO    4

// Fall state machine
typedef enum {
    FALL_IDLE,
    FALL_FREEFALL,
    FALL_IMPACT_WAIT,
    FALL_INACTIVITY_WAIT,
    FALL_CONFIRMED,
    FALL_SOS_COUNTDOWN
} fall_state_t;

static Adafruit_MPU6050 mpu;
static fall_state_t fallState = FALL_IDLE;
static unsigned long stateTimestamp = 0;
static unsigned long lastSosCycleMs = 0;

static bool init_mpu6050(void) {
    if (!mpu.begin()) {
        Serial.println("[FALL] MPU6050 not found!");
        return false;
    }
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
    mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("[FALL] MPU6050 initialized");
    return true;
}

static float read_sv(void) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float sv = sqrtf(a.acceleration.x * a.acceleration.x +
                     a.acceleration.y * a.acceleration.y +
                     a.acceleration.z * a.acceleration.z);
    return sv;
}

static void play_sos_alarm(void) {
    // SOS pattern: 3 short + 3 long + 3 short (International distress)
    uint16_t shortMs = 150;
    uint16_t longMs  = 400;
    uint16_t gapMs   = 100;

    for (int cycle = 0; cycle < 3; cycle++) {
        if (digitalRead(BTN_TRIGGER) == LOW) {
            Serial.println("[FALL] SOS cancelled by user!");
            fallState = FALL_IDLE;
            return;
        }

        // 3 short beeps
        for (int i = 0; i < 3; i++) {
            tone_driver_play(1000, shortMs, 21);
            vTaskDelay(pdMS_TO_TICKS(shortMs + gapMs));
        }
        vTaskDelay(pdMS_TO_TICKS(200));

        // 3 long beeps
        for (int i = 0; i < 3; i++) {
            tone_driver_play(1000, longMs, 21);
            vTaskDelay(pdMS_TO_TICKS(longMs + gapMs));
        }
        vTaskDelay(pdMS_TO_TICKS(200));

        // 3 short beeps
        for (int i = 0; i < 3; i++) {
            tone_driver_play(1000, shortMs, 21);
            vTaskDelay(pdMS_TO_TICKS(shortMs + gapMs));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void fall_task(void *pvParameters) {
    pinMode(BTN_TRIGGER, INPUT_PULLUP);

    if (!init_mpu6050()) {
        Serial.println("[FALL] Task halting - no MPU6050");
        vTaskDelete(NULL);
        return;
    }

    Serial.println("[FALL] 3-phase detection task started");

    while (true) {
        // User is pressing button (AI active) — reset fall detection
        if (ai_pipeline_is_busy()) {
            if (fallState != FALL_IDLE) {
                fallState = FALL_IDLE;
                Serial.println("[FALL] AI active — reset to IDLE");
            }
            vTaskDelay(pdMS_TO_TICKS(MPU_READ_MS));
            continue;
        }

        float sv = read_sv();
        float svG = sv / G_TO_MS2;
        unsigned long now = millis();

        switch (fallState) {
            case FALL_IDLE:
                if (svG < FALL_FREEFALL_G) {
                    fallState = FALL_FREEFALL;
                    stateTimestamp = now;
                    Serial.printf("[FALL] Phase 1: Free-fall detected (SV=%.2fg)\n", svG);
                }
                break;

            case FALL_FREEFALL:
                if (svG > FALL_IMPACT_G) {
                    fallState = FALL_IMPACT_WAIT;
                    stateTimestamp = now;
                    Serial.printf("[FALL] Phase 2: Impact detected (SV=%.2fg)\n", svG);
                } else if (now - stateTimestamp > FALL_IMPACT_WINDOW) {
                    fallState = FALL_IDLE;
                }
                break;

            case FALL_IMPACT_WAIT:
                if (svG > 0.9f && svG < 1.1f) {
                    fallState = FALL_INACTIVITY_WAIT;
                    stateTimestamp = now;
                    Serial.printf("[FALL] Phase 3: Inactivity monitoring (SV=%.2fg)\n", svG);
                } else if (svG > FALL_IMPACT_G) {
                    stateTimestamp = now;
                }
                break;

            case FALL_INACTIVITY_WAIT:
                if (svG > 0.9f && svG < 1.1f) {
                    if (now - stateTimestamp >= FALL_INACTIVITY_MS) {
                        fallState = FALL_CONFIRMED;
                        Serial.println("[FALL] >>> FALL CONFIRMED! Starting SOS countdown...");
                    }
                } else if (svG > FALL_IMPACT_G || svG < FALL_FREEFALL_G) {
                    fallState = FALL_IDLE;
                    Serial.println("[FALL] Inactivity interrupted, resetting");
                }
                break;

            case FALL_CONFIRMED:
                tone_driver_play(800, 200, 15);
                vTaskDelay(pdMS_TO_TICKS(300));
                tone_driver_play(800, 200, 15);
                vTaskDelay(pdMS_TO_TICKS(300));
                fallState = FALL_SOS_COUNTDOWN;
                stateTimestamp = now;
                Serial.printf("[FALL] Press button within %lu ms to cancel SOS\n",
                              FALL_CANCEL_WAIT_MS);
                break;

            case FALL_SOS_COUNTDOWN:
                if (digitalRead(BTN_TRIGGER) == LOW) {
                    Serial.println("[FALL] SOS CANCELLED by user");
                    fallState = FALL_IDLE;
                    break;
                }
                if (now - stateTimestamp >= FALL_CANCEL_WAIT_MS) {
                    Serial.println("[FALL] >>> ACTIVATING SOS ALARM!");
                    play_sos_alarm();
                    lastSosCycleMs = now;
                    fallState = FALL_IDLE;
                }
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(MPU_READ_MS));
    }
}

void fall_detection_task_start(void) {
    xTaskCreatePinnedToCore(fall_task, "fall_det", FALL_TASK_STACK,
                            NULL, FALL_TASK_PRIO, NULL, 1);
}
