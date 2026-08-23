#include "tone_driver.h"
#include <math.h>
#include <string.h>
#include "driver/i2s.h"

#define TONE_QUEUE_SIZE    8
#define TONE_TASK_STACK    4096
#define TONE_TASK_PRIO     3

// --- I2S output: MAX98357A Class-D Amp -> Grove Speaker ---
// ESP32-S3 Master TX trên I2S_SPK_PORT (I2S_NUM_1), tách biệt
// mic RX trên I2S_MIC_PORT (I2S_NUM_0).
#define SPK_DMA_BUF_COUNT  8
#define SPK_DMA_BUF_LEN    256

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

static bool stop_check_stream(void) { return !streamActive || toneStopFlag; }

static bool spk_install_i2s(void) {
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = TONE_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = SPK_DMA_BUF_COUNT,
        .dma_buf_len = SPK_DMA_BUF_LEN,
        .use_apll = false,
    };

    i2s_pin_config_t pin_cfg = {
        .bck_io_num = AMP_I2S_BCLK,
        .ws_io_num = AMP_I2S_LRC,
        .data_out_num = AMP_I2S_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    if (i2s_driver_install(I2S_SPK_PORT, &i2s_cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_SPK_PORT, &pin_cfg) != ESP_OK) {
        i2s_driver_uninstall(I2S_SPK_PORT);
        return false;
    }
    i2s_zero_dma_buffer(I2S_SPK_PORT);
    return true;
}

// Ghi PCM mono ra I2S stereo (nhân đôi kênh cho MAX98357A),
// áp volume scaling, ghi block để DMA kịp trống.
static void i2s_output(const int16_t *buf, uint32_t samples, bool (*checkStop)(void)) {
    static int16_t frame[256][2];
    float volScale = currentVolume / 21.0f;

    uint32_t pos = 0;
    while (pos < samples) {
        if (checkStop && checkStop()) break;
        uint32_t chunk = samples - pos;
        if (chunk > 256) chunk = 256;
        for (uint32_t i = 0; i < chunk; i++) {
            int32_t s = (int32_t)(buf[pos + i] * volScale);
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            frame[i][0] = (int16_t)s;
            frame[i][1] = (int16_t)s;
        }
        size_t written = 0;
        i2s_write(I2S_SPK_PORT, frame, chunk * sizeof(frame[0]), &written, portMAX_DELAY);
        pos += chunk;
    }
}

bool tone_driver_init(void) {
    if (!spk_install_i2s()) {
        Serial.println("[TONE] I2S init failed for MAX98357A!");
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

    Serial.printf("[TONE] I2S speaker ready (MAX98357A DIN=%d LRC=%d BCLK=%d @ %dHz)\n",
                  AMP_I2S_DIN, AMP_I2S_LRC, AMP_I2S_BCLK, TONE_SAMPLE_RATE);
    return true;
}

// Phát sóng vuông freqHz trong durationMs qua I2S (software synthesized)
static void play_tone_request(const tone_request_t &req) {
    const uint32_t halfPeriodSamples = (TONE_SAMPLE_RATE / req.freqHz) / 2;
    if (halfPeriodSamples == 0) return;

    // Amplitude theo volume: 5..100% full scale (SOS cần tối đa)
    const int32_t amp = (int32_t)(32767.0f * (0.05f + 0.95f * req.volume / 21.0f));

    uint32_t totalSamples = ((uint64_t)TONE_SAMPLE_RATE * req.durationMs) / 1000;
    uint32_t phase = 0;
    uint32_t generated = 0;

    while (generated < totalSamples) {
        if (toneStopFlag || streamActive) break;

        uint32_t n = totalSamples - generated;
        if (n > TONE_BUF_SAMPLES) n = TONE_BUF_SAMPLES;

        for (uint32_t i = 0; i < n; i++) {
            toneBuf[i] = (phase < halfPeriodSamples) ? (int16_t)amp : (int16_t)-amp;
            if (++phase >= halfPeriodSamples * 2) phase = 0;
        }

        // Bíp dùng volume của chính request (SOS cần max 21, bíp US theo auto-volume)
        uint8_t savedVolume = currentVolume;
        currentVolume = req.volume;
        i2s_output(toneBuf, n, NULL);
        currentVolume = savedVolume;

        generated += n;
    }

    // Im lặng cuối bíp để ngắt âm sạch
    memset(toneBuf, 0, TONE_BUF_SAMPLES * sizeof(int16_t));
    i2s_output(toneBuf, TONE_BUF_SAMPLES / 4, NULL);
}

static void tone_task(void *pvParameters) {
    tone_request_t req;

    while (true) {
        if (streamActive) {
            size_t avail = (streamWriteIdx - streamReadIdx + streamCapacity) % streamCapacity;
            if (avail >= TONE_BUF_SAMPLES) {
                for (size_t i = 0; i < TONE_BUF_SAMPLES; i++) {
                    toneBuf[i] = streamBuf[streamReadIdx];
                    streamReadIdx = (streamReadIdx + 1) % streamCapacity;
                }
                i2s_output(toneBuf, TONE_BUF_SAMPLES, stop_check_stream);
                continue;
            }
            if (xQueueReceive(toneQueue, &req, pdMS_TO_TICKS(5)) == pdTRUE) {
                goto play_tone;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (xQueueReceive(toneQueue, &req, pdMS_TO_TICKS(100)) == pdTRUE) {
play_tone:
            tonePlaying = true;
            toneStopFlag = false;
            play_tone_request(req);
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
        i2s_zero_dma_buffer(I2S_SPK_PORT);
    }
}

bool tone_driver_stream_is_active(void) {
    return streamActive;
}

size_t tone_driver_stream_available(void) {
    if (!streamActive) return 0;
    return (streamWriteIdx - streamReadIdx + streamCapacity) % streamCapacity;
}
