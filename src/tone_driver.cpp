#include "tone_driver.h"
#include <math.h>
#include <string.h>
#include "driver/ledc.h"

#define TONE_QUEUE_SIZE    8
#define TONE_TASK_STACK    4096
#define TONE_TASK_PRIO     3

// --- PWM output: Seeed Grove Speaker (SIG = PWM, ESP32-S3 has no DAC) ---
#define PWM_CHANNEL        2    // camera uses LEDC_CHANNEL_0 / LEDC_TIMER_0
#define PWM_RES_BITS       10
#define PWM_RES_MAX        ((1 << PWM_RES_BITS) - 1)  // 1023
#define PWM_CARRIER_HZ     78125                       // 80MHz / 2^10
#define PWM_DC_MID         (PWM_RES_MAX / 2)           // ~silence

// 1000000 / TONE_SAMPLE_RATE = 62.5us -> alternate 62/63 per sample
#define SAMPLE_PERIOD_US_LO   62
#define SAMPLE_PERIOD_US_HI   63

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

static uint32_t ledcFreqHz = 0;

// Ép clock APB 80MHz. Nếu để AUTO, driver có thể chọn XTAL 40MHz -> 10-bit
// @ 78.125kHz vượt giới hạn -> ledcSetup fail -> loa phát rác rè rè.
static bool ledc_set_freq(uint32_t f) {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = (ledc_timer_t)LEDC_TIMER_1;
    timer.duty_resolution = (ledc_timer_bit_t)PWM_RES_BITS;
    timer.freq_hz = f;
    timer.clk_cfg = LEDC_USE_APB_CLK;
    return ledc_timer_config(&timer) == ESP_OK;
}

static void set_ledc_freq(uint32_t f) {
    if (ledcFreqHz != f) {
        ledc_set_freq(f);
        ledcFreqHz = f;
    }
}

// Play int16 PCM through PWM, paced at TONE_SAMPLE_RATE.
// checkStop (optional): abort early when true.
static void pwm_output(const int16_t *buf, uint32_t samples, bool (*checkStop)(void)) {
    set_ledc_freq(PWM_CARRIER_HZ);
    int64_t next = esp_timer_get_time();
    for (uint32_t i = 0; i < samples; i++) {
        if (checkStop && checkStop()) break;
        int32_t d = PWM_DC_MID + ((int32_t)buf[i] >> (16 - PWM_RES_BITS));
        if (d < 0) d = 0;
        else if (d > PWM_RES_MAX) d = PWM_RES_MAX;
        ledcWrite(PWM_CHANNEL, (uint32_t)d);
        next += (i & 1) ? SAMPLE_PERIOD_US_HI : SAMPLE_PERIOD_US_LO;
        while (esp_timer_get_time() < next) {}
    }
}

bool tone_driver_init(void) {
    if (!ledc_set_freq(PWM_CARRIER_HZ)) {
        Serial.printf("[TONE] PWM timer config failed (%dHz) -> speaker may be noisy\n", PWM_CARRIER_HZ);
    }
    ledcAttachPin(SPK_PWM_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0);   // duty 0 = im lặng (50% sẽ rè rè liên tục trên Grove amp)
    ledcFreqHz = PWM_CARRIER_HZ;

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

    Serial.printf("[TONE] PWM speaker ready (pin=%d, ch=%d, %d-bit @ %dHz)\n",
                  SPK_PWM_PIN, PWM_CHANNEL, PWM_RES_BITS, PWM_CARRIER_HZ);
    return true;
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
                pwm_output(toneBuf, toRead, stop_check_stream);
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

            // Sóng vuông tần số = req.freqHz: sạch & to trên Grove Speaker
            set_ledc_freq(req.freqHz);
            uint32_t pct = 5 + (45u * req.volume) / 21u;   // 5..50% duty
            ledcWrite(PWM_CHANNEL, (uint32_t)1023u * pct / 100u);

            uint32_t t0 = millis();
            while (millis() - t0 < req.durationMs) {
                if (toneStopFlag || streamActive) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            set_ledc_freq(PWM_CARRIER_HZ);
            ledcWrite(PWM_CHANNEL, 0);   // tắt hẳn sau bíp, tránh rè rè
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
