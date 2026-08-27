#include <Arduino.h>
#include "config.h"
#include "driver/i2s.h"
#include "tone_driver.h"
#include "ai_pipeline.h"
#include <math.h>

#define AV_SAMPLE_RATE    16000
#define AV_READ_SAMPLES   1024
#define AV_READ_MS        150    // chu kỳ ngắn để nhả mic nhanh khi AI cần
#define AV_TASK_STACK     3072
#define AV_TASK_PRIO      1

static int16_t *micBuf = NULL;

static bool init_mic_i2s(void) {
    // INMP441 xuất 24-bit trong slot 32-bit -> PHẢI đọc 32-bit
    // (giống initMicI2S trong mic_ai_cam_test)
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AV_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
    };

    i2s_pin_config_t pin_cfg = {
        .bck_io_num = MIC_BCLK,
        .ws_io_num = MIC_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_DATA_IN,
    };

    esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &i2s_cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[AUTO_VOL] i2s_driver_install failed: 0x%x\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_MIC_PORT, &pin_cfg);
    if (err != ESP_OK) {
        Serial.printf("[AUTO_VOL] i2s_set_pin failed: 0x%x\n", err);
        i2s_driver_uninstall(I2S_MIC_PORT);
        return false;
    }

    return true;
}

static void deinit_mic_i2s(void) {
    i2s_driver_uninstall(I2S_MIC_PORT);
}

static uint8_t rms_to_volume(float rms) {
    if (rms < AV_RMS_QUIET) return 5;
    if (rms < AV_RMS_MODERATE) return 10;
    if (rms < AV_RMS_LOUD) return 16;
    return 21;
}

static void auto_volume_task(void *pvParameters) {
    micBuf = (int16_t *)ps_malloc(AV_READ_SAMPLES * sizeof(int16_t));
    int32_t *rawBuf = (int32_t *)ps_malloc(AV_READ_SAMPLES * sizeof(int32_t));
    if (!micBuf || !rawBuf) {
        Serial.println("[AUTO_VOL] ps_malloc failed, task halting");
        vTaskDelete(NULL);
        return;
    }

    if (!init_mic_i2s()) {
        Serial.println("[AUTO_VOL] init_mic_i2s failed");
    } else {
        Serial.println("[AUTO_VOL] Permanent Mic I2S initialized");
    }

    while (true) {
        if (ai_pipeline_is_busy()) {
            // Khi AI đang thu âm hoặc xử lý -> nhả mic tức thì
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_MIC_PORT, rawBuf,
                                 AV_READ_SAMPLES * sizeof(int32_t),
                                 &bytesRead, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytesRead == 0) {
            vTaskDelay(pdMS_TO_TICKS(AV_READ_MS));
            continue;
        }

        // INMP441: 24-bit data nằm trong slot 32-bit -> shift >>8 (giống test)
        int numSamples = bytesRead / sizeof(int32_t);
        for (int i = 0; i < numSamples; i++) {
            micBuf[i] = (int16_t)(rawBuf[i] >> 8);
        }

        int64_t sumSq = 0;
        for (int i = 0; i < numSamples; i++) {
            sumSq += (int64_t)micBuf[i] * micBuf[i];
        }
        float rms = sqrtf((float)sumSq / numSamples);

        uint8_t vol = rms_to_volume(rms);
        tone_driver_set_volume(vol);

        vTaskDelay(pdMS_TO_TICKS(AV_READ_MS));
    }
}

void auto_volume_task_start(void) {
    xTaskCreatePinnedToCore(auto_volume_task, "auto_vol", AV_TASK_STACK,
                            NULL, AV_TASK_PRIO, NULL, 1);
}
