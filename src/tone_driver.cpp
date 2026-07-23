#include "tone_driver.h"
#include <math.h>

// ============================================================
// Tone Driver Implementation - I2S TX on Core 1
// Generates sine wave PCM and streams to PCM5102A DAC
// ============================================================

#define TONE_QUEUE_SIZE    8
#define TONE_TASK_STACK    4096
#define TONE_TASK_PRIO     3

static i2s_chan_handle_t txHandle = NULL;
static int16_t *toneBuf = NULL;
static volatile bool tonePlaying = false;
static volatile bool toneStopFlag = false;
static volatile uint8_t currentVolume = 12;

static QueueHandle_t toneQueue = NULL;

bool tone_driver_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPK_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &txHandle, NULL);
    if (err != ESP_OK) {
        Serial.printf("[TONE] i2s_new_channel failed: 0x%x\n", err);
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)SPK_BCLK,
            .ws   = (gpio_num_t)SPK_LRCK,
            .dout = (gpio_num_t)SPK_DATA_OUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };

    err = i2s_channel_init_std_mode(txHandle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("[TONE] i2s_channel_init_std_mode failed: 0x%x\n", err);
        return false;
    }

    err = i2s_channel_enable(txHandle);
    if (err != ESP_OK) {
        Serial.printf("[TONE] i2s_channel_enable failed: 0x%x\n", err);
        return false;
    }

    toneBuf = (int16_t *)ps_malloc(TONE_BUF_SAMPLES * sizeof(int16_t));
    if (!toneBuf) {
        Serial.println("[TONE] ps_malloc failed for tone buffer");
        return false;
    }

    toneQueue = xQueueCreate(TONE_QUEUE_SIZE, sizeof(tone_request_t));
    if (!toneQueue) {
        Serial.println("[TONE] xQueueCreate failed");
        return false;
    }

    Serial.printf("[TONE] I2S TX init OK (buf=%d samples on PSRAM)\n", TONE_BUF_SAMPLES);
    return true;
}

static void generate_tone_chunk(int16_t *buf, uint32_t samples,
                                 uint16_t freq, uint8_t vol) {
    static uint32_t phase = 0;
    float amplitude = (vol / 21.0f) * 16000.0f;

    if (freq == 0) {
        memset(buf, 0, samples * sizeof(int16_t));
        return;
    }

    for (uint32_t i = 0; i < samples; i++) {
        float t = (float)phase / TONE_SAMPLE_RATE;
        buf[i] = (int16_t)(amplitude * sinf(2.0f * M_PI * freq * t));
        phase++;
    }
    phase %= TONE_SAMPLE_RATE;
}

static void tone_task(void *pvParameters) {
    tone_request_t req;

    while (true) {
        if (xQueueReceive(toneQueue, &req, pdMS_TO_TICKS(100)) == pdTRUE) {
            tonePlaying = true;
            toneStopFlag = false;

            uint32_t totalSamples = (uint32_t)TONE_SAMPLE_RATE * req.durationMs / 1000;
            uint32_t written = 0;

            Serial.printf("[TONE] Playing %uHz for %lu ms (vol=%d)\n",
                          req.freqHz, req.durationMs, req.volume);

            while (written < totalSamples && !toneStopFlag) {
                uint32_t chunk = totalSamples - written;
                if (chunk > TONE_BUF_SAMPLES) chunk = TONE_BUF_SAMPLES;

                generate_tone_chunk(toneBuf, chunk, req.freqHz, req.volume);

                size_t bytesWritten = 0;
                i2s_write(txHandle, toneBuf, chunk * sizeof(int16_t),
                          &bytesWritten, pdMS_TO_TICKS(50));
                written += bytesWritten / sizeof(int16_t);
            }

            // Silence after tone
            memset(toneBuf, 0, TONE_BUF_SAMPLES * sizeof(int16_t));
            size_t silenceWritten = 0;
            i2s_write(txHandle, toneBuf, TONE_BUF_SAMPLES * sizeof(int16_t),
                      &silenceWritten, pdMS_TO_TICKS(50));

            tonePlaying = false;
        }
    }
}

void tone_driver_start_task(void) {
    xTaskCreatePinnedToCore(tone_task, "tone_task", TONE_TASK_STACK,
                            NULL, TONE_TASK_PRIO, NULL, 1);
    Serial.println("[TONE] Task started on Core 1");
}

bool tone_driver_play(uint16_t freqHz, uint32_t durationMs, uint8_t volume) {
    if (!toneQueue) return false;
    tone_request_t req = { freqHz, durationMs, volume };
    return xQueueSend(toneQueue, &req, 0) == pdTRUE;
}

void tone_driver_stop(void) {
    toneStopFlag = true;
    xQueueReset(toneQueue);
}

bool tone_driver_is_playing(void) {
    return tonePlaying;
}

void tone_driver_set_volume(uint8_t vol) {
    if (vol > 21) vol = 21;
    currentVolume = vol;
}

uint8_t tone_driver_get_volume(void) {
    return currentVolume;
}
