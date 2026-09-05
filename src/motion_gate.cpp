#include "motion_gate.h"

// ============================================================
// Motion Gate - MPU6050 accelerometer movement detection
// Computes stddev of SV over a sliding window.
//   still  -> SV ~= 1g constant  -> tiny stddev (< 0.03g)
//   moving -> SV oscillates      -> stddev >= MOTION_STDDEV_G
// Hysteresis timers prevent flicker at the threshold.
// ============================================================

#ifndef MOTION_WINDOW_SAMPLES
#define MOTION_WINDOW_SAMPLES  20
#endif
#ifndef MOTION_STDDEV_G
#define MOTION_STDDEV_G        0.055f
#endif
#ifndef MOTION_P2P_G
#define MOTION_P2P_G           0.12f
#endif
#ifndef MOTION_ON_MS
#define MOTION_ON_MS           180
#endif
#ifndef MOTION_OFF_MS
#define MOTION_OFF_MS          1200
#endif

static float    window[MOTION_WINDOW_SAMPLES] = {0};
static uint32_t windowCount = 0;
static uint32_t windowIdx   = 0;

static bool          gateEnabled   = false;
static unsigned long movingSinceMs = 0;
static unsigned long stillSinceMs  = 0;
static float         lastGyroDegS  = 0.0f;
static float         lastStdDev    = 0.0f;
static float         lastP2P       = 0.0f;

static bool detect_motion(void) {
    if (windowCount < 6) {
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
    if (validCount < 5) return false;

    float mean = sum / (float)validCount;

    float sqDiff = 0.0f;
    for (uint32_t i = 0; i < windowCount; i++) {
        if (window[i] > 0.20f && window[i] < 5.0f) {
            float d = window[i] - mean;
            sqDiff += d * d;
        }
    }
    lastStdDev = sqrtf(sqDiff / (float)validCount);
    lastP2P = maxSv - minSv;

    // PHÂN BIỆT BƯỚC ĐI (LOCOMOTION) VS XOAY ĐẦU TẠI CHỖ:
    // - Xoay đầu tại chỗ / Đứng yên: chuyển động mượt, stddev <= 0.04G, p2p <= 0.10G -> GATE OFF (Im lặng 100%)
    // - Bước đi: gót chân tiếp đất tạo xung lực nhấp nhô, stddev >= MOTION_STDDEV_G VÀ p2p >= MOTION_P2P_G -> GATE ON
    return (lastStdDev >= MOTION_STDDEV_G && lastP2P >= MOTION_P2P_G);
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
        // Enable sau khi phát hiện chuyển động liên tục đạt ngưỡng thời gian MOTION_ON_MS
        if (!gateEnabled && (now - movingSinceMs >= MOTION_ON_MS)) {
            gateEnabled = true;
            Serial.printf("[MOTION] GATE ON - user is moving (stddev=%.3f, p2p=%.3f)\n", lastStdDev, lastP2P);
        }
    } else {
        if (stillSinceMs == 0) {
            stillSinceMs = now;
        }
        // Tránh reset timer kích hoạt nếu chỉ bị hụt 1-2 mẫu tức thời (< 100ms) giữa các nhịp chân
        if (!gateEnabled && (now - stillSinceMs >= 100)) {
            movingSinceMs = 0;
        }
        // Disable sau khi duy trì đứng yên đủ lâu (MOTION_OFF_MS)
        if (gateEnabled && (now - stillSinceMs >= MOTION_OFF_MS)) {
            gateEnabled = false;
            movingSinceMs = 0;
            Serial.printf("[MOTION] GATE OFF - user is still (stddev=%.3f, p2p=%.3f)\n", lastStdDev, lastP2P);
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
    lastStdDev    = 0.0f;
    lastP2P       = 0.0f;
}

void motion_gate_get_metrics(float *out_stddev, float *out_p2p) {
    if (out_stddev) *out_stddev = lastStdDev;
    if (out_p2p)    *out_p2p    = lastP2P;
}
