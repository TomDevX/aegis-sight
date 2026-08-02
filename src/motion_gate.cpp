#include "motion_gate.h"

// ============================================================
// Motion Gate - MPU6050 accelerometer movement detection
// Computes stddev of SV over a sliding window.
//   still  -> SV ~= 1g constant  -> tiny stddev
//   moving -> SV oscillates      -> large stddev
// Hysteresis timers prevent flicker at the threshold.
// ============================================================

static float    window[MOTION_WINDOW_SAMPLES] = {0};
static uint32_t windowCount = 0;
static uint32_t windowIdx   = 0;

static bool          gateEnabled   = false;
static unsigned long gateSinceMs   = 0;
static bool          motionRunning = false;
static unsigned long motionSinceMs = 0;

static bool detect_motion(void) {
    if (windowCount < 2) {
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
        if (!motionRunning) {
            motionRunning = true;
            motionSinceMs = now;
        }
    } else {
        motionRunning = false;
    }

    // Enable after sustained movement, disable after sustained stillness
    if (motionRunning) {
        if (!gateEnabled && (now - motionSinceMs >= MOTION_ON_MS)) {
            gateEnabled = true;
            gateSinceMs = now;
            Serial.println("[MOTION] GATE ON - user is moving");
        }
    } else if (gateEnabled) {
        if (now - gateSinceMs >= MOTION_OFF_MS) {
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
    motionRunning = false;
}
