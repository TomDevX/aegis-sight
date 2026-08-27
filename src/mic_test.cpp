#include <Arduino.h>
#include "config.h"
#include "driver/i2s.h"
#include <math.h>

#ifdef ENABLE_MIC_TEST

// ============================================================
// COMBINED MIC TEST (INMP441 -> MAX98357A SPEAKER + SERIAL RMS)
// - Mic: INMP441 I2S RX (I2S_NUM_0: SD=2, SCK=41, WS=42)
// - Speaker: MAX98357A I2S TX (I2S_NUM_1: DIN=19, LRC=21, BCLK=20)
// ============================================================

#define MIC_SAMPLE_RATE    16000
#define CHUNK_SAMPLES      256
#define REPORT_INTERVAL_MS 100

static int32_t *micRxRaw = NULL;
static int16_t *spkTxBuf = NULL; // Stereo: CHUNK_SAMPLES * 2

static bool init_i2s_mic(void) {
    i2s_config_t i2s_mic_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // 32-bit clock chuẩn để INMP441 xuất đủ 24-bit
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t mic_pins = {
        .bck_io_num = MIC_BCLK,
        .ws_io_num = MIC_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_DATA_IN
    };

    if (i2s_driver_install(I2S_MIC_PORT, &i2s_mic_cfg, 0, NULL) != ESP_OK) {
        Serial.println("[MIC_TEST] Loi khoi tao I2S Mic RX!");
        return false;
    }
    if (i2s_set_pin(I2S_MIC_PORT, &mic_pins) != ESP_OK) {
        Serial.println("[MIC_TEST] Loi set pin I2S Mic RX!");
        return false;
    }
    return true;
}

static bool init_i2s_spk(void) {
    i2s_config_t i2s_spk_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t spk_pins = {
        .bck_io_num = AMP_I2S_BCLK,
        .ws_io_num = AMP_I2S_LRC,
        .data_out_num = AMP_I2S_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    if (i2s_driver_install(I2S_SPK_PORT, &i2s_spk_cfg, 0, NULL) != ESP_OK) {
        Serial.println("[MIC_TEST] Loi khoi tao I2S Speaker TX!");
        return false;
    }
    if (i2s_set_pin(I2S_SPK_PORT, &spk_pins) != ESP_OK) {
        Serial.println("[MIC_TEST] Loi set pin I2S Speaker TX!");
        return false;
    }
    return true;
}

static void print_visual_meter(float rms, int16_t peak) {
    // Thước đo 20 ký tự đại diện cho RMS từ 0 đến 10000
    const int BAR_LEN = 24;
    int fill = (int)((rms / 8000.0f) * BAR_LEN);
    if (fill > BAR_LEN) fill = BAR_LEN;
    if (fill < 0) fill = 0;

    char bar[BAR_LEN + 1];
    for (int i = 0; i < BAR_LEN; i++) {
        bar[i] = (i < fill) ? '=' : ' ';
    }
    bar[BAR_LEN] = '\0';

    const char *status = "YEN TINH";
    if (rms > 3500) status = "GIONG NOI TO / VỖ TAY";
    else if (rms > 800) status = "CO TIENG NOI";
    else if (rms > 300) status = "TIENG THI THAM / ON NHE";

    Serial.printf("[MIC_TEST] [%s] RMS: %5.0f | Peak: %5d | %s\n",
                  bar, rms, peak, status);
}

#define REC_SECONDS         3
#define REC_TOTAL_SAMPLES   (MIC_SAMPLE_RATE * REC_SECONDS)
static int16_t *playbackBuf = NULL;

static void play_beep(uint16_t freq, uint32_t durationMs) {
    uint32_t samples = (MIC_SAMPLE_RATE * durationMs) / 1000;
    float phaseInc = (2.0f * (float)M_PI * (float)freq) / (float)MIC_SAMPLE_RATE;
    float phase = 0.0f;
    int16_t bBuf[128][2];

    uint32_t generated = 0;
    while (generated < samples) {
        uint32_t n = samples - generated;
        if (n > 128) n = 128;
        for (uint32_t i = 0; i < n; i++) {
            int16_t val = (int16_t)(sinf(phase) * 15000.0f);
            bBuf[i][0] = val;
            bBuf[i][1] = val;
            phase += phaseInc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        size_t written = 0;
        i2s_write(I2S_SPK_PORT, bBuf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        generated += n;
    }
}

static void do_record_and_playback(void) {
    if (!playbackBuf) return;

    Serial.println("\n[MIC_TEST] >>> BẮT ĐẦU GHI ÂM (HÃY NÓI VÀO MIC TRONG 3 GIÂY) <<<");
    play_beep(1000, 60);
    delay(50);

    size_t samplesRecorded = 0;
    float dcOffset = 0.0f;
    int32_t *rawBuf = (int32_t *)ps_malloc(512 * sizeof(int32_t));
    if (!rawBuf) return;

    size_t dummy_bytes = 0;
    i2s_read(I2S_MIC_PORT, rawBuf, 512 * sizeof(int32_t), &dummy_bytes, 100);

    unsigned long start = millis();
    while (samplesRecorded < REC_TOTAL_SAMPLES && (millis() - start < (REC_SECONDS * 1000 + 500))) {
        size_t bytesRead = 0;
        esp_err_t res = i2s_read(I2S_MIC_PORT, rawBuf, 512 * sizeof(int32_t), &bytesRead, portMAX_DELAY);
        if (res == ESP_OK && bytesRead > 0) {
            int count = bytesRead / sizeof(int32_t);
            for (int i = 0; i < count; i++) {
                if (samplesRecorded >= REC_TOTAL_SAMPLES) break;

                // Trích xuất 16-bit chuẩn từ 32-bit slot
                int32_t s16 = rawBuf[i] >> 14;
                float sample = (float)s16;

                // 1. Lọc DC Offset
                dcOffset = 0.995f * dcOffset + 0.005f * sample;
                float clean = (sample - dcOffset) * 3.5f;

                // 2. Triệt tiêu hoàn toàn tiếng rè nền (Noise Gate 350 LSB)
                if (fabsf(clean) < 350.0f) {
                    clean = 0.0f;
                }

                if (clean > 32000.0f) clean = 32000.0f;
                else if (clean < -32000.0f) clean = -32000.0f;

                playbackBuf[samplesRecorded++] = (int16_t)clean;
            }
        }
    }

    free(rawBuf);

    Serial.printf("[MIC_TEST] >>> ĐÃ THU XONG %zu SAMPLES. BẮT ĐẦU PHÁT LẠI RA LOA... <<<\n", samplesRecorded);
    play_beep(1320, 60);
    delay(100);

    // Phát lại ra loa MAX98357A ở âm lượng vừa vặn, trung thực
    int16_t outChunk[256][2];
    size_t pos = 0;
    while (pos < samplesRecorded) {
        size_t chunk = samplesRecorded - pos;
        if (chunk > 256) chunk = 256;
        for (size_t i = 0; i < chunk; i++) {
            int16_t sample = playbackBuf[pos + i];
            outChunk[i][0] = sample;
            outChunk[i][1] = sample;
        }
        size_t written = 0;
        i2s_write(I2S_SPK_PORT, outChunk, chunk * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        pos += chunk;
    }

    Serial.println("[MIC_TEST] >>> PHÁT LẠI HOÀN TẤT. Tiếp tục đo âm lượng thời gian thực.\n");
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n==================================================");
    Serial.println("  AEGIS SIGHT — MIC TEST & VOICE PLAYBACK");
    Serial.println("  1. Quan sát thước đo RMS thời gian thực");
    Serial.println("  2. BẤM NÚT (GPIO14) -> Ghi âm 3s và PHÁT LẠI RA LOA");
    Serial.println("==================================================\n");

    pinMode(BTN_TRIGGER, INPUT_PULLUP);

    micRxRaw = (int32_t *)ps_malloc(CHUNK_SAMPLES * sizeof(int32_t));
    spkTxBuf = (int16_t *)ps_malloc(CHUNK_SAMPLES * 2 * sizeof(int16_t));
    playbackBuf = (int16_t *)ps_malloc(REC_TOTAL_SAMPLES * sizeof(int16_t));

    if (!micRxRaw || !spkTxBuf || !playbackBuf) {
        Serial.println("[MIC_TEST] FATAL: ps_malloc failed!");
        return;
    }

    if (!init_i2s_mic()) {
        Serial.println("[MIC_TEST] Init Mic failed!");
        return;
    }
    if (!init_i2s_spk()) {
        Serial.println("[MIC_TEST] Init Speaker failed!");
        return;
    }

    // Phát thử 1 đoạn nốt nhạc mẫu kiểm tra loa (Đồ - Mi - Sol - Đố)
    Serial.println("[MIC_TEST] Đang phát thử nốt nhạc mẫu để kiểm tra độ trong của loa MAX98357A...");
    play_beep(523, 150); // C5
    play_beep(659, 150); // E5
    play_beep(784, 150); // G5
    play_beep(1046, 300); // C6
    delay(200);

    Serial.println("[MIC_TEST] Sẵn sàng! Bấm nút GPIO14 để ghi âm và nghe lại.\n");
}

void loop() {
    static uint32_t lastReport = 0;
    static float accumSumSq = 0;
    static int accumSamples = 0;
    static int16_t maxPeak = 0;
    static float dcOffset = 0.0f;
    static int lastBtn = HIGH;

    // Kiểm tra bấm nút GPIO 14 để Ghi âm & Phát lại
    int btn = digitalRead(BTN_TRIGGER);
    if (btn == LOW && lastBtn == HIGH) {
        delay(50);
        do_record_and_playback();
        lastBtn = btn;
        return;
    }
    lastBtn = btn;

    size_t bytesRead = 0;
    esp_err_t res = i2s_read(I2S_MIC_PORT, micRxRaw, CHUNK_SAMPLES * sizeof(int32_t),
                             &bytesRead, pdMS_TO_TICKS(50));

    if (res == ESP_OK && bytesRead > 0) {
        int count = bytesRead / sizeof(int32_t);

        for (int i = 0; i < count; i++) {
            float s = (float)(micRxRaw[i] >> 14);

            dcOffset = 0.995f * dcOffset + 0.005f * s;
            float clean = (s - dcOffset) * 3.5f;

            if (fabsf(clean) < 350.0f) clean = 0.0f;

            if (clean > 32000.0f) clean = 32000.0f;
            else if (clean < -32000.0f) clean = -32000.0f;

            int16_t pcm16 = (int16_t)clean;

            accumSumSq += (float)pcm16 * (float)pcm16;
            accumSamples++;
            if (abs(pcm16) > maxPeak) maxPeak = abs(pcm16);
        }
    }

    if (millis() - lastReport >= REPORT_INTERVAL_MS) {
        float rms = (accumSamples > 0) ? sqrtf(accumSumSq / accumSamples) : 0;
        print_visual_meter(rms, maxPeak);

        accumSumSq = 0;
        accumSamples = 0;
        maxPeak = 0;
        lastReport = millis();
    }
}
    vTaskDelay(pdMS_TO_TICKS(1));
}

#endif // ENABLE_MIC_TEST
