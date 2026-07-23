#include <Arduino.h>
#include "config.h"

#ifdef ENABLE_CAMERA_OV2640
#include "esp_camera.h"
#endif

#ifdef ENABLE_MPU6050_FALL_DETECTION
#include <Wire.h>
#endif

#include "tone_driver.h"
#include "ai_pipeline.h"
#include "secrets.h"
#include "config_portal.h"

// ============================================================
// GLOBAL STATE
// ============================================================
static bool psramAvailable = false;
static bool cameraInitialized = false;

// ============================================================
// FUNCTION DECLARATIONS
// ============================================================
static void initSerial(void);
static void checkPSRAM(void);

#ifdef ENABLE_CAMERA_OV2640
static bool initCamera(void);
#endif

void ultrasonic_task_start(void);
void fall_detection_task_start(void);
void auto_volume_task_start(void);

// ============================================================
// FACTORY RESET: hold button 5s on boot
// ============================================================
static void check_factory_reset(void) {
    pinMode(BTN_TRIGGER, INPUT_PULLUP);
    uint32_t t = millis();
    bool held = true;
    while (millis() - t < 5000) {
        if (digitalRead(BTN_TRIGGER) != LOW) { held = false; break; }
        delay(10);
    }
    if (held) {
        Serial.println("[INIT] Factory reset triggered (button held 5s)");
        secrets_clear();
        Serial.println("[INIT] Secrets cleared. Starting config portal...");
        config_portal_start();
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    initSerial();
    checkPSRAM();

    // --- Secrets + config portal ---
    secrets_begin();
    check_factory_reset();
    if (!secrets_has_all()) {
        Serial.println("[INIT] No saved config — starting setup portal");
        config_portal_start();
    }

    // --- I2C bus for MPU6050 ---
    #ifdef ENABLE_MPU6050_FALL_DETECTION
    Wire.begin(MPU_SDA, MPU_SCL);
    Serial.println("[INIT] I2C bus started for MPU6050");
    #endif

    // --- Camera OV2640 ---
    #ifdef ENABLE_CAMERA_OV2640
    cameraInitialized = initCamera();
    if (cameraInitialized) {
        Serial.println("[INIT] Camera OV2640 ready");
    } else {
        Serial.println("[ERROR] Camera init failed!");
    }
    #endif

    // --- Trigger button (AI / Hủy SOS - Active LOW, Internal Pull-Up) ---
    #ifdef ENABLE_CAMERA_OV2640
    pinMode(BTN_TRIGGER, INPUT_PULLUP);
    Serial.println("[INIT] Trigger button ready on GPIO " + String(BTN_TRIGGER));
    #endif

    // --- I2S Speaker (Tone Driver) - shared by all audio features ---
    if (tone_driver_init()) {
        tone_driver_start_task();
        Serial.println("[INIT] I2S Speaker tone driver ready");
    } else {
        Serial.println("[ERROR] Tone driver init failed!");
    }

    // --- Ultrasonic Proximity Beep (Chặng 2) ---
    #ifdef ENABLE_ULTRASONIC_HC_SR04
    ultrasonic_task_start();
    Serial.println("[INIT] Ultrasonic proximity task spawned");
    #endif

    // --- MPU6050 Fall Detection (Chặng 2) ---
    #ifdef ENABLE_MPU6050_FALL_DETECTION
    fall_detection_task_start();
    Serial.println("[INIT] Fall detection task spawned");
    #endif

    // --- Auto Volume Adjust (Chặng 2) ---
    #ifdef ENABLE_AUTO_VOLUME
    auto_volume_task_start();
    Serial.println("[INIT] Auto volume task spawned");
    #endif

    // --- AI Pipeline (Chặng 3) ---
    #ifdef ENABLE_AI_PIPELINE
    ai_pipeline_net_task_start();
    ai_pipeline_audio_task_start();
    Serial.println("[INIT] AI pipeline tasks spawned (Core 0 HTTP + Core 1 Capture)");
    #endif

    Serial.println("\n========================================");
    Serial.println("  AEGIS SIGHT v2.0 - Chặng 3 Ready");
    Serial.println("  Core 0: Wi-Fi + HTTP REST + Gemini");
    Serial.println("  Core 1: US + Fall + AutoVol + AI Rec + Speaker");
    Serial.println("========================================");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ============================================================
// PSRAM CHECK
// ============================================================
static void checkPSRAM(void) {
    Serial.println("[INIT] Checking PSRAM...");

    if (psramFound()) {
        size_t psramSize = ESP.getPsramSize();
        Serial.println("[OK] PSRAM detected: " + String(psramSize / (1024 * 1024)) + " MB (" + String(psramSize) + " bytes)");

        if (psramSize >= PSRAM_EXPECTED_SIZE) {
            Serial.println("[OK] Capacity matches expected 8MB OPI PSRAM");
        } else {
            Serial.println("[WARN] Smaller than expected 8MB - some features may be limited");
        }
        psramAvailable = true;

        void* testBuf = ps_malloc(4096);
        if (testBuf != NULL) {
            Serial.println("[OK] ps_malloc(4096) test passed");
            free(testBuf);
        } else {
            Serial.println("[ERROR] ps_malloc test failed!");
            psramAvailable = false;
        }
    } else {
        Serial.println("[ERROR] No PSRAM detected!");
        Serial.println("[ERROR] AI features require 8MB OPI PSRAM. Check hardware!");
        psramAvailable = false;
    }

    Serial.println("[INFO] Heap free: " + String(ESP.getFreeHeap() / 1024) + " KB");
    Serial.println("[INFO] PSRAM free: " + String(ESP.getFreePsram() / 1024) + " KB");
}

// ============================================================
// SERIAL INIT
// ============================================================
static void initSerial(void) {
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println("\n\n========================================");
    Serial.println("  AEGIS SIGHT v2.0 - Chặng 3");
    Serial.println("  ESP32-S3-N16R8-CAM | 8MB OPI PSRAM");
    Serial.println("  Gemini Live Stream Pipeline");
    Serial.println("========================================\n");
}

// ============================================================
// CAMERA OV2640 INIT
// ============================================================
#ifdef ENABLE_CAMERA_OV2640
static bool initCamera(void) {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_Y2;
    config.pin_d1       = CAM_Y3;
    config.pin_d2       = CAM_Y4;
    config.pin_d3       = CAM_Y5;
    config.pin_d4       = CAM_Y6;
    config.pin_d5       = CAM_Y7;
    config.pin_d6       = CAM_Y8;
    config.pin_d7       = CAM_Y9;
    config.pin_xclk     = CAM_XCLK;
    config.pin_pclk     = CAM_PCLK;
    config.pin_vsync    = CAM_VSYNC;
    config.pin_href     = CAM_HREF;
    config.pin_sccb_sda = CAM_SIOD;
    config.pin_sccb_scl = CAM_SIOC;
    config.pin_pwdn     = CAM_PWDN;
    config.pin_reset    = CAM_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramAvailable) {
        config.frame_size   = FRAMESIZE_SVGA;
        config.jpeg_quality = 10;
        config.fb_count     = 2;
        config.fb_location  = CAMERA_FB_IN_PSRAM;
        config.grab_mode    = CAMERA_GRAB_LATEST;
    } else {
        config.frame_size   = FRAMESIZE_QVGA;
        config.jpeg_quality = 15;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.println("[ERROR] Camera init failed: 0x" + String(err, HEX));
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 1);
        s->set_saturation(s, -1);
    }
    return true;
}
#endif
