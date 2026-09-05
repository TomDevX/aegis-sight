#include <Arduino.h>
#include "config.h"
#include "ai_pipeline.h"
#include "fall_detection.h"
#include "tone_driver.h"
#include "tts_driver.h"
#include "secrets.h"
#include "offline_sounds.h"
#include <ArduinoJson.h>

#ifdef ENABLE_AI_PIPELINE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include "img_converters.h"    // fmt2jpg: RGB565 -> JPEG software (GC2145 has no JPEG HW)
#include "driver/i2s.h"
#include "mbedtls/base64.h"

// ============================================================
// Shared state (khai báo sớm cho các helper phía dưới)
// ============================================================
static uint8_t *jpegBuf = NULL;      // JPEG sau khi nén bằng fmt2jpg
static size_t   jpegSize = 0;
static int16_t *recordBuf = NULL;    // PCM ghi âm từ mic (Hold-to-Talk)
static volatile size_t recordSamples = 0;
static volatile bool pipelineBusy = false;
static volatile bool dataReady    = false;

#define WAV_HEADER_SIZE       44
#define RECORD_MIN_MS         300     // tối thiểu 300ms mới coi là có câu hỏi

static bool alloc_buffers(void);

// ============================================================
// Text normalization
// ============================================================
static String normalize_text(const String &text) {
    String t = text;
    t.replace("\\times", " nhân ");
    t.replace("\\text{", ""); t.replace("}", "");
    t.replace("\\", ""); t.replace("$", "");
    t.replace("**", ""); t.replace("*", "");
    t.replace("10^9", " tỷ"); t.replace("10^6", " triệu");
    t.replace("^", " mũ ");
    t.replace("ESP32-S3", "E ét pi 32 ét ba");
    t.replace("ESP32", "E ét pi 32");
    t.replace("WiFi", "Wai fai");
    t.replace("I2S", "I hai S"); t.replace("I2C", "I hai C");
    t.replace("API", "A P I"); t.replace("AI", "A I");
    return t;
}

// ============================================================
// Queue câu thoại TTS bất đồng bộ (giải phóng mạng Gemini, không bị block)
// ============================================================
static QueueHandle_t ttsSentenceQueue = NULL;
static TaskHandle_t ttsWorkerTaskHandle = NULL;

static void queue_sentence_for_tts(const String &text) {
    if (!pipelineBusy || text.length() == 0) return;
    String norm = normalize_text(text);
    norm.trim();
    if (norm.length() == 0) return;

    char *buf = strdup(norm.c_str());
    if (buf) {
        if (xQueueSend(ttsSentenceQueue, &buf, pdMS_TO_TICKS(100)) != pdTRUE) {
            free(buf);
        }
    }
}

// Core 0 task: nhận câu từ queue và phát TTS tuần tự từng câu siêu tốc
static void tts_worker_task(void *pv) {
    while (true) {
        char *sentence = NULL;
        if (xQueueReceive(ttsSentenceQueue, &sentence, portMAX_DELAY) == pdTRUE) {
            if (sentence != NULL) {
                if (pipelineBusy && strlen(sentence) > 0) {
                    tts_driver_speak(sentence, strlen(sentence));
                }
                free(sentence);
            }
        }
    }
}

// ============================================================
// Sentence boundary: ngắt từng câu một (khi gặp dấu câu đầu tiên) để phát ra loa tức thì
// ============================================================
static bool is_sentence_end(const String &buf, size_t i) {
    char c = buf[i];
    if (c == '?' || c == '!' || c == '\n') return true;
    if (c == '.') {
        bool p = i > 0 && isdigit(buf[i-1]);
        bool n = i + 1 < buf.length() && isdigit(buf[i+1]);
        return !(p && n);
    }
    return false;
}

static String ttsSentenceBuf;

static void flush_sentences(void) {
    while (true) {
        int firstBoundary = -1;
        for (int i = 0; i < (int)ttsSentenceBuf.length(); i++) {
            if (is_sentence_end(ttsSentenceBuf, i)) {
                firstBoundary = i;
                break;
            }
        }
        if (firstBoundary >= 0) {
            String complete = ttsSentenceBuf.substring(0, firstBoundary + 1);
            complete.trim();
            if (complete.length() > 0) queue_sentence_for_tts(complete);
            ttsSentenceBuf = ttsSentenceBuf.substring(firstBoundary + 1);
        } else {
            break;
        }
    }
}

static void flush_remaining(void) {
    ttsSentenceBuf.trim();
    if (ttsSentenceBuf.length() > 0) queue_sentence_for_tts(ttsSentenceBuf);
    ttsSentenceBuf = "";
}

// ============================================================
// Buffers
// ============================================================
static bool alloc_buffers(void) {
    if (!jpegBuf) {
        jpegBuf = (uint8_t *)ps_malloc(AI_JPEG_BUF_SIZE);
        if (!jpegBuf) return false;
    }
    if (!recordBuf) {
        recordBuf = (int16_t *)ps_malloc(AI_AUDIO_MAX_SAMPLES * sizeof(int16_t));
        if (!recordBuf) return false;
    }
    return true;
}

// ============================================================
// WiFi
// ============================================================
struct wifi_cred { String ssid, pass; };
static wifi_cred cached_nets[3];
static bool creds_loaded = false;
static String cached_last_ssid;
static String cached_api_key;

// Chỉ đọc key có tồn tại trong NVS để tránh log lỗi NOT_FOUND rác
static String secrets_get_if(const char *key) {
    return secrets_has(key) ? secrets_get(key) : String("");
}

static void wifi_creds_refresh(void) {
    cached_nets[0].ssid = secrets_get_if(SK_WIFI_SSID);
    cached_nets[0].pass = secrets_get_if(SK_WIFI_PASS);
    cached_nets[1].ssid = secrets_get_if(SK_WIFI_SSID2);
    cached_nets[1].pass = secrets_get_if(SK_WIFI_PASS2);
    cached_nets[2].ssid = secrets_get_if(SK_WIFI_SSID3);
    cached_nets[2].pass = secrets_get_if(SK_WIFI_PASS3);
    cached_last_ssid = secrets_get_if(SK_LAST_SSID);
    cached_api_key = secrets_get_if(SK_GEMINI_KEY);
    creds_loaded = true;
}

static bool try_ssid(const char *ssid, const char *pass, int tries) {
    if (!ssid || !*ssid) return false;
    Serial.printf("[WIFI] Đang thử \"%s\"...\n", ssid);
    WiFi.begin(ssid, pass);
    int c = 0;
    while (WiFi.status() != WL_CONNECTED && c < tries) {
        vTaskDelay(pdMS_TO_TICKS(250)); c++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[%6.2fs][WIFI] OK! IP: %s\n", millis() / 1000.0f, WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.printf("[%6.2fs][WIFI] Thất bại\n", millis() / 1000.0f);
    return false;
}

static bool ensure_wifi(void) {
    if (WiFi.status() == WL_CONNECTED) return true;
    if (!creds_loaded) wifi_creds_refresh();
    if (cached_nets[0].ssid.length() == 0) {
        Serial.println("[WIFI] KHÔNG CÓ SSID đã lưu! Giữ nút 5s khi boot để vào portal.");
        return false;
    }
    WiFi.mode(WIFI_STA);
    bool ok = false;
    if (cached_last_ssid.length()) {
        for (int i = 0; i < 3 && !ok; i++)
            if (cached_nets[i].ssid == cached_last_ssid)
                ok = try_ssid(cached_last_ssid.c_str(), cached_nets[i].pass.c_str(), 8);
    }
    for (int i = 0; i < 3 && !ok; i++) {
        if (cached_nets[i].ssid.length() == 0) continue;
        ok = try_ssid(cached_nets[i].ssid.c_str(), cached_nets[i].pass.c_str(), 24);
    }
    if (!ok) { WiFi.disconnect(true); Serial.println("[WIFI] Tất cả mạng đều thất bại!"); return false; }
    cached_last_ssid = WiFi.SSID();
    secrets_set(SK_LAST_SSID, WiFi.SSID());
    return true;
}

static void wifi_sleep(void) { WiFi.setSleep(true); }

// ============================================================
// WAV header 44 bytes (giống mic_ai_cam_test)
// ============================================================
static void create_wav_header(uint8_t *header, uint32_t pcmDataLen,
                              uint32_t sampleRate, uint16_t numChannels,
                              uint16_t bitsPerSample) {
    uint32_t totalDataLen = pcmDataLen + 36;
    uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    uint16_t blockAlign = numChannels * (bitsPerSample / 8);

    memcpy(header, "RIFF", 4);
    header[4] = totalDataLen & 0xff;
    header[5] = (totalDataLen >> 8) & 0xff;
    header[6] = (totalDataLen >> 16) & 0xff;
    header[7] = (totalDataLen >> 24) & 0xff;
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0;                    // PCM
    header[22] = (uint8_t)numChannels; header[23] = 0;
    header[24] = sampleRate & 0xff;
    header[25] = (sampleRate >> 8) & 0xff;
    header[26] = (sampleRate >> 16) & 0xff;
    header[27] = (sampleRate >> 24) & 0xff;
    header[28] = byteRate & 0xff;
    header[29] = (byteRate >> 8) & 0xff;
    header[30] = (byteRate >> 16) & 0xff;
    header[31] = (byteRate >> 24) & 0xff;
    header[32] = (uint8_t)blockAlign; header[33] = 0;
    header[34] = (uint8_t)bitsPerSample; header[35] = 0;
    memcpy(header + 36, "data", 4);
    header[40] = pcmDataLen & 0xff;
    header[41] = (pcmDataLen >> 8) & 0xff;
    header[42] = (pcmDataLen >> 16) & 0xff;
    header[43] = (pcmDataLen >> 24) & 0xff;
}

// ============================================================
// Base64 encode (mbedtls) — giống mic_ai_cam_test
// ============================================================
static bool base64_encode(const uint8_t *data, size_t length,
                          uint8_t **outBuf, size_t *outLen) {
    if (!data || length == 0) return false;

    size_t expected_len = ((length + 2) / 3) * 4 + 1;
    uint8_t *buf = (uint8_t *)ps_malloc(expected_len);
    if (!buf) return false;

    size_t olen = 0;
    if (mbedtls_base64_encode(buf, expected_len, &olen, data, length) != 0) {
        free(buf);
        return false;
    }
    buf[olen] = '\0';
    *outBuf = buf;
    *outLen = olen;
    return true;
}

// ============================================================
// Mic I2S cho ghi âm Hold-to-Talk (giống initMicI2S trong test:
// 32-bit ONLY_LEFT @16kHz — chuẩn INMP441)
// ============================================================
static bool rec_mic_install(void) {
    static bool installed = false;
    if (installed) return true;

    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AI_AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_cfg = {
        .bck_io_num = MIC_BCLK,
        .ws_io_num = MIC_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_DATA_IN,
    };

    if (i2s_driver_install(I2S_MIC_PORT, &i2s_cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_MIC_PORT, &pin_cfg) != ESP_OK) {
        i2s_driver_uninstall(I2S_MIC_PORT);
        return false;
    }
    installed = true;
    return true;
}

#if ENABLE_DEEPGRAM_STREAMING
static WiFiClientSecure deepgramWsClient;
static bool deepgramWsConnected = false;
static String liveStreamedText = "";

static bool deepgram_ws_connect(void) {
    if (strlen(DEEPGRAM_API_KEY) == 0) return false;
    deepgramWsClient.setInsecure();
    deepgramWsClient.setHandshakeTimeout(3);
    deepgramWsClient.setTimeout(4000);
    if (!deepgramWsClient.connect(DEEPGRAM_API_HOST, DEEPGRAM_API_PORT)) {
        return false;
    }

    String path = String("/v1/listen?model=") + DEEPGRAM_MODEL + "&language=vi&encoding=linear16&sample_rate=16000&channels=1&smart_format=true&punctuate=true&numerals=true&endpointing=20&utterance_end_ms=1000&vad_events=true";
    
    deepgramWsClient.printf("GET %s HTTP/1.1\r\n", path.c_str());
    deepgramWsClient.printf("Host: %s\r\n", DEEPGRAM_API_HOST);
    deepgramWsClient.printf("Upgrade: websocket\r\n");
    deepgramWsClient.printf("Connection: Upgrade\r\n");
    deepgramWsClient.printf("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
    deepgramWsClient.printf("Sec-WebSocket-Version: 13\r\n");
    deepgramWsClient.printf("Authorization: Token %s\r\n\r\n", DEEPGRAM_API_KEY);

    unsigned long start = millis();
    char lineBuf[256];
    bool upgraded = false;
    while ((millis() - start < 3000) && deepgramWsClient.connected()) {
        if (deepgramWsClient.available()) {
            size_t n = deepgramWsClient.readBytesUntil('\n', (uint8_t *)lineBuf, sizeof(lineBuf)-1);
            lineBuf[n] = '\0';
            if (strstr(lineBuf, "101 Switching Protocols") || strstr(lineBuf, "101 ")) {
                upgraded = true;
            }
            if (n <= 2 && upgraded) break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    deepgramWsConnected = upgraded;
    liveStreamedText = "";
    if (upgraded) {
        Serial.printf("[%6.2fs][DEEPGRAM] WebSocket Live Streaming Connected!\n", millis() / 1000.0f);
    }
    return upgraded;
}

static bool deepgram_ws_send_pcm(const int16_t *pcm, size_t count) {
    if (!deepgramWsConnected || count == 0) return false;
    size_t len = count * sizeof(int16_t);
    uint8_t header[8];
    size_t hLen = 0;
    header[0] = 0x82; // FIN + Binary Frame
    if (len < 126) {
        header[1] = 0x80 | (uint8_t)len;
        hLen = 2;
    } else {
        header[1] = 0x80 | 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        hLen = 4;
    }
    header[hLen++] = 0x12; header[hLen++] = 0x34;
    header[hLen++] = 0x56; header[hLen++] = 0x78;

    deepgramWsClient.write(header, hLen);

    uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    const uint8_t *rawBytes = (const uint8_t *)pcm;
    uint8_t maskedBuf[1024];
    for (size_t i = 0; i < len; i++) {
        maskedBuf[i] = rawBytes[i] ^ mask[i % 4];
    }
    deepgramWsClient.write(maskedBuf, len);
    return true;
}

static String deepgram_ws_finish_and_get_text(void) {
    if (!deepgramWsConnected) return "";
    const char *closeMsg = "{\"type\":\"CloseStream\"}";
    size_t cLen = strlen(closeMsg);
    uint8_t closeHeader[6];
    closeHeader[0] = 0x81;
    closeHeader[1] = 0x80 | (uint8_t)cLen;
    closeHeader[2] = 0x00; closeHeader[3] = 0x00; closeHeader[4] = 0x00; closeHeader[5] = 0x00;
    deepgramWsClient.write(closeHeader, 6);
    deepgramWsClient.write((const uint8_t *)closeMsg, cLen);
    deepgramWsClient.flush();

    String fullTranscript = "";
    unsigned long startWait = millis();
    while (deepgramWsClient.connected() && (millis() - startWait < 2000)) {
        if (deepgramWsClient.available() >= 2) {
            uint8_t b1 = deepgramWsClient.read();
            uint8_t b2 = deepgramWsClient.read();
            size_t pLen = b2 & 0x7F;
            if (pLen == 126) {
                while (deepgramWsClient.available() < 2 && millis() - startWait < 2000) vTaskDelay(1);
                pLen = (deepgramWsClient.read() << 8) | deepgramWsClient.read();
            }
            if (pLen > 0) {
                String jsonPayload = "";
                while (pLen > 0 && deepgramWsClient.available()) {
                    jsonPayload += (char)deepgramWsClient.read();
                    pLen--;
                }
                char *txtKey = strstr(jsonPayload.c_str(), "\"transcript\":\"");
                if (txtKey) {
                    char *valStart = txtKey + 14;
                    char *p = valStart;
                    String piece = "";
                    while (*p && *p != '\"') {
                        if (*p == '\\' && *(p+1)) {
                            p++;
                            if (*p == 'n') piece += '\n';
                            else piece += *p;
                        } else piece += *p;
                        p++;
                    }
                    piece.trim();
                    if (piece.length() > 0) {
                        if (fullTranscript.length() > 0) fullTranscript += " ";
                        fullTranscript += piece;
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    deepgramWsClient.stop();
    deepgramWsConnected = false;
    fullTranscript.trim();
    return fullTranscript;
}
#endif

// ============================================================
// Core 1: Ghi âm liên tục từ mic vào recordBuf cho đến khi thả nút
// ============================================================
static size_t record_until_release(void) {
    if (!recordBuf) return 0;

    vTaskDelay(pdMS_TO_TICKS(10));

    rec_mic_install();
    i2s_zero_dma_buffer(I2S_MIC_PORT);

#if ENABLE_DEEPGRAM_STREAMING
    deepgram_ws_connect();
#endif

    int32_t i2sBuffer[512];
    size_t samples = 0;
    float dc_offset = 0.0f;
    double sumSq = 0.0;
    int16_t maxPeak = 0;

    size_t dummy_bytes = 0;
    i2s_read(I2S_MIC_PORT, i2sBuffer, sizeof(i2sBuffer), &dummy_bytes, 100);

    unsigned long start = millis();
    bool released = false;
    unsigned long releaseTime = 0;
    int highStableCount = 0;

    while ((millis() - start < AI_AUDIO_MAX_RECORD_MS) &&
           (samples < AI_AUDIO_MAX_SAMPLES)) {
        
        // Chống nhiễu rung phím tuyệt đối: Chỉ coi là nhả nút khi tín hiệu HIGH duy trì ổn định
        if (digitalRead(BTN_TRIGGER) == HIGH) {
            highStableCount++;
            if (!released && highStableCount >= 3) {
                released = true;
                releaseTime = millis();
            }
        } else {
            // Người dùng vẫn đang đè nút -> Reset ngay trạng thái nhả nút!
            highStableCount = 0;
            released = false;
        }

        if (released && (millis() - releaseTime >= 250)) {
            break;
        }

        size_t bytes_read = 0;
        esp_err_t res = i2s_read(I2S_MIC_PORT, i2sBuffer, sizeof(i2sBuffer),
                                 &bytes_read, portMAX_DELAY);
        if (res != ESP_OK || bytes_read == 0) continue;

        int samplesCount = bytes_read / sizeof(int32_t);
        size_t chunkStart = samples;

        for (int i = 0; i < samplesCount; i++) {
            if (samples >= AI_AUDIO_MAX_SAMPLES) break;

            // Trích xuất chuẩn xác từ 32-bit I2S slot của INMP441 (giống mic_test.cpp)
            int32_t s16 = i2sBuffer[i] >> 14;
            float sample = (float)s16;

            // 1. Lọc DC Offset
            dc_offset = 0.995f * dc_offset + 0.005f * sample;
            float clean_sample = (sample - dc_offset) * 2.5f;

            // 2. Triệt tiêu hoàn toàn tạp âm xì xào nền (Noise Gate 300 LSB)
            if (fabsf(clean_sample) < 300.0f) {
                clean_sample = 0.0f;
            }

            if (clean_sample > 30000.0f) clean_sample = 30000.0f;
            else if (clean_sample < -30000.0f) clean_sample = -30000.0f;

            int16_t sample_16bit = (int16_t)clean_sample;
            recordBuf[samples++] = sample_16bit;

            sumSq += (double)sample_16bit * (double)sample_16bit;
            if (abs(sample_16bit) > maxPeak) maxPeak = abs(sample_16bit);
        }

#if ENABLE_DEEPGRAM_STREAMING
        if (samples > chunkStart) {
            deepgram_ws_send_pcm(recordBuf + chunkStart, samples - chunkStart);
        }
#endif
    }

#if ENABLE_DEEPGRAM_STREAMING
    liveStreamedText = deepgram_ws_finish_and_get_text();
#endif

    unsigned long elapsedMs = millis() - start;
    float rms = (samples > 0) ? sqrtf(sumSq / samples) : 0.0f;
    Serial.printf("[AI] Ghi âm xong: %zu samples (%lu ms) | RMS=%.0f | Peak=%d\n",
                  samples, elapsedMs, rms, maxPeak);

    // Padding tối thiểu 250ms cho file WAV
    if (samples < 4000) {
        while (samples < 4000) {
            recordBuf[samples++] = 0;
        }
    }
    return samples;
}

// ============================================================
// Xoay ảnh RGB565 theo góc bất kỳ (Bù góc nghiêng camera gọng kính)
// ============================================================
static uint16_t *rotatedBuf = NULL;

static void rotate_rgb565(const uint16_t *src, uint16_t srcW, uint16_t srcH,
                          uint16_t *dst, uint16_t dstW, uint16_t dstH,
                          float angleDeg, float scale) {
    if (!src || !dst || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return;
    if (scale <= 0.05f) scale = 1.0f;

    float rad = angleDeg * (float)M_PI / 180.0f;
    float cosA = cosf(rad);
    float sinA = sinf(rad);

    float cx_s = (float)(srcW - 1) * 0.5f;
    float cy_s = (float)(srcH - 1) * 0.5f;
    float cx_d = (float)(dstW - 1) * 0.5f;
    float cy_d = (float)(dstH - 1) * 0.5f;

    // Fixed-point 16.16: Tối ưu hoá vi xử lý ESP32-S3, chạy cực nhanh < 2ms
    float invScale = 1.0f / scale;
    int32_t step_xx = (int32_t)((cosA * invScale) * 65536.0f);
    int32_t step_xy = (int32_t)((-sinA * invScale) * 65536.0f);
    int32_t step_yx = (int32_t)((sinA * invScale) * 65536.0f);
    int32_t step_yy = (int32_t)((cosA * invScale) * 65536.0f);

    float row_start_x_f = cx_s + invScale * (-cx_d * cosA - cy_d * sinA);
    float row_start_y_f = cy_s + invScale * (cx_d * sinA - cy_d * cosA);

    int32_t row_start_x = (int32_t)(row_start_x_f * 65536.0f);
    int32_t row_start_y = (int32_t)(row_start_y_f * 65536.0f);

    size_t dst_idx = 0;
    for (uint16_t yd = 0; yd < dstH; yd++) {
        int32_t xs_fp = row_start_x;
        int32_t ys_fp = row_start_y;
        for (uint16_t xd = 0; xd < dstW; xd++) {
            int32_t xs = xs_fp >> 16;
            int32_t ys = ys_fp >> 16;
            if (xs >= 0 && xs < srcW && ys >= 0 && ys < srcH) {
                dst[dst_idx] = src[ys * srcW + xs];
            } else {
                dst[dst_idx] = 0x0000; // Đen viền nếu nằm ngoài khung ảnh gốc
            }
            dst_idx++;
            xs_fp += step_xx;
            ys_fp += step_xy;
        }
        row_start_x += step_yx;
        row_start_y += step_yy;
    }
}

// ============================================================
// Chụp ảnh RGB565 -> nén JPEG bằng fmt2jpg (giống captureJpeg test)
// ============================================================
static bool capture_jpeg(void) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[AI] Lỗi esp_camera_fb_get!");
        return false;
    }

    uint8_t *outJpg = NULL;
    size_t outLen = 0;
    pixformat_t srcFmt = (pixformat_t)fb->format;
    bool ok = false;

    uint16_t srcW = fb->width, srcH = fb->height;
    uint8_t *encodeBuf = fb->buf;
    size_t encodeLen = fb->len;
    uint16_t encodeW = srcW, encodeH = srcH;

#ifdef CAM_ROTATE_DEGREES
    if (fabsf((float)CAM_ROTATE_DEGREES) > 0.001f && srcFmt == PIXFORMAT_RGB565) {
        float normDeg = fmodf(fmodf((float)CAM_ROTATE_DEGREES, 360.0f) + 360.0f, 360.0f);
        bool isPortrait = (normDeg >= 45.0f && normDeg <= 135.0f) || (normDeg >= 225.0f && normDeg <= 315.0f);
        uint16_t dstW = isPortrait ? srcH : srcW;
        uint16_t dstH = isPortrait ? srcW : srcH;

        if (!rotatedBuf) {
            rotatedBuf = (uint16_t *)ps_malloc(320 * 320 * sizeof(uint16_t));
        }

        if (rotatedBuf) {
            rotate_rgb565((const uint16_t *)fb->buf, srcW, srcH,
                          rotatedBuf, dstW, dstH,
                          (float)CAM_ROTATE_DEGREES, (float)CAM_ROTATE_SCALE);
            encodeBuf = (uint8_t *)rotatedBuf;
            encodeLen = (size_t)dstW * dstH * sizeof(uint16_t);
            encodeW = dstW;
            encodeH = dstH;
        }
    }
#endif

    if (srcFmt == PIXFORMAT_RGB565 || srcFmt == PIXFORMAT_GRAYSCALE) {
        ok = fmt2jpg(encodeBuf, encodeLen, encodeW, encodeH, srcFmt,
                     AI_JPEG_QUALITY, &outJpg, &outLen);
    } else {
        // Fallback: sensor trả JPEG trực tiếp
        if (fb->len <= AI_JPEG_BUF_SIZE) {
            memcpy(jpegBuf, fb->buf, fb->len);
            jpegSize = fb->len;
            ok = true;
        }
    }

    esp_camera_fb_return(fb);

    if (ok && outJpg && outLen > 0 && outLen <= AI_JPEG_BUF_SIZE && jpegSize == 0) {
        memcpy(jpegBuf, outJpg, outLen);
        jpegSize = outLen;
        free(outJpg);
    } else if (outJpg) {
        free(outJpg);
        ok = false;
    }

    if (ok && jpegSize > 0) {
#ifdef CAM_ROTATE_DEGREES
        Serial.printf("[AI] Đã chụp: %ux%u (Xoay bù %.0f° -> %ux%u) -> JPEG %u bytes\n",
                      srcW, srcH, (float)CAM_ROTATE_DEGREES, encodeW, encodeH, (unsigned)jpegSize);
#else
        Serial.printf("[AI] Đã chụp: %ux%u -> JPEG %u bytes\n", encodeW, encodeH, (unsigned)jpegSize);
#endif
        return true;
    }
    Serial.println("[AI] Lỗi nén JPEG (fmt2jpg)!");
    return false;
}

// Quản lý Trạng Thái Hội Thoại (Stateful Interactions API) của Google Gemini
static String lastInteractionId = "";
static unsigned long lastInteractionMs = 0;

// Quản lý Trạng Thái Hội Thoại Đa Lượt (Multi-Turn Context) cho Groq Vision
static String lastGroqUserPrompt = "";
static String lastGroqAiReply = "";
static uint8_t *lastSavedImgB64 = NULL;
static size_t lastSavedImgB64Len = 0;

static void clear_session_memory(void) {
    lastInteractionId = "";
    lastGroqUserPrompt = "";
    lastGroqAiReply = "";
    if (lastSavedImgB64) {
        free(lastSavedImgB64);
        lastSavedImgB64 = NULL;
    }
    lastSavedImgB64Len = 0;
}

// Cờ phân biệt: Bấm 1 lần = Chủ đề mới hoàn toàn, Bấm đúp 2 lần = Nối tiếp hội thoại cũ
static bool isFollowUpSession = false;

// ============================================================
// Core 1: Nút nhấn Hold-to-Talk + Chụp ảnh
// Bấm 1 lần giữ     -> Tiếng "Tít" đơn   -> Chủ đề mới tinh (Xóa nhớ cũ)
// Bấm đúp 2 lần giữ -> Tiếng "Tít-Tít"   -> Nối tiếp hội thoại (Soi lại ảnh cũ)
// ============================================================
static void ai_audio_task(void *pv) {
    pinMode(BTN_TRIGGER, INPUT_PULLUP);
    alloc_buffers(); // Cấp phát PSRAM ngay khi khởi động để lần bấm đầu tiên luôn sẵn sàng 100%!
    int lastBtn = HIGH;
    uint32_t debounceMs = 0;
    uint32_t lastBtnReleaseMs = 0;
    while (true) {
        int btn = digitalRead(BTN_TRIGGER);
        uint32_t now = millis();
        if (btn == LOW && lastBtn == HIGH && (now - debounceMs) > 50) {
            debounceMs = now;
            #ifdef ENABLE_MPU6050_FALL_DETECTION
            if (fall_alarm_busy()) {
                fall_alarm_dismiss();
                lastBtn = btn;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (fall_alarm_was_cancelled_recently()) {
                lastBtn = btn;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            #endif
            if (!pipelineBusy) {
                // Khử rung dội phím ban đầu
                vTaskDelay(pdMS_TO_TICKS(15));
                if (digitalRead(BTN_TRIGGER) != LOW) {
                    lastBtn = HIGH;
                    continue;
                }

                if (!alloc_buffers()) continue;

                uint32_t pressStart = millis();
                bool isHold = false;
                
                // Đợi tối đa 250ms xem người dùng đè giữ hay nhấp nhả
                while (digitalRead(BTN_TRIGGER) == LOW) {
                    if (millis() - pressStart >= 250) {
                        isHold = true;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }

                if (isHold) {
                    // 1. BẤM GIỮ ĐƠN (HOLD TO TALK) -> HỎI CÂU HỎI MỚI (CHỦ ĐỀ MỚI)
                    isFollowUpSession = false;
                    clear_session_memory();
                    Serial.printf("[%6.2fs][BTN] >>> BẤM GIỮ ĐƠN: HỎI CÂU HỎI MỚI <<< \n", millis() / 1000.0f);
                    tone_driver_play_quick_beep(); // 🎵 Tiếng "Tít" đơn
                    recordSamples = record_until_release();
                } else {
                    // Người dùng vừa nhấp nhả nhanh (< 250ms)
                    // Đợi tối đa 300ms xem có cái nhấn thứ 2 không (Double-Click)
                    uint32_t waitSecond = millis();
                    bool hasSecondPress = false;
                    while (millis() - waitSecond < 300) {
                        if (digitalRead(BTN_TRIGGER) == LOW) {
                            vTaskDelay(pdMS_TO_TICKS(15)); // Chống rung phím lần 2
                            if (digitalRead(BTN_TRIGGER) == LOW) {
                                hasSecondPress = true;
                                break;
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(5));
                    }

                    if (hasSecondPress) {
                        // 2. BẤM ĐÚP (DOUBLE-CLICK) -> NỐI TIẾP HỘI THOẠI CŨ (GIỮ NGUYÊN TRÍ NHỚ)
                        isFollowUpSession = true;
                        Serial.printf("[%6.2fs][BTN] >>> BẤM ĐÚP (DOUBLE-CLICK): NỐI TIẾP HỘI THOẠI (GIỮ TRÍ NHỚ) <<< \n", millis() / 1000.0f);
                        tone_driver_play_double_beep(); // 🎵 Tiếng "Tít-Tít" đôi
                        recordSamples = record_until_release();
                    } else {
                        // 3. NHẤP NHANH 1 CÁI (SINGLE CLICK) -> CHỤP ẢNH MÔ TẢ NGAY (KHÔNG GHI ÂM)
                        isFollowUpSession = false;
                        clear_session_memory();
                        Serial.printf("[%6.2fs][BTN] >>> NHẤP NHANH 1 CÁI: CHỤP ẢNH MÔ TẢ NGAY <<< \n", millis() / 1000.0f);
                        tone_driver_play_quick_beep();
                        recordSamples = 0;
                    }
                }

                // Tăng tốc CPU lên 240MHz để xử lý ảnh, nén JPEG và AI siêu tốc
                setCpuFrequencyMhz(240);

                tts_driver_stop();

                pipelineBusy = true;
                dataReady = false;
                jpegSize = 0;

                // Chụp ảnh ngay lập tức
                if (!capture_jpeg()) {
                    pipelineBusy = false;
                    continue;
                }

                dataReady = true;
                // Bật nhạc chờ Elevator Music từ PSRAM (Zero-CPU) trong lúc gửi và chờ AI xử lý
                tone_driver_waiting_music_set(true);
                Serial.printf("[%6.2fs][AI] Đã chụp ảnh (%zu bytes) + %zu samples audio -> Gửi lên Gemini [%s]...\n",
                              millis() / 1000.0f, jpegSize, recordSamples, isFollowUpSession ? "Nối tiếp" : "Mới");
            } else {
                // Bấm trong lúc đang chạy -> hủy
                tone_driver_waiting_music_set(false);
                tts_driver_stop();
                tone_driver_stream_set_active(false);
                pipelineBusy = false; dataReady = false;
            }
        }
        lastBtn = btn;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================
// Stream dữ liệu lớn lên socket SSL siêu tốc (4KB chunks)
// ============================================================
static bool stream_to_client(WiFiClientSecure &client, const uint8_t *data, size_t len) {
    if (!data || len == 0) return false;

    size_t offset = 0;
    while (offset < len) {
        if (!pipelineBusy || !client.connected()) return false;
        size_t chunk = (len - offset > 4096) ? 4096 : (len - offset);
        size_t written = client.write(data + offset, chunk);
        if (written == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            written = client.write(data + offset, chunk);
            if (written == 0) {
                Serial.printf("[%6.2fs][NET] Lỗi ghi socket tại offset %zu/%zu\n", millis() / 1000.0f, offset, len);
                return false;
            }
        }
        offset += written;
    }
    return true;
}

static WiFiClientSecure groqSttClient;

static void pre_connect_groq(void) {
    if (!groqSttClient.connected()) {
        groqSttClient.setInsecure();
        groqSttClient.setHandshakeTimeout(4);
        groqSttClient.setTimeout(5000);
        groqSttClient.connect(GROQ_API_HOST, GROQ_API_PORT);
    }
}

// ============================================================
// Core 0: Gửi Audio WAV lên Groq LPU Whisper STT (~250ms)
// ============================================================
static String groq_whisper_transcribe(const uint8_t *wavBuf, size_t wavSize, bool &isNetworkError) {
    isNetworkError = false;
    if (wavBuf == NULL || wavSize <= WAV_HEADER_SIZE) return "";

    if (!groqSttClient.connected()) {
        Serial.printf("[%6.2fs][GROQ] Đang kết nối tới Groq LPU (Whisper STT)...\n", millis() / 1000.0f);
        groqSttClient.setInsecure();
        groqSttClient.setHandshakeTimeout(4);
        groqSttClient.setTimeout(5000);
        if (!groqSttClient.connect(GROQ_API_HOST, GROQ_API_PORT)) {
            Serial.printf("[%6.2fs][GROQ] Lỗi kết nối tới %s!\n", millis() / 1000.0f, GROQ_API_HOST);
            isNetworkError = true;
            return "";
        }
    } else {
        Serial.printf("[%6.2fs][GROQ] Dùng kênh SSL Pre-connected sẵn sàng (0ms chờ kết nối)!\n", millis() / 1000.0f);
    }

    WiFiClientSecure &client = groqSttClient;
    if (client.connected()) {
        client.setNoDelay(true);
    }

    static const char *kBoundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";

    String partModel = String("--") + kBoundary + "\r\n"
                     + "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                     + GROQ_MODEL + "\r\n";

    String partLang = String("--") + kBoundary + "\r\n"
                    + "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
                    + "vi\r\n";

    String partFormat = String("--") + kBoundary + "\r\n"
                      + "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
                      + "json\r\n";

    String partTemp = String("--") + kBoundary + "\r\n"
                    + "Content-Disposition: form-data; name=\"temperature\"\r\n\r\n"
                    + "0.0\r\n";

    String partFileHeader = String("--") + kBoundary + "\r\n"
                          + "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                          + "Content-Type: audio/wav\r\n\r\n";

    String partFooter = String("\r\n--") + kBoundary + "--\r\n";

    size_t totalBodyLen = partModel.length() + partLang.length() + partFormat.length()
                        + partTemp.length() + partFileHeader.length() + wavSize + partFooter.length();

    // Gửi HTTP POST Headers
    client.printf("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n");
    client.printf("Host: %s\r\n", GROQ_API_HOST);
    client.printf("Authorization: Bearer %s\r\n", GROQ_API_KEY);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", kBoundary);
    client.printf("Content-Length: %zu\r\n", totalBodyLen);
    client.printf("Connection: close\r\n\r\n");

    // Gửi Multipart Body
    client.print(partModel);
    client.print(partLang);
    client.print(partFormat);
    client.print(partTemp);
    client.print(partFileHeader);
    
    // Stream file WAV nhị phân siêu tốc (chunk 2KB tối ưu cho TLS buffer của ESP32)
    size_t offset = 0;
    while (offset < wavSize && client.connected() && pipelineBusy) {
        size_t toWrite = (wavSize - offset > 2048) ? 2048 : (wavSize - offset);
        size_t n = client.write(wavBuf + offset, toWrite);
        if (n > 0) {
            offset += n;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (offset != wavSize) {
        Serial.printf("[%6.2fs][GROQ] Lỗi stream WAV lên Groq (gửi %zu/%zu bytes)!\n",
                      millis() / 1000.0f, offset, wavSize);
        client.stop();
        isNetworkError = true;
        return "";
    }
    client.print(partFooter);
    client.flush();

    Serial.printf("[%6.2fs][GROQ] Đã gửi WAV (%u bytes). Đang chờ Groq LPU giải mã...\n",
                  millis() / 1000.0f, (unsigned)wavSize);

    // Đọc phản hồi JSON từ Groq
    unsigned long startWait = millis();
    while (!client.available() && (millis() - startWait < 8000)) {
        if (!pipelineBusy) { client.stop(); return ""; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!client.available()) {
        Serial.printf("[%6.2fs][GROQ] Timeout phản hồi từ Groq!\n", millis() / 1000.0f);
        client.stop();
        isNetworkError = true;
        return "";
    }

    bool inHeader = true;
    String transcribedText = "";
    char lineBuf[512];

    while (client.connected() || client.available()) {
        if (client.available()) {
            size_t n = client.readBytesUntil('\n', (uint8_t *)lineBuf, sizeof(lineBuf) - 1);
            if (n == 0) continue;
            lineBuf[n] = '\0';

            while (n > 0 && (lineBuf[n-1] == '\r' || lineBuf[n-1] == ' ' || lineBuf[n-1] == '\t')) {
                lineBuf[--n] = '\0';
            }

            if (inHeader) {
                if (n == 0) inHeader = false;
                continue;
            }

            // Trích xuất trường "text": "..."
            char *textKey = strstr(lineBuf, "\"text\":");
            if (textKey) {
                char *valStart = strchr(textKey + 7, '\"');
                if (valStart) {
                    valStart++;
                    char *p = valStart;
                    while (*p) {
                        if (*p == '\\' && *(p+1)) {
                            p++;
                            if (*p == 'n') transcribedText += '\n';
                            else if (*p == '\"') transcribedText += '\"';
                            else if (*p == '\\') transcribedText += '\\';
                            else transcribedText += *p;
                        } else if (*p == '\"') {
                            break;
                        } else {
                            transcribedText += *p;
                        }
                        p++;
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    client.stop();
    transcribedText.trim();
    return transcribedText;
}

// ============================================================
// Core 0: Gửi Audio WAV lên Deepgram Nova STT (Tầng 2 STT Fallback)
// ============================================================
static String deepgram_transcribe(const uint8_t *wavBuf, size_t wavSize, bool &isNetworkError) {
    isNetworkError = false;
    if (strlen(DEEPGRAM_API_KEY) == 0 || wavBuf == NULL || wavSize <= WAV_HEADER_SIZE) return "";

    Serial.printf("[%6.2fs][DEEPGRAM] Đang kết nối tới Deepgram Nova STT (%s)...\n",
                  millis() / 1000.0f, DEEPGRAM_MODEL);

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(3);
    client.setTimeout(4000);

    if (!client.connect(DEEPGRAM_API_HOST, DEEPGRAM_API_PORT)) {
        Serial.printf("[%6.2fs][DEEPGRAM] Không thể kết nối SSL tới Deepgram!\n", millis() / 1000.0f);
        isNetworkError = true;
        return "";
    }

    String urlPath = String("/v1/listen?model=") + DEEPGRAM_MODEL + "&language=vi&smart_format=true&punctuate=true&numerals=true";

    client.printf("POST %s HTTP/1.1\r\n", urlPath.c_str());
    client.printf("Host: %s\r\n", DEEPGRAM_API_HOST);
    client.printf("Authorization: Token %s\r\n", DEEPGRAM_API_KEY);
    client.printf("Content-Type: audio/wav\r\n");
    client.printf("Content-Length: %zu\r\n", wavSize);
    client.printf("Connection: close\r\n\r\n");

    if (!stream_to_client(client, wavBuf, wavSize)) {
        Serial.printf("[%6.2fs][DEEPGRAM] Lỗi gửi WAV tới Deepgram!\n", millis() / 1000.0f);
        client.stop();
        isNetworkError = true;
        return "";
    }
    client.flush();

    Serial.printf("[%6.2fs][DEEPGRAM] Đã gửi WAV (%zu bytes). Đang chờ Deepgram giải mã...\n",
                  millis() / 1000.0f, wavSize);

    unsigned long startWait = millis();
    while (!client.available() && (millis() - startWait < 6000)) {
        if (!pipelineBusy) { client.stop(); return ""; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!client.available()) {
        Serial.printf("[%6.2fs][DEEPGRAM] Timeout (6s) phản hồi từ Deepgram!\n", millis() / 1000.0f);
        client.stop();
        isNetworkError = true;
        return "";
    }

    String transcribedText = "";
    String accum = "";
    int httpStatus = 0;
    bool inHeader = true;
    char lineBuf[256];

    // Đọc toàn bộ response với timeout tổng tối đa 6 giây (CỰC KỲ AN TOÀN, KHÔNG BAO GIỜ TREO)
    unsigned long readDeadline = millis() + 6000;
    while ((client.connected() || client.available()) && (millis() < readDeadline) && pipelineBusy) {
        if (client.available()) {
            if (inHeader) {
                size_t n = client.readBytesUntil('\n', (uint8_t *)lineBuf, sizeof(lineBuf) - 1);
                if (n == 0) continue;
                lineBuf[n] = '\0';
                while (n > 0 && (lineBuf[n-1] == '\r' || lineBuf[n-1] == ' ' || lineBuf[n-1] == '\t')) {
                    lineBuf[--n] = '\0';
                }
                if (strncmp(lineBuf, "HTTP/1.", 7) == 0) {
                    if (strstr(lineBuf, " 200 ")) httpStatus = 200;
                    else httpStatus = 400;
                }
                if (n == 0) {
                    inHeader = false; // Hết Header!
                }
            } else {
                // Đọc Body
                char c = (char)client.read();
                accum += c;
                int tIdx = accum.indexOf("\"transcript\":\"");
                if (tIdx >= 0) {
                    int startPos = tIdx + 14;
                    int endPos = accum.indexOf('\"', startPos);
                    if (endPos >= 0) {
                        transcribedText = accum.substring(startPos, endPos);
                        break; // ĐÃ LẤY XONG TRANSCRIPT! THOÁT NGAY!
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    client.stop();
    transcribedText.trim();
    if (httpStatus != 200 && httpStatus != 0) {
        isNetworkError = true;
    }
    return transcribedText;
}

static String escape_json_str(const String &src) {
    String out = "";
    out.reserve(src.length() + 16);
    for (size_t i = 0; i < src.length(); i++) {
        char c = src.charAt(i);
        if (c == '\"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') {}
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

// ============================================================
// Core 0: Gửi câu hỏi Text + Ảnh lên Groq Vision API (Qwen 3.8/3.6)
// ============================================================
static String ask_groq_vision(const char *modelName, const String &promptText,
                              const uint8_t *imgB64, size_t imgB64Len, bool &isRateLimit) {
    isRateLimit = false;
    if (strlen(GROQ_API_KEY) == 0) {
        Serial.printf("[%6.2fs][NET] Bỏ qua Groq Vision — Chưa có GROQ_API_KEY!\n", millis() / 1000.0f);
        return "";
    }

    Serial.printf("[%6.2fs][NET] Đang kết nối tới Groq Vision [Model: %s]...\n",
                  millis() / 1000.0f, modelName);

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(4);
    client.setTimeout(10000);

    if (!client.connect(GROQ_API_HOST, GROQ_API_PORT)) {
        Serial.printf("[%6.2fs][NET] Không thể kết nối SSL tới Groq Vision!\n", millis() / 1000.0f);
        return "";
    }
    client.setNoDelay(true);

    bool useMultiTurn = (isFollowUpSession && lastGroqUserPrompt.length() > 0 && lastSavedImgB64 && lastSavedImgB64Len > 0);

    String safePrompt = escape_json_str(promptText);
    String jsonPart1 = "";
    String jsonPart2 = "";
    size_t totalLen = 0;

    if (useMultiTurn) {
        // Gửi Multi-turn: Lượt 1 (Ảnh cũ + Prompt cũ) -> AI Reply cũ -> Lượt 2 (Prompt mới)
        String safeOldPrompt = escape_json_str(lastGroqUserPrompt);
        String safeOldReply  = escape_json_str(lastGroqAiReply);

        jsonPart1 = "{\"model\":\"" + String(modelName) + "\",\"messages\":["
                    "{\"role\":\"user\",\"content\":["
                    "{\"type\":\"text\",\"text\":\"" + safeOldPrompt + "\"},"
                    "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";

        jsonPart2 = "\"}}]},"
                    "{\"role\":\"assistant\",\"content\":\"" + safeOldReply + "\"},"
                    "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"" + safePrompt + "\"}]}"
                    "],\"temperature\":0.3,\"top_p\":0.95,\"reasoning_effort\":\"none\",\"max_completion_tokens\":300,\"stream\":true}";

        totalLen = jsonPart1.length() + lastSavedImgB64Len + jsonPart2.length();
        Serial.printf("[%6.2fs][GROQ] >>> NỐI TIẾP HỘI THOẠI (Gửi Ngữ Cảnh + Ảnh Cũ Lên Groq Vision)... <<<\n", millis() / 1000.0f);
    } else {
        // Single Turn thông thường
        jsonPart1 = "{\"model\":\"" + String(modelName) + "\",\"messages\":[{\"role\":\"user\",\"content\":["
                    "{\"type\":\"text\",\"text\":\"" + safePrompt + "\"},"
                    "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
        jsonPart2 = "\"}}]}],\"temperature\":0.3,\"top_p\":0.95,\"reasoning_effort\":\"none\",\"max_completion_tokens\":300,\"stream\":true}";
        totalLen = jsonPart1.length() + imgB64Len + jsonPart2.length();
    }

    client.printf("POST /openai/v1/chat/completions HTTP/1.1\r\n");
    client.printf("Host: %s\r\n", GROQ_API_HOST);
    client.printf("Authorization: Bearer %s\r\n", GROQ_API_KEY);
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %zu\r\n", totalLen);
    client.printf("Connection: close\r\n\r\n");

    Serial.printf("[%6.2fs][NET] Gửi câu hỏi Text %s lên Groq Vision [%s]...\n",
                  millis() / 1000.0f, useMultiTurn ? "(Dùng lại ảnh cũ)" : "+ Ảnh", modelName);

    bool streamOk = (client.print(jsonPart1) > 0);
    if (useMultiTurn) {
        streamOk = streamOk && stream_to_client(client, lastSavedImgB64, lastSavedImgB64Len);
    } else {
        streamOk = streamOk && stream_to_client(client, imgB64, imgB64Len);
    }
    streamOk = streamOk && (client.print(jsonPart2) > 0);
    client.flush();

    if (!streamOk) {
        Serial.printf("[%6.2fs][NET] Lỗi ghi socket Groq Vision!\n", millis() / 1000.0f);
        client.stop();
        return "";
    }

    unsigned long startWait = millis();
    while (!client.available() && (millis() - startWait < 10000)) {
        if (!pipelineBusy) { client.stop(); return ""; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!client.available()) {
        Serial.printf("[%6.2fs][NET] Hết thời gian chờ phản hồi (10s) từ Groq Vision!\n", millis() / 1000.0f);
        client.stop();
        return "";
    }

    ttsSentenceBuf = "";
    String fullReply = "";
    bool inHeader = true;
    bool inThinking = false;
    unsigned long lastDataTime = millis();
    unsigned long totalDeadline = millis() + 25000;
    int httpStatus = 0;
    char lineBuf[512];

    while ((client.connected() || client.available()) && (millis() - lastDataTime < 4000) && (millis() < totalDeadline)) {
        if (!pipelineBusy) break;

        if (client.available()) {
            size_t n = client.readBytesUntil('\n', (uint8_t *)lineBuf, sizeof(lineBuf) - 1);
            if (n == 0) continue;
            lineBuf[n] = '\0';
            lastDataTime = millis();

            while (n > 0 && (lineBuf[n-1] == '\r' || lineBuf[n-1] == ' ' || lineBuf[n-1] == '\t')) {
                lineBuf[--n] = '\0';
            }

            if (inHeader) {
                if (strncmp(lineBuf, "HTTP/1.", 7) == 0) {
                    Serial.printf("[%6.2fs][NET] %s\n", millis() / 1000.0f, lineBuf);
                    if (strstr(lineBuf, " 200 ")) httpStatus = 200;
                    else if (strstr(lineBuf, " 429 ")) { httpStatus = 429; isRateLimit = true; }
                    else httpStatus = 400;
                }
                if (n == 0) {
                    inHeader = false;
                    if (httpStatus == 200) {
                        Serial.printf("[%6.2fs][NET] AI Trả lời [Groq %s]: ", millis() / 1000.0f, modelName);
                    }
                }
                continue;
            }

            if (httpStatus != 200) {
                if (strstr(lineBuf, "rate_limit_exceeded") || strstr(lineBuf, "Rate limit")) {
                    isRateLimit = true;
                }
                Serial.println(lineBuf);
                continue;
            }

            char *jsonPayload = lineBuf;
            if (strncmp(lineBuf, "data: ", 6) == 0) jsonPayload = lineBuf + 6;
            while (*jsonPayload == ' ') jsonPayload++;
            if (strcmp(jsonPayload, "[DONE]") == 0) break;

            char *contentKey = strstr(jsonPayload, "\"content\":");
            if (contentKey) {
                char *valStart = strchr(contentKey + 10, '\"');
                if (valStart) {
                    valStart++;
                    char *p = valStart;
                    String piece = "";
                    while (*p) {
                        if (*p == '\\' && *(p+1)) {
                            p++;
                            if (*p == 'n') piece += '\n';
                            else if (*p == 'r') piece += '\r';
                            else if (*p == 't') piece += '\t';
                            else if (*p == '\"') piece += '\"';
                            else if (*p == '\\') piece += '\\';
                            else if (*p == 'u' && isxdigit(*(p+1)) && isxdigit(*(p+2)) && isxdigit(*(p+3)) && isxdigit(*(p+4))) {
                                char hex[5] = { *(p+1), *(p+2), *(p+3), *(p+4), 0 };
                                long code = strtol(hex, NULL, 16);
                                p += 4;
                                if (code == 0x3c) piece += '<';
                                else if (code == 0x3e) piece += '>';
                                else if (code < 128) piece += (char)code;
                            } else piece += *p;
                        } else if (*p == '\"') {
                            break;
                        } else {
                            piece += *p;
                        }
                        p++;
                    }

                    if (!inThinking) {
                        if (piece.indexOf("<think") >= 0 || piece.indexOf("u003cthink") >= 0 || piece.indexOf("think>") >= 0) {
                            inThinking = true;
                        }
                    }

                    if (inThinking) {
                        int closeIdx = piece.indexOf("</think>");
                        if (closeIdx < 0) closeIdx = piece.indexOf("u003c/think");
                        if (closeIdx < 0) closeIdx = piece.indexOf("</think");
                        if (closeIdx < 0) closeIdx = piece.indexOf("think>");

                        if (closeIdx >= 0) {
                            inThinking = false;
                            int endTag = piece.indexOf('>', closeIdx);
                            if (endTag >= 0) {
                                piece = piece.substring(endTag + 1);
                            } else {
                                piece = "";
                            }
                        } else {
                            piece = "";
                        }
                    }

                    if (!inThinking && piece.length() > 0) {
                        tone_driver_waiting_music_set(false);
                        Serial.print(piece);
                        fullReply += piece;
                        ttsSentenceBuf += piece;
                        flush_sentences();
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    tone_driver_waiting_music_set(false);
    Serial.println();
    client.stop();

    if (httpStatus == 200 && fullReply.length() > 0) {
        flush_remaining();
    }
    return fullReply;
}

static String ask_gemini_interactions(const char *currentModel, const String &promptText,
                                     const uint8_t *imgB64, size_t imgB64Len, bool &isContextExpired) {
    isContextExpired = false;
    bool hasPrevious = (lastInteractionId.length() > 0);
    if (hasPrevious) {
        Serial.printf("[%6.2fs][INTERACTIONS] >>> Nối tiếp hội thoại (Previous ID: %s)... <<<\n",
                      millis() / 1000.0f, lastInteractionId.c_str());
    } else {
        Serial.printf("[%6.2fs][INTERACTIONS] >>> Bắt đầu phiên hội thoại mới [Model: %s]... <<<\n",
                      millis() / 1000.0f, currentModel);
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(4);
    client.setTimeout(10000);

    if (WiFi.status() != WL_CONNECTED) {
        ensure_wifi();
    }

    if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
        Serial.printf("[%6.2fs][INTERACTIONS] Không thể kết nối SSL tới Gemini Interactions!\n", millis() / 1000.0f);
        lastInteractionId = "";
        return "";
    }

    // 2. Xây dựng JSON Request Payload cho Google Interactions API
    // Schema chuẩn: {"model":"...","input":[{"type":"user_input","content":"..."},{"type":"image","data":"...","mime_type":"image/jpeg"}],"previous_interaction_id":"..."}
    // Gửi CẢ ảnh chụp mới VÀ previous_interaction_id để Google tích lũy toàn bộ ảnh trong session!
    bool sendImageNow = (imgB64 && imgB64Len > 0);

    String jsonHeader = "{\"model\":\"" + String(currentModel) + "\",\"input\":[{\"type\":\"user_input\",\"content\":\"" + promptText + "\"}";
    String jsonImage = "";
    if (sendImageNow) {
        jsonImage = ",{\"type\":\"image\",\"data\":\"";
    }
    String jsonFooter = "";
    if (sendImageNow) {
        jsonFooter = "\",\"mime_type\":\"image/jpeg\"}";
    }
    jsonFooter += "]";
    if (hasPrevious) {
        jsonFooter += ",\"previous_interaction_id\":\"" + lastInteractionId + "\"";
    }
    jsonFooter += ",\"stream\":true";
    jsonFooter += "}";

    size_t totalContentLength = jsonHeader.length() + jsonImage.length() + (sendImageNow ? imgB64Len : 0) + jsonFooter.length();

    String urlPath = String("/v1beta/interactions?key=") + cached_api_key;

    client.setNoDelay(true);
    client.printf("POST %s HTTP/1.1\r\n", urlPath.c_str());
    client.printf("Host: %s\r\n", GEMINI_API_HOST);
    client.printf("Content-Type: application/json\r\n");
    client.printf("Accept: text/event-stream\r\n");
    client.printf("Accept-Encoding: identity\r\n");
    client.printf("Content-Length: %zu\r\n", totalContentLength);
    client.printf("Connection: close\r\n\r\n");

    Serial.printf("[%6.2fs][INTERACTIONS] Gửi câu hỏi Text %s lên Gemini [%s] (Real-time SSE Stream)...\n",
                  millis() / 1000.0f, sendImageNow ? "+ Ảnh mới" : "", currentModel);

    bool streamOk = (client.print(jsonHeader) > 0);
    if (sendImageNow) {
        streamOk = streamOk && (client.print(jsonImage) > 0);
        streamOk = streamOk && stream_to_client(client, imgB64, imgB64Len);
    }
    streamOk = streamOk && (client.print(jsonFooter) > 0);
    client.flush();

    if (!streamOk) {
        Serial.printf("[%6.2fs][INTERACTIONS] Lỗi ghi Socket SSL!\n", millis() / 1000.0f);
        client.stop();
        lastInteractionId = "";
        return "";
    }

    unsigned long startWait = millis();
    while (!client.available() && (millis() - startWait < 10000)) {
        if (!pipelineBusy) { client.stop(); break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!client.available()) {
        Serial.printf("[%6.2fs][INTERACTIONS] Hết thời gian chờ (10s) -> Nhảy thẳng sang Groq Vision!\n", millis() / 1000.0f);
        client.stop();
        lastInteractionId = "";
        return "";
    }

    ttsSentenceBuf = "";
    String fullReply = "";
    int httpStatus = 0;
    char lineBuf[512];
    bool inHeader = true;
    unsigned long lastDataTime = millis();
    unsigned long totalDeadline = millis() + 25000; // Hạn chót tổng tối đa 25 giây cho toàn bộ câu trả lời
    String newInteractionId = "";

    // Đọc Headers & Body với Time Limit
    while ((client.connected() || client.available()) && (millis() - lastDataTime < 4000) && (millis() < totalDeadline)) {
        if (!pipelineBusy) break;

        if (client.available()) {
            size_t n = client.readBytesUntil('\n', (uint8_t *)lineBuf, sizeof(lineBuf) - 1);
            if (n == 0) continue;
            lineBuf[n] = '\0';
            lastDataTime = millis();

            while (n > 0 && (lineBuf[n-1] == '\r' || lineBuf[n-1] == ' ' || lineBuf[n-1] == '\t')) {
                lineBuf[--n] = '\0';
            }

            if (inHeader) {
                if (strncmp(lineBuf, "HTTP/1.", 7) == 0) {
                    Serial.printf("[%6.2fs][INTERACTIONS] %s\n", millis() / 1000.0f, lineBuf);
                    if (strstr(lineBuf, " 200 ")) httpStatus = 200;
                    else httpStatus = 400;
                }
                if (n == 0) {
                    inHeader = false;
                    if (httpStatus == 200) {
                        Serial.printf("[%6.2fs][INTERACTIONS] AI Trả lời [%s]: ", millis() / 1000.0f, currentModel);
                    }
                }
                continue;
            }

            if (httpStatus != 200) {
                Serial.println(lineBuf);
                if (strstr(lineBuf, "not found") || strstr(lineBuf, "NOT_FOUND") ||
                    strstr(lineBuf, "INVALID_ARGUMENT") || strstr(lineBuf, "expired") ||
                    strstr(lineBuf, "context") || strstr(lineBuf, "RESOURCE_EXHAUSTED") ||
                    strstr(lineBuf, "interaction")) {
                    isContextExpired = true;
                    lastInteractionId = "";
                }
                continue;
            }

            // Trích xuất "id":"..." từ JSON trả về
            char *idKey = strstr(lineBuf, "\"id\":\"");
            if (idKey && newInteractionId.length() == 0) {
                char *idStart = idKey + 6;
                char *idEnd = strchr(idStart, '\"');
                if (idEnd) {
                    newInteractionId = String(idStart).substring(0, idEnd - idStart);
                }
            }

            // Khi nhận được tín hiệu kết thúc stream -> ngắt đọc ngay lập tức
            if (strstr(lineBuf, "[DONE]") || strstr(lineBuf, "interaction.completed")) {
                break;
            }

            // Trích xuất Text trong model_output hoặc step.delta streaming
            char *textKey = strstr(lineBuf, "\"type\":\"model_output\"");
            if (!textKey) textKey = strstr(lineBuf, "\"text\":");
            if (textKey) {
                char *actualTextKey = strstr(lineBuf, "\"text\":\"");
                if (actualTextKey) {
                    char *valStart = actualTextKey + 8;
                    char *p = valStart;
                    String chunkText = "";
                    while (*p && *p != '\"') {
                        if (*p == '\\' && *(p+1)) {
                            p++;
                            if (*p == 'n') chunkText += '\n';
                            else if (*p == '\"') chunkText += '\"';
                            else if (*p == 'r') {}
                            else if (*p == 't') chunkText += '\t';
                            else chunkText += *p;
                        } else {
                            chunkText += *p;
                        }
                        p++;
                    }

                    if (chunkText.length() > 0) {
                        tone_driver_waiting_music_set(false);
                        Serial.print(chunkText);
                        fullReply += chunkText;
                        ttsSentenceBuf += chunkText;
                        flush_sentences();
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    tone_driver_waiting_music_set(false);
    client.stop();

    if (newInteractionId.length() > 0) {
        lastInteractionId = newInteractionId;
        lastInteractionMs = millis();
        Serial.printf("\n[%6.2fs][INTERACTIONS] Đã lưu Session ID: %s\n",
                      millis() / 1000.0f, lastInteractionId.c_str());
    }

    if (httpStatus == 200 && fullReply.length() > 0) {
        flush_remaining();
    }

    return fullReply;
}

// ============================================================
// Core 0: Gửi Prompt + Ảnh JPEG lên Gemini (3.5 hoặc 3.1)
// ============================================================
static String ask_gemini_vision(const char *currentModel, const String &promptText, const uint8_t *imgB64, size_t imgB64Len) {
    Serial.printf("[%6.2fs][NET] Đang kết nối tới Gemini API [Model: %s]...\n",
                  millis() / 1000.0f, currentModel);

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(4);
    client.setTimeout(10000);

    if (WiFi.status() != WL_CONNECTED) {
        ensure_wifi();
    }

    if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
        Serial.printf("[%6.2fs][NET] Không thể kết nối SSL tới Gemini [%s]! Nhảy thẳng sang model khác...\n",
                      millis() / 1000.0f, currentModel);
        return "";
    }

    String jsonHeader = "{\"contents\":[{\"parts\":["
                        "{\"text\":\"" + promptText + "\"},"
                        "{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"";
    String jsonFooter = "\"}}]}],\"generationConfig\":{\"temperature\":0.3}}";

    size_t totalContentLength = jsonHeader.length() + imgB64Len + jsonFooter.length();

    String urlPath = String("/v1beta/models/") + currentModel +
                     ":streamGenerateContent?alt=sse&key=" + cached_api_key;

    client.printf("POST %s HTTP/1.1\r\n", urlPath.c_str());
    client.printf("Host: %s\r\n", GEMINI_API_HOST);
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %zu\r\n", totalContentLength);
    client.printf("Connection: close\r\n\r\n");

    Serial.printf("[%6.2fs][NET] Gửi câu hỏi Text + Ảnh(%u bytes) lên Gemini [%s]...\n",
                  millis() / 1000.0f, (unsigned)imgB64Len, currentModel);

    bool streamOk = (client.print(jsonHeader) > 0);
    streamOk = streamOk && stream_to_client(client, imgB64, imgB64Len);
    streamOk = streamOk && (client.print(jsonFooter) > 0);
    client.flush();

    if (!streamOk) {
        Serial.printf("[%6.2fs][NET] Lỗi ghi Socket SSL với model %s!\n", millis() / 1000.0f, currentModel);
        client.stop();
        return "";
    }

    unsigned long startWait = millis();
    while (!client.available() && (millis() - startWait < 15000)) {
        if (!pipelineBusy) { client.stop(); break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!client.available()) {
        Serial.printf("[%6.2fs][NET] Hết thời gian chờ phản hồi từ model %s!\n",
                      millis() / 1000.0f, currentModel);
        client.stop();
        return "";
    }

    ttsSentenceBuf = "";
    String fullReply = "";
    bool inHeader = true;
    unsigned long lastDataTime = millis();
    int httpStatus = 0;
    char lineBuf[512];

    while ((client.connected() || client.available()) && (millis() - lastDataTime < 6000)) {
        if (!pipelineBusy) break;

        if (client.available()) {
            size_t n = client.readBytesUntil('\n', (uint8_t *)lineBuf, sizeof(lineBuf) - 1);
            if (n == 0) continue;
            lineBuf[n] = '\0';
            lastDataTime = millis();

            while (n > 0 && (lineBuf[n-1] == '\r' || lineBuf[n-1] == ' ' || lineBuf[n-1] == '\t')) {
                lineBuf[--n] = '\0';
            }

            if (inHeader) {
                if (strncmp(lineBuf, "HTTP/1.", 7) == 0) {
                    Serial.printf("[%6.2fs][NET] %s\n", millis() / 1000.0f, lineBuf);
                    if (strstr(lineBuf, " 200 ")) httpStatus = 200;
                    else httpStatus = 400;
                }
                if (n == 0) {
                    inHeader = false;
                    if (httpStatus == 200) {
                        Serial.printf("[%6.2fs][NET] AI Trả lời [%s]: ", millis() / 1000.0f, currentModel);
                    } else {
                        Serial.printf("[%6.2fs][NET] Lỗi từ model %s:\n", millis() / 1000.0f, currentModel);
                    }
                }
                continue;
            }

            if (httpStatus != 200) {
                Serial.println(lineBuf);
                continue;
            }

            char *jsonPayload = lineBuf;
            if (strncmp(lineBuf, "data: ", 6) == 0) jsonPayload = lineBuf + 6;
            while (*jsonPayload == ' ') jsonPayload++;
            if (strcmp(jsonPayload, "[DONE]") == 0) break;

            char *textKey = strstr(jsonPayload, "\"text\":");
            if (textKey) {
                char *valStart = strchr(textKey + 7, '\"');
                if (valStart) {
                    valStart++;
                    char *p = valStart;
                    String piece = "";
                    while (*p) {
                        if (*p == '\\' && *(p+1)) {
                            p++;
                            if (*p == 'n') piece += '\n';
                            else if (*p == 'r') piece += '\r';
                            else if (*p == 't') piece += '\t';
                            else if (*p == '\"') piece += '\"';
                            else if (*p == '\\') piece += '\\';
                            else piece += *p;
                        } else if (*p == '\"') {
                            break;
                        } else {
                            piece += *p;
                        }
                        p++;
                    }

                    if (piece.length() > 0) {
                        tone_driver_waiting_music_set(false);
                        Serial.print(piece);
                        fullReply += piece;
                        ttsSentenceBuf += piece;
                        flush_sentences();
                    }
                }
            }

            if (strstr(jsonPayload, "\"finishReason\"")) {
                break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    tone_driver_waiting_music_set(false);
    Serial.println();
    client.stop();

    if (httpStatus == 200 && fullReply.length() > 0) {
        flush_remaining();
    }
    return fullReply;
}

// ============================================================
// Core 0: Gửi ảnh JPEG + câu hỏi Text tới AI (Gemini / Groq Vision)
// ============================================================
static String send_audio_image_to_gemini(void) {
    if (cached_api_key.length() == 0 && strlen(GROQ_API_KEY) == 0) {
        Serial.printf("[%6.2fs][AI] CHƯA CÓ API KEY!\n", millis() / 1000.0f);
        return "";
    }
    if (jpegBuf == NULL || jpegSize == 0) {
        Serial.printf("[%6.2fs][AI] Chưa có ảnh để gửi!\n", millis() / 1000.0f);
        return "";
    }

    WiFi.setSleep(false);

    // 1. Tạo WAV từ PCM
    uint32_t useSamples = (uint32_t)recordSamples;
    size_t rawPcmBytes = (size_t)useSamples * sizeof(int16_t);
    size_t pcmBytes = (rawPcmBytes / 2) * 2;
    size_t wavSize = WAV_HEADER_SIZE + pcmBytes;

    uint8_t *wavBuf = (uint8_t *)ps_malloc(wavSize);
    if (!wavBuf) {
        Serial.printf("[%6.2fs][NET] LỖI: Cấp phát PSRAM lưu WAV thất bại!\n", millis() / 1000.0f);
        return "";
    }
    create_wav_header(wavBuf, pcmBytes, AI_AUDIO_SAMPLE_RATE, 1, 16);
    memcpy(wavBuf + WAV_HEADER_SIZE, (const uint8_t *)recordBuf, pcmBytes);

    // 2. STT: Groq Whisper LPU (Tầng 1 Chính - 0.3s) -> Deepgram Nova-3 (Tầng 2 Dự phòng)
    String questionText = "";
    if (recordSamples >= 4500) {
        bool isSttNetError = false;
        questionText = groq_whisper_transcribe(wavBuf, wavSize, isSttNetError);
        
        // CHỈ KHI LỖI MẠNG / SERVER THẬT SỰ (isSttNetError == true) THÌ MỚI CHUYỂN TẦNG 2!
        // Nếu người dùng chỉ im lặng chụp ảnh (HTTP 200, text = "") -> Không bao giờ chuyển tầng 2!
        if (isSttNetError && strlen(DEEPGRAM_API_KEY) > 0 && pipelineBusy) {
            Serial.printf("[%6.2fs][AI] >>> Groq STT lỗi mạng/không kết nối được -> Chuyển sang Tầng 2 (Deepgram Nova-3 STT)... <<<\n",
                millis() / 1000.0f);
            bool isDgNetError = false;
            questionText = deepgram_transcribe(wavBuf, wavSize, isDgNetError);
        }
    }
    free(wavBuf);

    // Lọc bỏ triệt để ảo giác (Hallucination) của Whisper khi phòng im lặng
    if (questionText.indexOf("Ghiền Mì Gõ") >= 0 ||
        questionText.indexOf("subscribe") >= 0 ||
        questionText.indexOf("Subscribe") >= 0 ||
        questionText.indexOf("đăng ký kênh") >= 0 ||
        questionText.indexOf("Cảm ơn các bạn đã theo dõi") >= 0) {
        questionText = "";
    }

    if (questionText.length() > 0) {
        Serial.printf("[%6.2fs][AI] >>> Giọng nói nhận diện: \"%s\" <<<\n",
                      millis() / 1000.0f, questionText.c_str());
    } else {
        Serial.printf("[%6.2fs][AI] >>> Không có giọng nói trong audio (Chế độ mô tả ảnh) <<<\n",
                      millis() / 1000.0f);
    }

    // 3. Mã hóa Base64 ảnh JPEG
    uint8_t *imgB64 = NULL; size_t imgB64Len = 0;
    if (!base64_encode(jpegBuf, jpegSize, &imgB64, &imgB64Len)) {
        Serial.printf("[%6.2fs][NET] LỖI: Mã hóa Base64 ảnh thất bại!\n", millis() / 1000.0f);
        return "";
    }

    // 4. Prompt
    String promptText = "";
    if (questionText.length() > 0) {
        promptText = "Bạn là trợ lý AI Aegis Sight hỗ trợ người khiếm thị. "
                     "Nhiệm vụ: Trả lời trực tiếp, ngắn gọn dưới 30 từ bằng tiếng Việt. "
                     "ƯU TIÊN HÀNG ĐẦU: Luôn đối chiếu và sử dụng thông tin trong lịch sử cuộc trò chuyện (như tên người, tên vật nuôi, con số, chữ viết hoặc đồ vật vừa nói ở các lượt trước) để trả lời. "
                     "Chỉ phân tích hình ảnh khi người dùng thực sự hỏi về quang cảnh trước mắt hoặc khi lịch sử trò chuyện chưa có câu trả lời. "
                     "Câu hỏi: \\\"" + questionText + "\\\"";
    } else {
        promptText = "Bạn là trợ lý AI Aegis Sight hỗ trợ người khiếm thị. Không cần giới thiệu bản thân. "
                     "Hãy quan sát bức ảnh và mô tả ngắn gọn đồ vật, quang cảnh hoặc chữ (nếu có) phía trước bằng tiếng Việt (dưới 35 từ).";
    }

    // ============================================================
    // HOÁN ĐỔI THÔNG MINH 2 VÒNG:
    // Tầng 1: Google Gemini Interactions API (gemini-3.5 <-> 3.1 với SSE Real-time Stream & Server-side Session ID)
    // Tầng 2: Groq Vision LLM (Qwen 3.8 <-> 3.6 với Multi-turn lưu trong PSRAM)
    // ============================================================
    static const char *kGroqModels[] = { GROQ_VISION_MODEL_A, GROQ_VISION_MODEL_B };
    static size_t groqModelIdx = 0;

    String fullReply = "";

    for (int swapRound = 1; swapRound <= 2; swapRound++) {
        if (!pipelineBusy) break;

        // 1. Thử Tầng 1: Google Gemini Interactions API (Stateful Session Memory)
        Serial.printf("[%6.2fs][NET] >>> [Vòng %d/2] Thử Tầng 1: Google Gemini Interactions API [%s]... <<<\n",
                      millis() / 1000.0f, swapRound, GEMINI_MODEL_PRIMARY);
        bool isContextExpired = false;
        fullReply = ask_gemini_interactions(GEMINI_MODEL_PRIMARY, promptText, imgB64, imgB64Len, isContextExpired);

        // CHỈ đổi sang Gemini 3.1 khi server báo HẾT CONTEXT / LỖI SESSION CŨ:
        if (fullReply.length() == 0 && isContextExpired && pipelineBusy) {
            Serial.printf("[%6.2fs][NET] >>> Gemini 3.5 báo hết Context/Lỗi Session -> Thử phiên mới với Gemini 3.1 [%s]... <<<\n",
                          millis() / 1000.0f, GEMINI_MODEL_FALLBACK);
            bool retryExpired = false;
            fullReply = ask_gemini_interactions(GEMINI_MODEL_FALLBACK, promptText, imgB64, imgB64Len, retryExpired);
        }

        if (fullReply.length() > 0) break;

        // 2. Nếu Gemini gặp sự cố/quá tải -> Nhảy thẳng sang Tầng 2: Groq Vision LLM!
        Serial.printf("[%6.2fs][NET] >>> Gemini gặp sự cố -> Nhảy sang Tầng 2 (Groq Vision [%s])... <<<\n",
                      millis() / 1000.0f, kGroqModels[groqModelIdx]);
        bool isRateLimit = false;
        fullReply = ask_groq_vision(kGroqModels[groqModelIdx], promptText, imgB64, imgB64Len, isRateLimit);

        // Nếu Groq bị Rate Limit -> tự động đảo model Groq (3.8 <-> 3.6) và thử lại ngay
        if (fullReply.length() == 0 && isRateLimit && pipelineBusy) {
            groqModelIdx = (groqModelIdx + 1) % 2; // Đổi sang model kia
            Serial.printf("[%6.2fs][NET] >>> Groq bị Rate Limit! Đổi sang '%s' và thử lại ngay... <<<\n",
                          millis() / 1000.0f, kGroqModels[groqModelIdx]);
            vTaskDelay(pdMS_TO_TICKS(50));
            fullReply = ask_groq_vision(kGroqModels[groqModelIdx], promptText, imgB64, imgB64Len, isRateLimit);
        }

        if (fullReply.length() > 0) break;

        if (swapRound < 2 && pipelineBusy) {
            Serial.printf("[%6.2fs][NET] >>> Hoán đổi Vòng 1 kết thúc. Bắt đầu hoán đổi Vòng 2... <<<\n", millis() / 1000.0f);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    if (fullReply.length() > 0) {
        lastGroqUserPrompt = promptText;
        lastGroqAiReply = fullReply;

        // Sao lưu ảnh Base64 vào PSRAM cho các lượt hỏi tiếp theo (nếu là ảnh của lượt đầu)
        if (imgB64 && imgB64Len > 0 && !isFollowUpSession) {
            if (lastSavedImgB64) free(lastSavedImgB64);
            lastSavedImgB64 = (uint8_t *)ps_malloc(imgB64Len);
            if (lastSavedImgB64) {
                memcpy(lastSavedImgB64, imgB64, imgB64Len);
                lastSavedImgB64Len = imgB64Len;
            }
        }
    }

    if (imgB64) {
        free(imgB64);
        imgB64 = NULL;
    }

    return fullReply;
}

// ============================================================
// Core 0 task chính
// ============================================================
static void ai_net_task(void *pv) {
    while (true) {
        // Proactive Wi-Fi & SSL Pre-connect: kết nối sẵn Wi-Fi và đường truyền SSL tới Groq trong lúc người dùng đang nói
        if (pipelineBusy && !dataReady) {
            if (ensure_wifi()) {
                pre_connect_groq();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!pipelineBusy || !dataReady) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        if (!ensure_wifi()) {
            Serial.printf("[%6.2fs][NET] Bỏ lượt hỏi — Wi-Fi không kết nối được\n", millis() / 1000.0f);
            tone_driver_waiting_music_set(false);
            Serial.printf("[%6.2fs][AI] >>> Phát giọng nói OFFLINE: 'Kết nối không thành công' <<<\n", millis() / 1000.0f);
            tone_driver_stream_set_active(false);
            tts_driver_play_progmem(OFFLINE_FAIL_MP3, OFFLINE_FAIL_MP3_LEN);
            tts_driver_wait_playback_done();

            pipelineBusy = false; dataReady = false;
            wifi_sleep();
            continue;
        }
        if (!creds_loaded) wifi_creds_refresh();
        if (cached_api_key.length() == 0) {
            Serial.printf("[%6.2fs][NET] Bỏ lượt hỏi — CHƯA CÓ API KEY!\n", millis() / 1000.0f);
            tone_driver_waiting_music_set(false);
            Serial.printf("[%6.2fs][AI] >>> Phát giọng nói OFFLINE: 'Kết nối không thành công' <<<\n", millis() / 1000.0f);
            tone_driver_stream_set_active(false);
            tts_driver_play_progmem(OFFLINE_FAIL_MP3, OFFLINE_FAIL_MP3_LEN);
            tts_driver_wait_playback_done();

            pipelineBusy = false; dataReady = false;
            wifi_sleep();
            continue;
        }

        // --- Gửi ảnh + audio tới Gemini SSE stream ---
        String reply = send_audio_image_to_gemini();

        if (reply.length() > 0 && pipelineBusy) {
            // Theo dõi động: chờ TTS giải mã và loa phát xong 100% tất cả các câu (không giới hạn 25s cứng)
            uint32_t lastActivityMs = millis();
            while (pipelineBusy) {
                bool isBusy = (uxQueueMessagesWaiting(ttsSentenceQueue) > 0 ||
                               tts_driver_is_busy() ||
                               tone_driver_stream_available() > 0);
                if (isBusy) {
                    lastActivityMs = millis();
                    vTaskDelay(pdMS_TO_TICKS(20));
                } else {
                    // Đợi thêm 150ms để chắc chắn không còn âm thanh sót lại
                    if (millis() - lastActivityMs >= 150) break;
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }

            // Tiếng chuông báo hiệu hoàn thành toàn bộ câu trả lời
            Serial.printf("[%6.2fs][AI] >>> Hoàn thành câu trả lời! Phát tiếng chuông báo hiệu <<<\n", millis() / 1000.0f);
            tone_driver_play_captain_chime();
            vTaskDelay(pdMS_TO_TICKS(350)); // Chờ tiếng chuông ngân vang trọn vẹn 350ms
        } else if (pipelineBusy) {
            // Khi kết nối thất bại (SSL/DNS/Server Error) → phát giọng fail offline
            tone_driver_waiting_music_set(false);
            Serial.printf("[%6.2fs][AI] >>> Phát giọng nói OFFLINE: 'Kết nối không thành công' <<<\n", millis() / 1000.0f);
            tone_driver_stream_set_active(false);
            tts_driver_play_progmem(OFFLINE_FAIL_MP3, OFFLINE_FAIL_MP3_LEN);
            tts_driver_wait_playback_done();
        }

        tone_driver_waiting_music_set(false);
        tone_driver_stream_set_active(false);
        pipelineBusy = false;
        dataReady = false;
        wifi_sleep();
    }
}

// ============================================================
// Public API
// ============================================================
void ai_pipeline_start(void) {}
void ai_pipeline_stop(void) {
    if (!pipelineBusy) return;
    pipelineBusy = false; dataReady = false;
    tone_driver_waiting_music_set(false);
    tts_driver_stop();
    tone_driver_stream_set_active(false);
    wifi_sleep();
}

bool ai_pipeline_is_busy(void) {
    return pipelineBusy || tts_driver_is_busy() ||
           (ttsSentenceQueue != NULL && uxQueueMessagesWaiting(ttsSentenceQueue) > 0);
}

void ai_pipeline_net_task_start(void) {
    if (!ttsSentenceQueue) {
        ttsSentenceQueue = xQueueCreate(32, sizeof(char *)); // 32 câu thoải mái cho câu chuyện dài
    }
    if (!ttsWorkerTaskHandle) {
        xTaskCreatePinnedToCore(tts_worker_task, "tts_worker", 10240, NULL, 3, &ttsWorkerTaskHandle, 0);
    }
    xTaskCreatePinnedToCore(ai_net_task, "ai_net", 32768, NULL, 2, NULL, 0);
}

void ai_pipeline_audio_task_start(void) {
    xTaskCreatePinnedToCore(ai_audio_task, "ai_audio", 8192, NULL, 4, NULL, 1);
}

#else
void ai_pipeline_start(void) {}
void ai_pipeline_stop(void) {}
bool ai_pipeline_is_busy(void) { return false; }
void ai_pipeline_net_task_start(void) {}
void ai_pipeline_audio_task_start(void) {}
#endif
