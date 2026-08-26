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
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
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

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n==================================================");
    Serial.println("  AEGIS SIGHT — COMBINED MIC & SPEAKER TEST");
    Serial.println("  1. INMP441 Mic (I2S0) -> Visual RMS Meter");
    Serial.println("  2. MAX98357A Amp (I2S1) -> Real-time Speaker Echo");
    Serial.println("==================================================\n");

    micRxRaw = (int32_t *)ps_malloc(CHUNK_SAMPLES * sizeof(int32_t));
    spkTxBuf = (int16_t *)ps_malloc(CHUNK_SAMPLES * 2 * sizeof(int16_t));

    if (!micRxRaw || !spkTxBuf) {
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

    Serial.println("[MIC_TEST] San sang! Hay noi vao Mic de nghe giong tren loa va quan sat thanh do.\n");
}

void loop() {
    static uint32_t lastReport = 0;
    static float accumSumSq = 0;
    static int accumSamples = 0;
    static int16_t maxPeak = 0;
    static float dcOffset = 0.0f;

    size_t bytesRead = 0;
    esp_err_t res = i2s_read(I2S_MIC_PORT, micRxRaw, CHUNK_SAMPLES * sizeof(int32_t),
                             &bytesRead, pdMS_TO_TICKS(50));

    if (res == ESP_OK && bytesRead > 0) {
        int samplesRead = bytesRead / sizeof(int32_t);

        for (int i = 0; i < samplesRead; i++) {
            // 1. Trích xuất 24-bit MSB và đưa về 16-bit
            int32_t raw24 = micRxRaw[i] >> 8;
            float s = (float)raw24;

            // 2. Lọc DC Offset (High-pass 5Hz)
            dcOffset = 0.995f * dcOffset + 0.005f * s;
            float clean = s - dcOffset;

            int16_t pcm16 = (int16_t)clean;

            // Thống kê RMS & Peak
            accumSumSq += (float)pcm16 * (float)pcm16;
            accumSamples++;
            if (abs(pcm16) > maxPeak) maxPeak = abs(pcm16);

            // 3. Scale âm lượng ra loa vừa vặn cho Loa Seeed Grove qua MAX98357A (Gain an toàn)
            int32_t outSample = (int32_t)(clean * 0.35f);
            if (outSample > 32767) outSample = 32767;
            if (outSample < -32768) outSample = -32768;

            int16_t val16 = (int16_t)outSample;

            // Đẩy ra 2 kênh Stereo cho MAX98357A
            spkTxBuf[i * 2]     = val16; // Left
            spkTxBuf[i * 2 + 1] = val16; // Right
        }

        // Ghi sang Speaker MAX98357A theo thời gian thực (Zero-latency Loopback)
        size_t bytesWritten = 0;
        i2s_write(I2S_SPK_PORT, spkTxBuf, samplesRead * 2 * sizeof(int16_t),
                  &bytesWritten, pdMS_TO_TICKS(20));
    }

    uint32_t now = millis();
    if (now - lastReport >= REPORT_INTERVAL_MS) {
        if (accumSamples > 0) {
            float rms = sqrtf(accumSumSq / accumSamples);
            print_visual_meter(rms, maxPeak);
        }
        lastReport = now;
        accumSumSq = 0;
        accumSamples = 0;
        maxPeak = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
}

#endif // ENABLE_MIC_TEST
