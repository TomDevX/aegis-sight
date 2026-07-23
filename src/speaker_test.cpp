#ifdef ENABLE_SPEAKER_TEST

#include <Arduino.h>
#include "config.h"
#include "driver/i2s.h"
#include <math.h>

// ============================================================
// Speaker PCM5102A Test - I2S TX 16kHz 16-bit
// Phát sine tone 1kHz trong 2s, pause 1s, lặp lại
// Kích hoạt: uncomment #define ENABLE_SPEAKER_TEST trong config.h
//             và comment TẤT CẢ main flags để tránh conflict
// ============================================================

#define SPK_SAMPLE_RATE   16000
#define SPK_BITS_SAMPLE   16
#define SPK_TONE_HZ       1000
#define SPK_TONE_MS       2000
#define SPK_PAUSE_MS      1000
#define SPK_AMPLITUDE     16000

static i2s_chan_handle_t spkTxHandle = NULL;
static int16_t *spkToneBuf = NULL;

static bool initSpeakerI2S(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPK_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &spkTxHandle, NULL);
    if (err != ESP_OK) {
        Serial.printf("[SPK_TEST] i2s_new_channel failed: 0x%x\n", err);
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
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

    err = i2s_channel_init_std_mode(spkTxHandle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("[SPK_TEST] i2s_channel_init_std_mode failed: 0x%x\n", err);
        return false;
    }

    err = i2s_channel_enable(spkTxHandle);
    if (err != ESP_OK) {
        Serial.printf("[SPK_TEST] i2s_channel_enable failed: 0x%x\n", err);
        return false;
    }

    return true;
}

static void generateSineTone(int16_t *buf, uint32_t samples, uint16_t freq, uint16_t amplitude) {
    for (uint32_t i = 0; i < samples; i++) {
        float t = (float)i / SPK_SAMPLE_RATE;
        buf[i] = (int16_t)(amplitude * sinf(2.0f * M_PI * freq * t));
    }
}

static void playToneBurst(void) {
    uint32_t totalSamples = (uint32_t)SPK_SAMPLE_RATE * SPK_TONE_MS / 1000;
    size_t written = 0;

    Serial.printf("[SPK_TEST] Playing %dHz sine for %dms...\n", SPK_TONE_HZ, SPK_TONE_MS);

    uint32_t offset = 0;
    while (offset < totalSamples) {
        uint32_t chunk = totalSamples - offset;
        if (chunk > 1024) chunk = 1024;
        esp_err_t err = i2s_write(spkTxHandle, &spkToneBuf[offset],
                                  chunk * sizeof(int16_t), &written, 0);
        if (err != ESP_OK) break;
        offset += written / sizeof(int16_t);
    }

    Serial.printf("[SPK_TEST] Played %lu samples\n", offset);
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n========================================");
    Serial.println("  SPEAKER TEST - PCM5102A I2S TX");
    Serial.println("  1kHz Sine | 16kHz 16-bit | BCLK=1");
    Serial.println("========================================\n");

    uint32_t totalSamples = (uint32_t)SPK_SAMPLE_RATE * SPK_TONE_MS / 1000;
    spkToneBuf = (int16_t *)ps_malloc(totalSamples * sizeof(int16_t));
    if (!spkToneBuf) {
        Serial.println("[SPK_TEST] FATAL: ps_malloc failed for tone buffer!");
        return;
    }
    Serial.printf("[SPK_TEST] Tone buffer: %lu bytes on PSRAM\n",
                  totalSamples * sizeof(int16_t));

    generateSineTone(spkToneBuf, totalSamples, SPK_TONE_HZ, SPK_AMPLITUDE);
    Serial.println("[SPK_TEST] Sine wave generated");

    if (!initSpeakerI2S()) {
        Serial.println("[SPK_TEST] I2S TX init failed, halting.");
        return;
    }
    Serial.println("[SPK_TEST] I2S TX initialized OK\n");
}

void loop() {
    playToneBurst();
    vTaskDelay(pdMS_TO_TICKS(SPK_TONE_MS + SPK_PAUSE_MS));
}

#endif // ENABLE_SPEAKER_TEST
