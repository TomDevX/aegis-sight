#include <Arduino.h>
#include "config.h"
#include "driver/i2s.h"
#include "tone_driver.h"
#include <math.h>

// ============================================================
// Auto-Volume Adjust - Mic INMP441 RMS -> Speaker Volume
// Reads ambient noise from I2S RX, calculates RMS,
// maps to volume 1-21 for the I2S speaker output
// ============================================================

#define AV_SAMPLE_RATE    16000
#define AV_READ_SAMPLES   1024
#define AV_READ_MS        500
#define AV_TASK_STACK     3072
#define AV_TASK_PRIO      1

static i2s_chan_handle_t micRxHandle = NULL;
static int16_t *micBuf = NULL;

static bool init_mic_i2s(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &micRxHandle);
    if (err != ESP_OK) {
        Serial.printf("[AUTO_VOL] i2s_new_channel failed: 0x%x\n", err);
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AV_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)MIC_BCLK,
            .ws   = (gpio_num_t)MIC_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)MIC_DATA_IN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };

    err = i2s_channel_init_std_mode(micRxHandle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("[AUTO_VOL] i2s_channel_init_std_mode failed: 0x%x\n", err);
        return false;
    }

    err = i2s_channel_enable(micRxHandle);
    if (err != ESP_OK) {
        Serial.printf("[AUTO_VOL] i2s_channel_enable failed: 0x%x\n", err);
        return false;
    }

    return true;
}

static uint8_t rms_to_volume(float rms) {
    if (rms < AV_RMS_QUIET) return 5;
    if (rms < AV_RMS_MODERATE) return 10;
    if (rms < AV_RMS_LOUD) return 16;
    return 21;
}

static void auto_volume_task(void *pvParameters) {
    if (!init_mic_i2s()) {
        Serial.println("[AUTO_VOL] Mic I2S init failed, task halting");
        vTaskDelete(NULL);
        return;
    }

    micBuf = (int16_t *)ps_malloc(AV_READ_SAMPLES * sizeof(int16_t));
    if (!micBuf) {
        Serial.println("[AUTO_VOL] ps_malloc failed, task halting");
        vTaskDelete(NULL);
        return;
    }

    Serial.println("[AUTO_VOL] Task started - ambient noise -> volume mapping");

    while (true) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(micRxHandle, micBuf,
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

        Serial.printf("[AUTO_VOL] RMS: %.0f -> Volume: %d/21\n", rms, vol);

        vTaskDelay(pdMS_TO_TICKS(AV_READ_MS));
    }
}

void auto_volume_task_start(void) {
    xTaskCreatePinnedToCore(auto_volume_task, "auto_vol", AV_TASK_STACK,
                            NULL, AV_TASK_PRIO, NULL, 1);
}
