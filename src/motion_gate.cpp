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

static bool detect_motion(void) {
    if (windowCount < 5) {
        return false;
    }

    float sum = 0.0f;
    for (uint32_t i = 0; i < windowCount; i++) {
        sum += window[i];
    }
    float mean = sum / (float)windowCount;

    float sqDiff = 0.0f;
    for (uint32_t i = 0; i < windowCount; i++) {
        float d = window[i] - mean;
        sqDiff += d * d;
    }
    float stddev = sqrtf(sqDiff / (float)windowCount);

    return (stddev > MOTION_STDDEV_G);
}

void motion_gate_update(float svG) {
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
