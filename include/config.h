#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// FEATURE FLAGS
// Comment out to disable features at compile time.
// Disabled code is completely excluded from build.
// ============================================================

// Core AI Pipeline (Button -> Camera + Mic -> Cloud AI -> Speaker)
#define ENABLE_CAMERA_OV2640
#define ENABLE_MICROPHONE_INMP441
#define ENABLE_SPEAKER_I2S

// Real-time Safety Features (Core 1 background tasks)
#define ENABLE_ULTRASONIC_HC_SR04
#define ENABLE_MPU6050_FALL_DETECTION

// Advanced Features
#define ENABLE_AUTO_VOLUME
// #define ENABLE_FACE_RECOGNITION

// Standalone HW Test Modules (Chặng 1)
// Uncomment 1 module at a time, comment ALL main.cpp flags to avoid conflict
// #define ENABLE_MIC_TEST
// #define ENABLE_SPEAKER_TEST
// #define ENABLE_ULTRASONIC_TEST
// #define ENABLE_MPU6050_TEST

// Camera Test Standalone (comment ENABLE_CAMERA_OV2640 để tránh conflict)
// #define ENABLE_CAMERA

// ============================================================
// CAMERA OV2640 - DVP Bus (Right Module - FPC DVP Bus)
// Dedicated DVP Pins on ESP32-S3 Cam board
// ============================================================
#define CAM_PWDN          -1
#define CAM_RESET         -1
#define CAM_XCLK          10
#define CAM_SIOD           4   // SDA
#define CAM_SIOC           5   // SCL
#define CAM_Y9            16
#define CAM_Y8            17
#define CAM_Y7            18
#define CAM_Y6            12
#define CAM_Y5            11
#define CAM_Y4             6
#define CAM_Y3             7
#define CAM_Y2            15
#define CAM_VSYNC         38
#define CAM_HREF          21
#define CAM_PCLK          13

// ============================================================
// MICROPHONE INMP441 - I2S RX (Left Module - Channel 0)
// L/R pin tied to GND = Left channel
// ============================================================
#define MIC_BCLK          41  // SCK
#define MIC_LRCK          42  // WS (LRCK)
#define MIC_DATA_IN        2  // SD

// ============================================================
// SPEAKER - Seeed Grove I2S / PCM5102A (Right Module - I2S TX Ch 1)
// ============================================================
#define SPK_BCLK           1  // BCLK
#define SPK_LRCK           3  // WS (LRCK)
#define SPK_DATA_OUT      40  // DIN

// ============================================================
// TRIGGER BUTTON (Hỏi AI / Hủy SOS) - Hộp Trái
// Internal Pull-Up (Active LOW)
// ============================================================
#define BTN_TRIGGER       14

// ============================================================
// ULTRASONIC HC-SR04 - Hộp Phải, hướng chính diện
// ============================================================
#define ULTRASONIC_TRIG    8
#define ULTRASONIC_ECHO    9

// ============================================================
// MPU6050 - I2C (Left Module - Wire through Headband)
// ============================================================
#define MPU_SDA           47
#define MPU_SCL           48

// ============================================================
// SYSTEM CONSTANTS
// ============================================================
#define SERIAL_BAUD     115200
#define PSRAM_EXPECTED_SIZE (8 * 1024 * 1024)  // 8MB

// ============================================================
// I2S PORT ASSIGNMENT
// ============================================================
#define I2S_MIC_PORT    I2S_NUM_0
#define I2S_SPK_PORT    I2S_NUM_1

// ============================================================
// ULTRASONIC HC-SR04 - Beep Thresholds (cm)
// Distance > SAFE: silence | WARNING: slow beep | DANGER: fast beep
// ============================================================
#define DISTANCE_DANGER    50  // D <= 50cm: beep dồn dập
#define DISTANCE_WARNING  100  // 50cm < D <= 100cm: beep vừa
#define DISTANCE_SLOW     150  // 100cm < D <= 150cm: beep chậm
// D > 150cm: im lặng

// ============================================================
// FALL DETECTION THRESHOLDS (3-Phase Algorithm)
// Phase 1: Free-fall  -> SV < 0.5g
// Phase 2: Impact     -> SV > 2.5g (within 300ms)
// Phase 3: Inactivity -> SV ≈ 1g for 2s
// ============================================================
#define FALL_FREEFALL_G      0.5   // m/s^2 threshold free-fall (< 0.5g)
#define FALL_IMPACT_G        2.5   // m/s^2 threshold impact (> 2.5g)
#define FALL_IMPACT_WINDOW   300   // ms window for impact after free-fall
#define FALL_INACTIVITY_MS   2000  // ms of stillness to confirm fall
#define FALL_CANCEL_WAIT_MS  10000 // ms to wait for user cancel before SOS
#define FALL_DEBOUNCE_MS     5000  // cooldown after fall alarm (ms)

// ============================================================
// AUTO-VOLUME RMS THRESHOLDS (16-bit signed PCM)
// Maps ambient noise RMS -> speaker volume 1-21
// ============================================================
#define AV_RMS_QUIET       200    // Quiet room -> volume 5
#define AV_RMS_MODERATE   2000    // Normal ambient -> volume 10
#define AV_RMS_LOUD       8000    // Street noise -> volume 16
#define AV_RMS_MAX       16000    // Very loud -> volume 21 (max)

#endif // CONFIG_H
