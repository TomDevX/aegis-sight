#include "motion_gate.h"

// ============================================================
// Motion Gate - MPU6050 accelerometer movement detection
// Computes stddev of SV over a sliding window.
//   still  -> SV ~= 1g constant  -> tiny stddev (< 0.04g)
//   moving -> SV oscillates      -> large stddev (> 0.10g)
// Hysteresis timers prevent flicker at the threshold.
// ============================================================

static float    window[MOTION_WINDOW_SAMPLES] = {0};
static uint32_t windowCount = 0;
static uint32_t windowIdx   = 0;

static bool          gateEnabled   = false;
static unsigned long movingSinceMs = 0;
static unsigned long stillSinceMs  = 0;
static float         lastGyroDegS  = 0.0f;

static bool detect_motion(void) {
    if (windowCount < 8) {
        return false;
    }

    float sum = 0.0f;
    uint32_t validCount = 0;
    for (uint32_t i = 0; i < windowCount; i++) {
        if (window[i] > 0.20f && window[i] < 5.0f) {
            sum += window[i];
            validCount++;
        }
    }
    if (validCount < 6) return false;

    float mean = sum / (float)validCount;

    float sqDiff = 0.0f;
    for (uint32_t i = 0; i < windowCount; i++) {
        if (window[i] > 0.20f && window[i] < 5.0f) {
            float d = window[i] - mean;
            sqDiff += d * d;
        }
    }
    float stddev = sqrtf(sqDiff / (float)validCount);

    // CHỈ TÍNH BƯỚC ĐI (LOCOMOTION):
    // - Khi đứng yên (kể cả quay đầu / nhìn quanh): stddev chỉ ~0.02G -> KHÔNG tính là di chuyển
    // - Khi bước đi: xung lực tiếp đất của bước chân làm dao động stddev >= 0.09G -> KÍCH HOẠT CẢNH BÁO
    return (stddev >= 0.09f);
}

void motion_gate_update(float svG, float gyroDegS) {
    lastGyroDegS = gyroDegS;

    // Bỏ qua giá trị rác nếu có
    if (svG < 0.20f || svG > 8.0f) return;

    window[windowIdx] = svG;
    windowIdx = (windowIdx + 1) % MOTION_WINDOW_SAMPLES;
    if (windowCount < MOTION_WINDOW_SAMPLES) {
        windowCount++;
    }

    bool moving = detect_motion();
    unsigned long now = millis();

    if (moving) {
        stillSinceMs = 0; // Reset stillness timer
        if (movingSinceMs == 0) {
            movingSinceMs = now;
        }
        // Enable after sustained movement
        if (!gateEnabled && (now - movingSinceMs >= MOTION_ON_MS)) {
            gateEnabled = true;
            Serial.println("[MOTION] GATE ON - user is moving");
        }
    } else {
        movingSinceMs = 0; // Reset motion timer
        if (stillSinceMs == 0) {
            stillSinceMs = now;
        }
        // Disable only after sustained stillness
        if (gateEnabled && (now - stillSinceMs >= MOTION_OFF_MS)) {
            gateEnabled = false;
            Serial.println("[MOTION] GATE OFF - user is still");
        }
    }
}

bool motion_gate_enabled(void) {
    return gateEnabled;
}

void motion_gate_reset(void) {
    windowCount = 0;
    windowIdx   = 0;
    gateEnabled = false;
    movingSinceMs = 0;
    stillSinceMs  = 0;
}
