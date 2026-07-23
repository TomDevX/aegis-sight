#ifdef ENABLE_MIC_TEST

#include <Arduino.h>
#include "config.h"
#include "driver/i2s.h"

// ============================================================
// Microphone INMP441 Test - I2S RX 16kHz 16-bit Mono
// Hiển thị RMS, Peak amplitude liên tục qua Serial
// Kích hoạt: uncomment #define ENABLE_MIC_TEST trong config.h
//             và comment TẤT CẢ main flags để tránh conflict
// ============================================================

#define MIC_SAMPLE_RATE   16000
#define MIC_BITS_SAMPLE   16
#define MIC_CHANNELS      1
#define MIC_READ_BUF_SIZE 2048
#define MIC_REPORT_MS     500

static i2s_chan_handle_t micRxHandle = NULL;
static int16_t *micRxBuf = NULL;

static bool initMicI2S(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &micRxHandle);
    if (err != ESP_OK) {
        Serial.printf("[MIC_TEST] i2s_new_channel failed: 0x%x\n", err);
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
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
        Serial.printf("[MIC_TEST] i2s_channel_init_std_mode failed: 0x%x\n", err);
        return false;
    }

    err = i2s_channel_enable(micRxHandle);
    if (err != ESP_OK) {
        Serial.printf("[MIC_TEST] i2s_channel_enable failed: 0x%x\n", err);
        return false;
    }

    return true;
}

static void readAndAnalyze(void) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(micRxHandle, micRxBuf, MIC_READ_BUF_SIZE, &bytesRead, 0);
    if (err != ESP_OK || bytesRead == 0) return;

    int16_t *samples = micRxBuf;
    int numSamples = bytesRead / sizeof(int16_t);

    int32_t sumSq = 0;
    int16_t peak = 0;

    for (int i = 0; i < numSamples; i++) {
        int16_t s = samples[i];
        sumSq += (int32_t)s * s;
        if (abs(s) > peak) peak = s;
    }

    float rms = sqrtf((float)sumSq / numSamples);
    float rmsDb = 20.0f * log10f(rms / 32768.0f + 1e-10f);

    Serial.printf("[MIC_TEST] Samples: %d | Peak: %d | RMS: %.0f | RMS dB: %.1f dB\n",
                  numSamples, peak, rms, rmsDb);
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n========================================");
    Serial.println("  MICROPHONE TEST - INMP441 I2S RX");
    Serial.println("  16kHz 16-bit Mono | BCLK=41 LR=42");
    Serial.println("========================================\n");

    micRxBuf = (int16_t *)ps_malloc(MIC_READ_BUF_SIZE);
    if (!micRxBuf) {
        Serial.println("[MIC_TEST] FATAL: ps_malloc failed for RX buffer!");
        return;
    }
    Serial.printf("[MIC_TEST] RX buffer: %d bytes on PSRAM\n", MIC_READ_BUF_SIZE);

    if (!initMicI2S()) {
        Serial.println("[MIC_TEST] I2S init failed, halting.");
        return;
    }
    Serial.println("[MIC_TEST] I2S RX initialized OK");
    Serial.println("[MIC_TEST] Speak into mic or play sound...\n");
}

void loop() {
    static unsigned long lastReport = 0;
    if (millis() - lastReport >= MIC_REPORT_MS) {
        lastReport = millis();
        readAndAnalyze();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

#endif // ENABLE_MIC_TEST
