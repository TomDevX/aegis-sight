#include "tone_driver.h"
#include <math.h>
#include <string.h>
#include "driver/i2s.h"
#include "AudioFileSourcePROGMEM.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h"

#define TONE_QUEUE_SIZE    16
#define TONE_TASK_STACK    8192
#define TONE_TASK_PRIO     4

// DMA Buffers (8 buffers x 1024 samples)
#define SPK_DMA_BUF_COUNT  8
#define SPK_DMA_BUF_LEN    1024

// Ngưỡng Pre-buffering: đệm siêu nhanh ~80ms là phát ngay ra loa
#define STREAM_PREBUFFER_SAMPLES (TONE_SAMPLE_RATE * 8 / 100)

static int16_t *toneBuf = NULL;
static volatile bool tonePlaying = false;
static volatile bool toneStopFlag = false;
static volatile uint8_t currentVolume = 21; // Âm lượng cực đại (Max 21)

static QueueHandle_t toneQueue = NULL;
static uint32_t currentSampleRate = TONE_SAMPLE_RATE;

// --- PSRAM Ring Buffer (cho AI TTS Voice) ---
static int16_t *streamBuf = NULL;
static volatile size_t streamWriteIdx = 0;
static volatile size_t streamReadIdx  = 0;
static size_t streamCapacity = 0;
static volatile bool streamActive = false;
static volatile bool streamBuffering = true;
static uint32_t lastStreamDataMs = 0;

// --- PSRAM Pre-decoded Waiting Music (Zero-CPU) ---
static int16_t *waitingMusicBuf = NULL;
static size_t waitingMusicTotalSamples = 0;
static size_t waitingMusicPlayIdx = 0;
static volatile bool waitingMusicActive = false;

static bool stop_check_stream(void) { return !streamActive; }
static bool stop_check_waiting(void) { return !waitingMusicActive || streamActive || toneStopFlag; }

static bool spk_install_i2s(void) {
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = currentSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = SPK_DMA_BUF_COUNT,
        .dma_buf_len = SPK_DMA_BUF_LEN,
        .use_apll = false, // Dùng PLL nội chuẩn, tránh lỗi divider gây lệch clock trên S3
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
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

// Cập nhật sample rate nếu MP3 giải mã ra tần số khác (ví dụ: 24kHz từ Google TTS)
void tone_driver_set_sample_rate(uint32_t rate) {
    if (rate != currentSampleRate && rate >= 8000 && rate <= 48000) {
        currentSampleRate = rate;
        i2s_set_sample_rates(I2S_SPK_PORT, currentSampleRate);
        Serial.printf("[TONE] Switched I2S Sample Rate to %u Hz\n", rate);
    }
}

// Ghi dữ liệu PCM ra 2 kênh stereo cho MAX98357A (Mono -> Stereo duplicated)
static void i2s_output(const int16_t *buf, uint32_t samples, bool (*checkStop)(void)) {
    static int16_t frame[512][2];
    
    // Thang âm lượng 1..21: dải scale 0.70f .. 1.50f cho âm lượng cực đại, to vang khắp phòng
    float volScale = 0.0f;
    if (currentVolume > 0) {
        volScale = 0.70f + (float)(currentVolume - 1) * (0.80f / 20.0f);
    }

    uint32_t pos = 0;
    while (pos < samples) {
        if (checkStop && checkStop()) break;
        uint32_t chunk = samples - pos;
        if (chunk > 512) chunk = 512;

        for (uint32_t i = 0; i < chunk; i++) {
            int32_t s = (int32_t)((float)buf[pos + i] * volScale);
            // Soft clipping an toàn
            if (s > 32000) s = 32000;
            else if (s < -32000) s = -32000;
            frame[i][0] = (int16_t)s;
            frame[i][1] = (int16_t)s;
        }

        uint8_t *pBytes = (uint8_t *)frame;
        size_t bytesLeft = chunk * sizeof(frame[0]);

        while (bytesLeft > 0) {
            if (checkStop && checkStop()) break;
            size_t written = 0;
            esp_err_t err = i2s_write(I2S_SPK_PORT, pBytes, bytesLeft, &written, pdMS_TO_TICKS(100));
            if (err == ESP_OK && written > 0) {
                pBytes += written;
                bytesLeft -= written;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
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

    Serial.printf("[TONE] I2S Speaker Ready (MAX98357A DIN=%d LRC=%d BCLK=%d @ %dHz)\n",
                  AMP_I2S_DIN, AMP_I2S_LRC, AMP_I2S_BCLK, currentSampleRate);
    return true;
}

static void play_tone_request(const tone_request_t &req) {
    if (req.freqHz == 0 || req.durationMs == 0) return;

    // Reset về sample rate chuẩn khi phát còi/bíp
    tone_driver_set_sample_rate(TONE_SAMPLE_RATE);

    float volScale = (req.volume == 0) ? 0.0f : (0.30f + (float)req.volume * (0.70f / 21.0f));
    const int32_t amp = (int32_t)(32000.0f * volScale);

    uint32_t totalSamples = ((uint64_t)TONE_SAMPLE_RATE * req.durationMs) / 1000;
    if (totalSamples == 0) return;

    float phaseInc = (2.0f * (float)M_PI * (float)req.freqHz) / (float)TONE_SAMPLE_RATE;
    float phase = 0.0f;
    uint32_t generated = 0;

    // Smooth Raised-Cosine Fade (12ms)
    uint32_t fadeSamples = (TONE_SAMPLE_RATE * 12) / 1000;
    if (fadeSamples > totalSamples / 3) fadeSamples = totalSamples / 3;
    if (fadeSamples == 0) fadeSamples = 1;

    while (generated < totalSamples) {
        if (toneStopFlag || streamActive) break;

        uint32_t n = totalSamples - generated;
        if (n > TONE_BUF_SAMPLES) n = TONE_BUF_SAMPLES;

        for (uint32_t i = 0; i < n; i++) {
            uint32_t sampleIdx = generated + i;
            float env = 1.0f;
            if (sampleIdx < fadeSamples) {
                float f = (float)sampleIdx / (float)fadeSamples;
                env = 0.5f * (1.0f - cosf(f * (float)M_PI));
            } else if (sampleIdx > totalSamples - fadeSamples) {
                float f = (float)(totalSamples - sampleIdx) / (float)fadeSamples;
                env = 0.5f * (1.0f - cosf(f * (float)M_PI));
            }

            // Sóng còi ấm áp chuẩn nguyên bản
            float wave = 0.88f * sinf(phase) + 0.12f * sinf(phase * 2.0f);
            toneBuf[i] = (int16_t)(wave * env * (float)amp);

            phase += phaseInc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }

        // Ghi trực tiếp ra I2S
        static int16_t frame[512][2];
        for (uint32_t i = 0; i < n; i++) {
            frame[i][0] = toneBuf[i];
            frame[i][1] = toneBuf[i];
        }
        size_t written = 0;
        i2s_write(I2S_SPK_PORT, frame, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        generated += n;
    }
}

static void tone_task(void *pvParameters) {
    tone_request_t req;

    while (true) {
        // 1. Ưu tiên số 1: Giọng nói AI từ Google TTS (Stream Ring Buffer)
        if (streamActive) {
            size_t r = streamReadIdx;
            size_t w = streamWriteIdx;
            size_t avail = (w - r + streamCapacity) % streamCapacity;

            // Pre-buffering: chờ đệm đủ âm thanh trước khi phát
            if (streamBuffering) {
                if (avail >= STREAM_PREBUFFER_SAMPLES) {
                    streamBuffering = false;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
            }

            if (avail > 0) {
                lastStreamDataMs = millis();
                size_t chunk = (avail > 512) ? 512 : avail;
                for (size_t i = 0; i < chunk; i++) {
                    toneBuf[i] = streamBuf[(r + i) % streamCapacity];
                }
                streamReadIdx = (r + chunk) % streamCapacity;
                i2s_output(toneBuf, chunk, stop_check_stream);
                continue;
            } else {
                streamBuffering = true;
                if (lastStreamDataMs > 0 && millis() - lastStreamDataMs > 1000) {
                    streamActive = false;
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        // 2. Nhạc chờ Elevator Music từ PSRAM (khi đang chờ AI phản hồi)
        if (waitingMusicActive && waitingMusicBuf && waitingMusicTotalSamples > 0) {
            // Kiểm tra nếu có cảnh báo siêu âm hoặc SOS trong toneQueue -> phát ưu tiên ngay lập tức không để dồn queue!
            if (xQueueReceive(toneQueue, &req, 0) == pdTRUE) {
                tonePlaying = true;
                toneStopFlag = false;
                play_tone_request(req);
                tonePlaying = false;
                continue;
            }

            static int16_t waitChunk[256];
            size_t chunk = 256;
            for (size_t i = 0; i < chunk; i++) {
                int32_t s = (int32_t)((float)waitingMusicBuf[waitingMusicPlayIdx] * 0.60f);
                waitChunk[i] = (int16_t)s;
                waitingMusicPlayIdx++;
                if (waitingMusicPlayIdx >= waitingMusicTotalSamples) {
                    waitingMusicPlayIdx = 0;
                }
            }
            // Phát trực tiếp ra I2S với callback ngắt tức thì nếu có streamActive hoặc tắt waitingMusic
            i2s_output(waitChunk, chunk, stop_check_waiting);
            continue;
        }

        // 3. Chuông, còi bíp, còi SOS từ hàng đợi toneQueue khi không có nhạc chờ
        if (xQueueReceive(toneQueue, &req, pdMS_TO_TICKS(20)) == pdTRUE) {
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

bool tone_driver_is_playing(void) { return tonePlaying; }
void tone_driver_set_volume(uint8_t vol) { currentVolume = (vol > 21) ? 21 : vol; }
uint8_t tone_driver_get_volume(void) { return currentVolume; }

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
    streamBuffering = true;
}

bool tone_driver_stream_write(const int16_t *data, size_t samples) {
    if (!streamBuf) return false;
    streamActive = true;

    size_t r = streamReadIdx;
    size_t w = streamWriteIdx;
    size_t space = (r - w + streamCapacity - 1) % streamCapacity;
    if (samples > space) return false;

    for (size_t i = 0; i < samples; i++) {
        streamBuf[(w + i) % streamCapacity] = data[i];
    }
    streamWriteIdx = (w + samples) % streamCapacity;
    lastStreamDataMs = millis();
    return true;
}

void tone_driver_stream_set_active(bool active) {
    streamActive = active;
    streamBuffering = true;
    lastStreamDataMs = millis();
    if (!active) {
        streamWriteIdx = 0;
        streamReadIdx  = 0;
        i2s_zero_dma_buffer(I2S_SPK_PORT);
    }
}

bool tone_driver_stream_is_active(void) { return streamActive; }
size_t tone_driver_stream_available(void) {
    if (!streamActive) return 0;
    size_t r = streamReadIdx;
    size_t w = streamWriteIdx;
    return (w - r + streamCapacity) % streamCapacity;
}

// ============================================================
// Sound Effects & Realistic Airplane Chimes (Exponential Decay)
// ============================================================

// Hàm tổng hợp tiếng chuông ngân vang tắt dần tự nhiên (Acoustic Bell Synthesizer)
static void play_bell_tone(float freqHz, uint32_t durationMs, float decaySpeed, uint8_t vol) {
    if (vol > 21) vol = 21;
    if (vol == 0) return;

    int32_t amp = 3000 + (int32_t)(vol - 1) * (26000 / 20);
    uint32_t totalSamples = ((uint64_t)TONE_SAMPLE_RATE * durationMs) / 1000;
    if (totalSamples == 0) return;

    float phaseInc = (2.0f * (float)M_PI * freqHz) / (float)TONE_SAMPLE_RATE;
    float phase = 0.0f;
    uint32_t generated = 0;

    static int16_t frame[512][2];

    while (generated < totalSamples) {
        if (toneStopFlag) break;

        uint32_t n = totalSamples - generated;
        if (n > 512) n = 512;

        for (uint32_t i = 0; i < n; i++) {
            uint32_t sampleIdx = generated + i;
            float tSec = (float)sampleIdx / (float)TONE_SAMPLE_RATE;

            // Đường bao tắt dần mượt mà tự nhiên (Exponential Decay)
            float env = expf(-tSec * decaySpeed);
            if (sampleIdx < 80) { // Fade-in 3ms chống tiếng click
                env *= (float)sampleIdx / 80.0f;
            }

            // Hài âm chuông ấm (Fundamental + Overtones)
            float wave = 0.82f * sinf(phase) + 0.15f * sinf(phase * 2.0f) + 0.03f * sinf(phase * 3.0f);
            int32_t s = (int32_t)(wave * env * (float)amp);
            if (s > 30000) s = 30000;
            else if (s < -30000) s = -30000;

            frame[i][0] = (int16_t)s;
            frame[i][1] = (int16_t)s;

            phase += phaseInc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }

        size_t written = 0;
        i2s_write(I2S_SPK_PORT, frame, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        generated += n;
    }
}

// 1. Hiệu ứng âm thanh khởi động thiết bị (Startup Resonant Arpeggio Chime)
void tone_driver_play_startup(void) {
    toneStopFlag = false;
    streamActive = false;
    play_bell_tone(523, 130, 4.5f, 20); // C5
    vTaskDelay(pdMS_TO_TICKS(110));
    play_bell_tone(659, 130, 4.5f, 20); // E5
    vTaskDelay(pdMS_TO_TICKS(110));
    play_bell_tone(784, 150, 4.0f, 20); // G5
    vTaskDelay(pdMS_TO_TICKS(130));
    play_bell_tone(1046, 550, 2.5f, 21); // C6 (ngân dài thanh thoát)
    vTaskDelay(pdMS_TO_TICKS(580));
}

// 2. Tiếng chuông Captain Speaking trên máy bay (Cabin PA Chime: Ding-Dong nhanh gọn, sang trọng ~230ms)
void tone_driver_play_captain_chime(void) {
    toneStopFlag = false;
    streamActive = false;
    // Nốt "Ding" (E5 - 659Hz) nhanh thanh thoát 90ms
    play_bell_tone(659.25f, 90, 7.5f, 21);
    vTaskDelay(pdMS_TO_TICKS(15));
    // Nốt "Dong" (A4 - 440Hz) trầm ấm 130ms
    play_bell_tone(440.00f, 130, 6.5f, 21);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// 2b. Tiếng "Tít" thanh mảnh siêu nhanh (Chirp ~35ms) cho phản hồi bấm nút tức thì
void tone_driver_play_quick_beep(void) {
    toneStopFlag = false;
    streamActive = false;
    play_bell_tone(880.0f, 35, 12.0f, 18); // Nốt A5 880Hz trong 35ms
}

// 2c. Tiếng "Tít-Tít" đôi (~90ms) báo hiệu Nối Tiếp Cuộc Trò Chuyện (Follow-up Turn)
void tone_driver_play_double_beep(void) {
    toneStopFlag = false;
    streamActive = false;
    play_bell_tone(880.0f, 30, 14.0f, 18);
    vTaskDelay(pdMS_TO_TICKS(25));
    play_bell_tone(1046.5f, 35, 12.0f, 20); // Nốt C6 cao hơn vui tai
}

// 3. Tiếng âm báo ngắt mic khi nhả nút (Warm Soft Two-Tone Chime: êm dịu, dứt khoát ~190ms)
void tone_driver_play_release_chime(void) {
    toneStopFlag = false;
    streamActive = false;
    play_bell_tone(587.33f, 80, 8.0f, 19); // Nốt D5 êm nhẹ 80ms
    vTaskDelay(pdMS_TO_TICKS(10));
    play_bell_tone(440.00f, 110, 7.0f, 19); // Nốt A4 trầm ấm 110ms
    vTaskDelay(pdMS_TO_TICKS(15));
}

// ============================================================
// Zero-CPU Pre-decoded PSRAM Waiting Music
// ============================================================
class AudioOutputToPcmBuffer : public AudioOutput {
private:
    int16_t *targetBuf;
    size_t maxSamples;
    size_t currentSamples;
public:
    AudioOutputToPcmBuffer(int16_t *buf, size_t maxS) : targetBuf(buf), maxSamples(maxS), currentSamples(0) {}
    virtual bool begin() override { currentSamples = 0; return true; }
    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (currentSamples >= maxSamples) return false;
        int32_t mixed = ((int32_t)sample[0] + (int32_t)sample[1]) >> 1;
        targetBuf[currentSamples++] = (int16_t)mixed;
        return true;
    }
    virtual bool stop() override { return true; }
    size_t getSamples() const { return currentSamples; }
};

void tone_driver_predecode_waiting_music(const uint8_t *mp3Data, size_t mp3Len) {
    if (!mp3Data || mp3Len == 0) return;

    // Cấp phát 12 giây ở sample rate hiện tại (288,000 samples = 576KB PSRAM)
    size_t maxSamples = (size_t)currentSampleRate * 12;
    waitingMusicBuf = (int16_t *)ps_malloc(maxSamples * sizeof(int16_t));
    if (!waitingMusicBuf) {
        Serial.println("[TONE] Cấp phát PSRAM lưu nhạc chờ thất bại!");
        return;
    }

    AudioFileSourcePROGMEM *file = new AudioFileSourcePROGMEM(mp3Data, mp3Len);
    AudioOutputToPcmBuffer *out = new AudioOutputToPcmBuffer(waitingMusicBuf, maxSamples);
    AudioGeneratorMP3 *mp3 = new AudioGeneratorMP3();

    out->begin();
    if (mp3->begin(file, out)) {
        while (mp3->isRunning() && out->getSamples() < maxSamples) {
            if (!mp3->loop()) break;
        }
    }
    waitingMusicTotalSamples = out->getSamples();

    out->stop();
    delete mp3;
    delete file;
    delete out;

    Serial.printf("[INIT] Pre-decoded %.1fs Elevator Music vào PSRAM (%zu samples)\n",
                  (float)waitingMusicTotalSamples / (float)currentSampleRate, waitingMusicTotalSamples);
}

void tone_driver_waiting_music_set(bool active) {
    if (active) {
        toneStopFlag = false;
        waitingMusicPlayIdx = 0;
    }
    waitingMusicActive = active;
}

bool tone_driver_waiting_music_is_active(void) {
    return waitingMusicActive;
}