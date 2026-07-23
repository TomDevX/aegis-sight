#include <Arduino.h>
#include "config.h"
#include "driver/i2s.h"
#include "tone_driver.h"
#include "ai_pipeline.h"
#include <math.h>

#define AV_SAMPLE_RATE    16000
#define AV_READ_SAMPLES   1024
#define AV_READ_MS        500
#define AV_TASK_STACK     3072
#define AV_TASK_PRIO      1

static int16_t *micBuf = NULL;

static bool init_mic_i2s(void) {
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AV_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 512,
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
    if (!micBuf) {
        Serial.println("[AUTO_VOL] ps_malloc failed, task halting");
        vTaskDelete(NULL);
        return;
    }

    bool micOwned = false;

    while (true) {
        if (ai_pipeline_is_busy()) {
            if (micOwned) {
                deinit_mic_i2s();
                micOwned = false;
                Serial.println("[AUTO_VOL] Released mic to AI pipeline");
            }
            vTaskDelay(pdMS_TO_TICKS(AV_READ_MS));
            continue;
        }

        if (!micOwned) {
            if (!init_mic_i2s()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            micOwned = true;
            Serial.println("[AUTO_VOL] Mic I2S re-acquired");
        }

        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_MIC_PORT, micBuf,
                                 AV_READ_SAMPLES * sizeof(int16_t),
                                 &bytesRead, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytesRead == 0) {
            vTaskDelay(pdMS_TO_TICKS(AV_READ_MS));
            continue;
        }

        int numSamples = bytesRead / sizeof(int16_t);
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
