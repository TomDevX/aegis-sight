#include <Arduino.h>
#include "config.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include <math.h>
#include "SPIFFS.h"
#include "libhelix-mp3/mp3dec.h"

#ifdef ENABLE_SPEAKER_TEST

// ============================================================
// Speaker Test — phát đoạn gemini_tts.mp3 qua Grove Speaker
// MP3 nằm trong SPIFFS (flash qua: pio run -t uploadfs)
// Decode bằng libhelix-mp3 -> PCM 16kHz -> PWM LEDC trên SIG
// Cùng đường phát giống TTS thật để nghe chất lượng giọng.
// Kích hoạt: uncomment ENABLE_SPEAKER_TEST trong config.h
// ============================================================

#define PWM_CHANNEL       2
#define PWM_RES_BITS      10
#define PWM_RES_MAX       ((1 << PWM_RES_BITS) - 1)
#define PWM_CARRIER_HZ    78125
#define PWM_DC_MID        (PWM_RES_MAX / 2)

#define SPK_SAMPLE_RATE   16000
#define MP3_FILENAME      "/gemini_tts.mp3"
#define MP3_MAX_SIZE      (256 * 1024)
#define PCM_MAX_SAMPLES   (SPK_SAMPLE_RATE * 10)

static short decodeBuf[1152 * 2];
static int16_t *g_pcm = NULL;
static uint32_t g_pcmLen = 0;

static bool spk_set_freq(uint32_t freq_hz) {
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.timer_num = (ledc_timer_t)LEDC_TIMER_1;
    t.duty_resolution = (ledc_timer_bit_t)PWM_RES_BITS;
    t.freq_hz = freq_hz;
    t.clk_cfg = LEDC_USE_APB_CLK;
    return ledc_timer_config(&t) == ESP_OK;
}

static size_t resample_linear(const int16_t *in, size_t inLen, int16_t *out,
                              size_t outCap, uint32_t inRate, uint32_t outRate) {
    if (inRate == outRate || inLen < 2) {
        size_t c = (inLen < outCap) ? inLen : outCap;
        memcpy(out, in, c * sizeof(int16_t));
        return c;
    }
    uint32_t step = ((uint32_t)inRate << 16) / outRate;
    uint32_t pos = 0;
    size_t o = 0;
    while (o < outCap) {
        uint32_t idx = pos >> 16;
        if (idx + 1 >= inLen) break;
        uint32_t frac = pos & 0xFFFF;
        int32_t s = (int32_t)in[idx] + (((int32_t)(in[idx + 1] - in[idx]) * (int32_t)frac) >> 16);
        out[o++] = (int16_t)s;
        pos += step;
    }
    return o;
}

static uint32_t decode_mp3(const uint8_t *mp3, size_t mp3Len, int16_t *out, size_t outCap) {
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) { Serial.println("[SPK_TEST] MP3InitDecoder failed"); return 0; }

    unsigned char *inbuf = (unsigned char *)mp3;
    int bytesLeft = (int)mp3Len;
    int sampleRate = 24000;
    size_t total = 0;

    while (bytesLeft > 0 && total < outCap) {
        int res = MP3Decode(dec, &inbuf, &bytesLeft, decodeBuf, 0);
        if (res == ERR_MP3_NONE) {
            MP3FrameInfo info;
            MP3GetLastFrameInfo(dec, &info);
            if (info.nChans > 0 && info.samprate > 0) sampleRate = info.samprate;
            int samples = info.outputSamps / info.nChans;

            int16_t mono[1152];
            if (info.nChans == 2) {
                for (int i = 0; i < samples; i++)
                    mono[i] = (int16_t)(((int32_t)decodeBuf[i * 2] + (int32_t)decodeBuf[i * 2 + 1]) >> 1);
            } else {
                memcpy(mono, decodeBuf, samples * sizeof(int16_t));
            }

            size_t playLen;
            if (sampleRate != SPK_SAMPLE_RATE) {
                playLen = resample_linear(mono, samples, out + total, outCap - total, sampleRate, SPK_SAMPLE_RATE);
            } else {
                size_t c = (samples < outCap - total) ? samples : (outCap - total);
                memcpy(out + total, mono, c * sizeof(int16_t));
                playLen = c;
            }
            total += playLen;
        } else if (res == ERR_MP3_INDATA_UNDERFLOW || res == ERR_MP3_MAINDATA_UNDERFLOW) {
            break;
        } else {
            int sync = MP3FindSyncWord(inbuf, bytesLeft);
            if (sync >= 0) { inbuf += sync; bytesLeft -= sync; continue; }
            break;
        }
    }
    MP3FreeDecoder(dec);
    return total;
}

static void pwm_play_pcm(const int16_t *pcm, uint32_t samples) {
    spk_set_freq(PWM_CARRIER_HZ);
    int64_t next = esp_timer_get_time();
    for (uint32_t i = 0; i < samples; i++) {
        int32_t d = PWM_DC_MID + ((int32_t)pcm[i] >> (16 - PWM_RES_BITS));
        if (d < 0) d = 0;
        else if (d > PWM_RES_MAX) d = PWM_RES_MAX;
        ledcWrite(PWM_CHANNEL, (uint32_t)d);
        next += 62;   // ~16kHz pacing
        while (esp_timer_get_time() < next) {}
    }
    ledcWrite(PWM_CHANNEL, 0);
}

// Sóng vuông tần số âm thanh (cách tone_driver làm bíp — đã nghe được)
static void beep(uint16_t freqHz, uint32_t durationMs) {
    spk_set_freq(freqHz);
    ledcWrite(PWM_CHANNEL, 512);
    vTaskDelay(pdMS_TO_TICKS(durationMs));
    spk_set_freq(PWM_CARRIER_HZ);
    ledcWrite(PWM_CHANNEL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n========================================");
    Serial.println("  SPEAKER TEST - Play gemini_tts.mp3");
    Serial.println("  SIG=GPIO40 | libhelix MP3 -> PWM");
    Serial.println("========================================\n");

    if (!spk_set_freq(PWM_CARRIER_HZ)) {
        Serial.println("[SPK_TEST] PWM timer config failed");
    }
    ledcAttachPin(SPK_PWM_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0);
    Serial.printf("[SPK_TEST] PWM speaker ready (SIG=GPIO%d)\n", SPK_PWM_PIN);

    if (!SPIFFS.begin(true)) {
        Serial.println("[SPK_TEST] SPIFFS mount failed");
        return;
    }

    File f = SPIFFS.open(MP3_FILENAME, "r");
    if (!f) {
        Serial.printf("[SPK_TEST] Missing %s (run: pio run -t uploadfs)\n", MP3_FILENAME);
        return;
    }
    size_t mp3Len = f.size();
    if (mp3Len > MP3_MAX_SIZE) mp3Len = MP3_MAX_SIZE;
    uint8_t *mp3 = (uint8_t *)ps_malloc(mp3Len + 1);
    if (!mp3) { Serial.println("[SPK_TEST] ps_malloc failed for MP3"); f.close(); return; }
    f.read(mp3, mp3Len);
    f.close();
    Serial.printf("[SPK_TEST] Loaded %s: %u bytes\n", MP3_FILENAME, (unsigned)mp3Len);

    g_pcm = (int16_t *)ps_malloc(PCM_MAX_SAMPLES * sizeof(int16_t));
    if (!g_pcm) { Serial.println("[SPK_TEST] ps_malloc failed for PCM"); free(mp3); return; }
    g_pcmLen = decode_mp3(mp3, mp3Len, g_pcm, PCM_MAX_SAMPLES);
    free(mp3);
    Serial.printf("[SPK_TEST] Decoded: %u samples (%.1f s @16kHz)\n",
                  g_pcmLen, (float)g_pcmLen / SPK_SAMPLE_RATE);

    int32_t peak = 0;
    double sum = 0;
    for (uint32_t i = 0; i < g_pcmLen; i++) {
        int32_t a = g_pcm[i] < 0 ? -g_pcm[i] : g_pcm[i];
        if (a > peak) peak = a;
        sum += (double)g_pcm[i] * g_pcm[i];
    }
    Serial.printf("[SPK_TEST] PCM peak=%d RMS=%.0f\n", peak,
                  g_pcmLen ? sqrt(sum / g_pcmLen) : 0.0);

    Serial.println("[SPK_TEST] Beep 1kHz 500ms (kiem chung speaker)...");
    beep(1000, 500);
    beep(2000, 300);
}

void loop() {
    if (g_pcmLen > 0) {
        Serial.printf("[SPK_TEST] Playing %u samples...\n", g_pcmLen);
        pwm_play_pcm(g_pcm, g_pcmLen);
        Serial.println("[SPK_TEST] Done. Restart to replay.");
        vTaskDelay(pdMS_TO_TICKS(3000));
    } else {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif // ENABLE_SPEAKER_TEST
