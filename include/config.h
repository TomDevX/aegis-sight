#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// FEATURE FLAGS
// Comment out to disable features at compile time.
// Disabled code is completely excluded from build.
// ============================================================

// Core AI Pipeline (Button -> Camera -> Cloud AI -> Speaker)
// #define ENABLE_CAMERA_OV2640
// #define ENABLE_SPEAKER_I2S

// AI Pipeline (Chặng 3 - HTTP REST + Gemini → text → Google TTS)
// #define ENABLE_AI_PIPELINE

// Cloud Text-to-Speech (Google Translate TTS) — HTTP MP3 → decode → play
// #define ENABLE_TTS_CLOUD

// Standalone HW Test Modules (Chặng 1)
// Uncomment 1 module at a time, comment ALL main pipeline flags to avoid conflict
// #define ENABLE_MIC_TEST
// #define ENABLE_SPEAKER_TEST
// #define ENABLE_ULTRASONIC_TEST
// #define ENABLE_MPU6050_TEST
#define ENABLE_MOTION_ULTRA_TEST

// Cloud API Test (Gemini + Google TTS) — standalone, no extra HW needed
// Comment ALL pipeline flags, uncomment this 1 flag
// #define ENABLE_API_TEST

// ============================================================
// MAIN PIPELINE (Chặng 2) - Real-time tasks on Core 1
// Enable these together for the full main build
// ============================================================
// #define ENABLE_ULTRASONIC_HC_SR04    // Obstacle proximity beep (motion-gated)
// #define ENABLE_MPU6050_FALL_DETECTION // MPU6050: 3-phase fall detection + SOS
// #define ENABLE_MOTION_GATE            // Motion gate: US beep only while moving

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
// GPIO48 is the onboard LED — moved SCL to a free pin.
// ============================================================
#define MPU_SDA           47
#define MPU_SCL           39

// ============================================================
// SYSTEM CONSTANTS
// ============================================================
#define SERIAL_BAUD     2000000
#define PSRAM_EXPECTED_SIZE (8 * 1024 * 1024)  // 8MB

// ============================================================
// I2S PORT ASSIGNMENT
// ============================================================
#define I2S_MIC_PORT    I2S_NUM_0
#define I2S_SPK_PORT    I2S_NUM_1

// ============================================================
// ULTRASONIC HC-SR04 - Beep Thresholds (cm)
// 3 zones: DANGER | WARNING | SAFE (silent)
// ============================================================
#define DISTANCE_DANGER    35  // D <= 35cm: beep dồn dập
#define DISTANCE_WARNING   45  // 35cm < D <= 45cm: beep vừa
// D > 45cm: im lặng

// ============================================================
// MOTION GATE - MPU6050 accelerometer movement detection
// Gates ultrasonic beeps: beep ONLY while the user is moving.
// Detect by stddev of SV over a sliding window (500ms @ 20ms).
// Hysteresis timers prevent gate flicker.
// ============================================================
#define MOTION_WINDOW_SAMPLES  25   // 25 samples x 20ms = 500ms window
#define MOTION_STDDEV_G       0.08  // stddev(SV) above this = moving
#define MOTION_ON_MS          400   // sustained movement to enable gate
#define MOTION_OFF_MS         1200  // sustained stillness to disable gate

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

// ============================================================
// AI PIPELINE (Chặng 3) - HTTP REST + Gemini
// ============================================================
// Multi-Wi-Fi - tự động kết nối vào mạng mạnh nhất trong danh sách
#define WIFI_SSID               ""
#define WIFI_PASS               ""
#define WIFI_SSID2              ""
#define WIFI_PASS2              ""
#define WIFI_SSID3              ""
#define WIFI_PASS3              ""
#define GEMINI_API_KEY          ""
#define GEMINI_MODEL            "models/gemini-3.5-flash-lite"
#define GEMINI_MODEL_SHORT      "gemini-3.5-flash-lite"
#define GEMINI_API_HOST         "generativelanguage.googleapis.com"
#define GEMINI_API_PORT         443
#define GEMINI_UPLOAD_HOST      "storage.googleapis.com"

// Hold-to-Talk: bấm giữ để ghi âm, thả để gửi. Tối đa 8 giây.
#define AI_AUDIO_SAMPLE_RATE    16000
#define AI_AUDIO_MAX_RECORD_MS  8000
#define AI_AUDIO_MAX_SAMPLES    (AI_AUDIO_MAX_RECORD_MS * AI_AUDIO_SAMPLE_RATE / 1000)  // 128000

// Ring buffer for TTS/response audio playback (PSRAM)
// Larger buffer (256KB) to accommodate local TTS PCM generation
#define AI_PCM_RINGBUF_SIZE     (256 * 1024)

// Task config
#define AI_NET_TASK_STACK       12288
#define AI_AUDIO_TASK_STACK     8192
#define AI_NET_TASK_PRIO        2
#define AI_AUDIO_TASK_PRIO      5

// ============================================================
// TEXT-TO-SPEECH — text limits & cloud TTS config
// ============================================================
#define TTS_MAX_TEXT_LEN        8192       // Max accumulated text from SSE response
#define TTS_CLOUD_MAX_CHARS     200        // Google TTS limit (~200 chars per request)
#define TTS_MP3_BUF_SIZE        (64 * 1024) // PSRAM buffer for downloaded MP3

#endif // CONFIG_H
