#ifdef ENABLE_CAMERA

#include <Arduino.h>
#include "config.h"
#include "esp_camera.h"

// ============================================================
// Camera Test - Chụp JPEG 1600x1200, lưu PSRAM, in dung lượng
// Kích hoạt: bỏ comment #define ENABLE_CAMERA trong config.h
//             và comment #define ENABLE_CAMERA_OV2640 để tránh
//             conflict setup/loop với main.cpp
// ============================================================

static bool initCameraTest(void) {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;ngfnscgfngnfngfnngfnfd
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

    // 1600x1200 (UXGA) bắt buộc dùng PSRAM cho frame buffer
    config.frame_size   = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM_TEST] Init failed: 0x%x\n", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_saturation(s, 0);
    }
    return true;
}

static void captureAndReport(void) {
    Serial.println("[CAM_TEST] Capturing 1600x1200 JPEG...");

    unsigned long tStart = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    unsigned long tElapsed = millis() - tStart;

    if (!fb) {
        Serial.println("[CAM_TEST] Capture FAILED!");
        return;
    }

    Serial.printf("[CAM_TEST] Capture OK in %lu ms\n", tElapsed);
    Serial.printf("[CAM_TEST] Resolution : %dx%d\n", fb->width, fb->height);
    Serial.printf("[CAM_TEST] JPEG size  : %u bytes (%.2f KB)\n",
                  fb->len, fb->len / 1024.0f);
    Serial.printf("[CAM_TEST] Buffer addr: %p (PSRAM: %s)\n",
                  fb->buf,
                  psramFound() ? "yes" : "no");

    // Kiểm tra buffer có nằm trong vùng PSRAM không
    if (psramFound()) {
        uint32_t psramStart = 0x3C000000;
        uint32_t psramEnd   = psramStart + ESP.getPsramSize();
        uint32_t bufAddr    = (uint32_t)fb->buf;
        bool inPsram = (bufAddr >= psramStart && bufAddr < psramEnd);
        Serial.printf("[CAM_TEST] Buffer in PSRAM region: %s\n",
                      inPsram ? "YES" : "NO");
    }

    esp_camera_fb_return(fb);
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n========================================");
    Serial.println("  CAMERA TEST - OV2640 UXGA 1600x1200");
    Serial.println("========================================\n");

    // Kiểm tra PSRAM
    if (!psramFound()) {
        Serial.println("[CAM_TEST] FATAL: No PSRAM! UXGA needs PSRAM.");
        return;
    }
    Serial.printf("[CAM_TEST] PSRAM: %d KB free\n",
                  ESP.getFreePsram() / 1024);

    if (!initCameraTest()) {
        Serial.println("[CAM_TEST] Camera init failed, halting.");
        return;
    }
    Serial.println("[CAM_TEST] Camera OV2640 initialized OK");

    // Chụp ảnh
    captureAndReport();

    Serial.println("\n========================================");
    Serial.println("  TEST COMPLETE - Entering idle loop");
    Serial.println("========================================");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(5000));
}

#endif // ENABLE_CAMERA
