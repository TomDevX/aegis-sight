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
    if (windowCount < 10) {
        return false;
    }

    float sum = 0.0f;
    float maxSv = 0.0f;
    float minSv = 99.0f;
    uint32_t validCount = 0;

    for (uint32_t i = 0; i < windowCount; i++) {
        if (window[i] > 0.20f && window[i] < 5.0f) {
            sum += window[i];
            if (window[i] > maxSv) maxSv = window[i];
            if (window[i] < minSv) minSv = window[i];
            validCount++;
        }
    }
    if (validCount < 8) return false;

    float mean = sum / (float)validCount;

    float sqDiff = 0.0f;
    for (uint32_t i = 0; i < windowCount; i++) {
        if (window[i] > 0.20f && window[i] < 5.0f) {
            float d = window[i] - mean;
            sqDiff += d * d;
        }
    }
    float stddev = sqrtf(sqDiff / (float)validCount);
    float p2p = maxSv - minSv;

    // PHÂN BIỆT BƯỚC ĐI (LOCOMOTION) VS XOAY ĐẦU TẠI CHỖ:
    // - Xoay đầu tại chỗ: chuyển động mượt, stddev <= 0.06G, p2p <= 0.15G -> GATE OFF (Im lặng 100%)
    // - Bước đi: gót chân tiếp đất tạo xung lực nhấp nhô, stddev >= 0.12G VÀ p2p >= 0.25G -> GATE ON
    return (stddev >= 0.12f && p2p >= 0.25f);
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
