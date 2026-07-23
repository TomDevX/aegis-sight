#include "tone_driver.h"
#include <math.h>
#include <string.h>

#define TONE_QUEUE_SIZE    8
#define TONE_TASK_STACK    4096
#define TONE_TASK_PRIO     3

static int16_t *toneBuf = NULL;
static volatile bool tonePlaying = false;
static volatile bool toneStopFlag = false;
static volatile uint8_t currentVolume = 12;

static QueueHandle_t toneQueue = NULL;

// --- AI audio stream ring buffer ---
static int16_t *streamBuf = NULL;
static volatile size_t streamWriteIdx = 0;
static volatile size_t streamReadIdx  = 0;
static size_t streamCapacity = 0;
static volatile bool streamActive = false;

bool tone_driver_init(void) {
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = TONE_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 512,
        .use_apll = false,
    };

    i2s_pin_config_t pin_cfg = {
        .bck_io_num = SPK_BCLK,
        .ws_io_num = SPK_LRCK,
        .data_out_num = SPK_DATA_OUT,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    esp_err_t err = i2s_driver_install(I2S_SPK_PORT, &i2s_cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[TONE] i2s_driver_install failed: 0x%x\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_SPK_PORT, &pin_cfg);
    if (err != ESP_OK) {
        Serial.printf("[TONE] i2s_set_pin failed: 0x%x\n", err);
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

    Serial.printf("[TONE] I2S TX init OK (port=%d, buf=%d samples on PSRAM)\n",
                  I2S_SPK_PORT, TONE_BUF_SAMPLES);
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
        if (streamActive) {
            size_t avail = (streamWriteIdx - streamReadIdx + streamCapacity) % streamCapacity;
            if (avail >= TONE_BUF_SAMPLES) {
                size_t toRead = TONE_BUF_SAMPLES;
                float volScale = currentVolume / 21.0f;
                for (size_t i = 0; i < toRead; i++) {
                    toneBuf[i] = (int16_t)(streamBuf[streamReadIdx] * volScale);
                    streamReadIdx = (streamReadIdx + 1) % streamCapacity;
                }
                size_t bytesWritten = 0;
                i2s_write(I2S_SPK_PORT, toneBuf, toRead * sizeof(int16_t),
                          &bytesWritten, pdMS_TO_TICKS(50));
                continue;
            }
            if (xQueueReceive(toneQueue, &req, pdMS_TO_TICKS(5)) == pdTRUE) {
                goto play_tone;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (xQueueReceive(toneQueue, &req, pdMS_TO_TICKS(100)) == pdTRUE) {
play_tone:
            tonePlaying = true;
            toneStopFlag = false;

            uint32_t totalSamples = (uint32_t)TONE_SAMPLE_RATE * req.durationMs / 1000;
            uint32_t written = 0;

            while (written < totalSamples && !toneStopFlag) {
                if (streamActive) {
                    tonePlaying = false;
                    break;
                }
                uint32_t chunk = totalSamples - written;
                if (chunk > TONE_BUF_SAMPLES) chunk = TONE_BUF_SAMPLES;

                generate_tone_chunk(toneBuf, chunk, req.freqHz, req.volume);

                size_t bytesWritten = 0;
                i2s_write(I2S_SPK_PORT, toneBuf, chunk * sizeof(int16_t),
                          &bytesWritten, pdMS_TO_TICKS(50));
                written += bytesWritten / sizeof(int16_t);
            }

            memset(toneBuf, 0, TONE_BUF_SAMPLES * sizeof(int16_t));
            size_t silenceWritten = 0;
            i2s_write(I2S_SPK_PORT, toneBuf, TONE_BUF_SAMPLES * sizeof(int16_t),
                      &silenceWritten, pdMS_TO_TICKS(50));

            tonePlaying = false;
        }
    }
}

void tone_driver_start_task(void) {
    xTaskCreatePinnedToCore(tone_task, "tone_task", TONE_TASK_STACK,
                            NULL, TONE_TASK_PRIO, NULL, 1);
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

void tone_driver_stream_init(void) {
    if (streamBuf) return;
    streamCapacity = AI_PCM_RINGBUF_SIZE / sizeof(int16_t);
    streamBuf = (int16_t *)ps_malloc(AI_PCM_RINGBUF_SIZE);
    if (!streamBuf) {
        Serial.println("[TONE] ps_malloc failed for stream ring buffer");
        return;
    }
    streamWriteIdx = 0;
    streamReadIdx  = 0;
    streamActive   = false;
    Serial.printf("[TONE] Stream ring buffer: %zu samples (%zu KB on PSRAM)\n",
                  streamCapacity, AI_PCM_RINGBUF_SIZE / 1024);
}

bool tone_driver_stream_write(const int16_t *data, size_t samples) {
    if (!streamBuf || !streamActive) return false;

    size_t space = (streamReadIdx - streamWriteIdx + streamCapacity - 1) % streamCapacity;
    if (samples > space) {
        return false;
    }

    for (size_t i = 0; i < samples; i++) {
        streamBuf[streamWriteIdx] = data[i];
        streamWriteIdx = (streamWriteIdx + 1) % streamCapacity;
    }
    return true;
}

void tone_driver_stream_set_active(bool active) {
    streamActive = active;
    if (!active) {
        streamWriteIdx = 0;
        streamReadIdx  = 0;
    }
}

bool tone_driver_stream_is_active(void) {
    return streamActive;
}

size_t tone_driver_stream_available(void) {
    if (!streamActive) return 0;
    return (streamWriteIdx - streamReadIdx + streamCapacity) % streamCapacity;
}
