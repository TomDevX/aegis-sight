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
    // Ngắt sớm ở dấu phẩy/chấm phẩy/gạch ngang nếu cụm từ đã có từ 20 ký tự để phát ra loa tức thì!
    if ((c == ',' || c == ';' || c == ':' || c == '-') && i >= 20) {
        return true;
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
        .dma_buf_count = 4,
        .dma_buf_len = 512,
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

// ============================================================
// Ghi âm từ lúc NHẤN tới khi THẢ nút (tối đa AI_AUDIO_MAX_RECORD_MS)
// ============================================================
static size_t record_until_release(void) {
    // Nghỉ 120ms sau chuông để màng loa tắt rung động hoàn toàn, tránh dội vào mic
    vTaskDelay(pdMS_TO_TICKS(120));

    rec_mic_install();
    i2s_zero_dma_buffer(I2S_MIC_PORT);

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

    while ((millis() - start < AI_AUDIO_MAX_RECORD_MS) &&
           (samples < AI_AUDIO_MAX_SAMPLES)) {
        // Ghi nhận thời điểm vừa nhấc nút
        if (!released && (millis() - start > RECORD_MIN_MS) && digitalRead(BTN_TRIGGER) == HIGH) {
            released = true;
            releaseTime = millis();
        }

        // Thu thêm 350ms sau khi nhả nút để giữ trọn vẹn 100% âm đuôi của từ cuối cùng
        if (released && (millis() - releaseTime >= 350)) {
            break;
        }

        size_t bytes_read = 0;
        esp_err_t res = i2s_read(I2S_MIC_PORT, i2sBuffer, sizeof(i2sBuffer),
                                 &bytes_read, portMAX_DELAY);
        if (res != ESP_OK || bytes_read == 0) continue;

        int samplesCount = bytes_read / sizeof(int32_t);
        for (int i = 0; i < samplesCount; i++) {
            if (samples >= AI_AUDIO_MAX_SAMPLES) break;

            // 1. Lấy 16-bit PCM từ INMP441 (dịch 14 bit)
            int32_t raw_sample = i2sBuffer[i] >> 14;

            // 2. Khử điện áp một chiều tĩnh (DC Offset Filter) đưa sóng âm về trục 0 chuẩn Whisper
            dc_offset = 0.995f * dc_offset + 0.005f * (float)raw_sample;
            float clean_sample = (float)raw_sample - dc_offset;

            // 3. Khống chế biên độ an toàn
            if (clean_sample > 32000.0f) clean_sample = 32000.0f;
            else if (clean_sample < -32000.0f) clean_sample = -32000.0f;

            int16_t sample_16bit = (int16_t)clean_sample;
            recordBuf[samples++] = sample_16bit;

            sumSq += (double)sample_16bit * (double)sample_16bit;
            if (abs(sample_16bit) > maxPeak) maxPeak = abs(sample_16bit);
        }
    }

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

    if (srcFmt == PIXFORMAT_RGB565 || srcFmt == PIXFORMAT_GRAYSCALE) {
        ok = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, srcFmt,
                     AI_JPEG_QUALITY, &outJpg, &outLen);
    } else {
        // Fallback: sensor trả JPEG trực tiếp
        if (fb->len <= AI_JPEG_BUF_SIZE) {
            memcpy(jpegBuf, fb->buf, fb->len);
            jpegSize = fb->len;
            ok = true;
        }
    }
    uint16_t w = fb->width, h = fb->height;
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
        Serial.printf("[AI] Đã chụp: %ux%u -> JPEG %u bytes\n", w, h, (unsigned)jpegSize);
        return true;
    }
    Serial.println("[AI] Lỗi nén JPEG (fmt2jpg)!");
    return false;
}

// ============================================================
// Core 1: Nút nhấn Hold-to-Talk + chụp ảnh
// Bấm giữ  -> ghi âm mic ngay lập tức (không bị trễ)
// Thả nút  -> dừng ghi âm, chụp JPEG, báo dataReady cho Core 0
// Bấm lần nữa khi đang chạy -> HỦY pipeline
// ============================================================
static void ai_audio_task(void *pv) {
    pinMode(BTN_TRIGGER, INPUT_PULLUP);
    int lastBtn = HIGH;
    uint32_t debounceMs = 0;
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
                if (!alloc_buffers()) continue;

                // Tăng tốc CPU lên 240MHz để xử lý ảnh, nén JPEG và AI siêu tốc
                setCpuFrequencyMhz(240);

                tts_driver_stop();

                pipelineBusy = true;
                dataReady = false;
                jpegSize = 0;
                recordSamples = 0;

                // Tiếng chuông Captain Speaking máy bay (Cabin PA Ding-Dong) báo hiệu bắt đầu lắng nghe
                tone_driver_play_captain_chime();

                // 1. Thu âm mic NGAY SAU tiếng chuông
                recordSamples = record_until_release();

                // 2. Chụp ảnh ngay khi vừa thả nút
                if (!capture_jpeg()) {
                    pipelineBusy = false;
                    continue;
                }

                dataReady = true;
                // Bật nhạc chờ Elevator Music từ PSRAM (Zero-CPU) trong lúc gửi và chờ AI xử lý
                tone_driver_waiting_music_set(true);
                Serial.printf("[%6.2fs][AI] Đã thu âm %zu samples + ảnh -> Gửi lên Gemini...\n", millis() / 1000.0f, recordSamples);
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
static String groq_whisper_transcribe(const uint8_t *wavBuf, size_t wavSize) {
    if (wavBuf == NULL || wavSize <= WAV_HEADER_SIZE) return "";

    if (!groqSttClient.connected()) {
        Serial.printf("[%6.2fs][GROQ] Đang kết nối tới Groq LPU (Whisper STT)...\n", millis() / 1000.0f);
        groqSttClient.setInsecure();
        groqSttClient.setHandshakeTimeout(4);
        groqSttClient.setTimeout(5000);
        if (!groqSttClient.connect(GROQ_API_HOST, GROQ_API_PORT)) {
            Serial.printf("[%6.2fs][GROQ] Lỗi kết nối tới %s!\n", millis() / 1000.0f, GROQ_API_HOST);
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
    
    // Stream file WAV nhị phân siêu tốc (chia theo TLS chunk 16KB, NoDelay)
    size_t offset = 0;
    while (offset < wavSize && client.connected() && pipelineBusy) {
        size_t toWrite = (wavSize - offset > 16384) ? 16384 : (wavSize - offset);
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
// Core 0: Gửi Text + Base64 JPEG lên Groq Vision (OpenAI Format)
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

    String jsonHeader = "{\"model\":\"" + String(modelName) + "\",\"messages\":[{\"role\":\"user\",\"content\":["
                        "{\"type\":\"text\",\"text\":\"" + promptText + "\"},"
                        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
    String jsonFooter = "\"}}]}],\"temperature\":0.3,\"top_p\":0.95,\"reasoning_effort\":\"none\",\"max_completion_tokens\":300,\"stream\":true}";

    size_t totalLen = jsonHeader.length() + imgB64Len + jsonFooter.length();

    client.printf("POST /openai/v1/chat/completions HTTP/1.1\r\n");
    client.printf("Host: %s\r\n", GROQ_API_HOST);
    client.printf("Authorization: Bearer %s\r\n", GROQ_API_KEY);
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %zu\r\n", totalLen);
    client.printf("Connection: close\r\n\r\n");

    Serial.printf("[%6.2fs][NET] Gửi câu hỏi Text + Ảnh(%u bytes) lên Groq Vision [%s]...\n",
                  millis() / 1000.0f, (unsigned)imgB64Len, modelName);

    bool streamOk = (client.print(jsonHeader) > 0);
    streamOk = streamOk && stream_to_client(client, imgB64, imgB64Len);
    streamOk = streamOk && (client.print(jsonFooter) > 0);
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
        Serial.printf("[%6.2fs][NET] Timeout phản hồi từ Groq Vision!\n", millis() / 1000.0f);
        client.stop();
        return "";
    }

    ttsSentenceBuf = "";
    String fullReply = "";
    bool inHeader = true;
    bool inThinking = false;
    unsigned long lastDataTime = millis();
    int httpStatus = 0;
    char lineBuf[512];

    while ((client.connected() || client.available()) && (millis() - lastDataTime < 5000)) {
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

    // 2. STT bằng Groq LPU Whisper (~250ms)
    String questionText = "";
    if (recordSamples >= 4500) {
        questionText = groq_whisper_transcribe(wavBuf, wavSize);
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
        promptText = "Bạn là trợ lý AI thông minh Aegis Sight. Không cần giới thiệu bản thân. "
                     "Câu hỏi của người dùng: \\\"" + questionText + "\\\". "
                     "Nhiệm vụ: Trả lời trực tiếp và chính xác câu hỏi ngắn gọn, đúng trọng tâm bằng tiếng Việt (dưới 35 từ). "
                     "Chỉ dùng bức ảnh đính kèm nếu câu hỏi có ý hỏi về hình ảnh, đọc chữ hoặc đồ vật xung quanh. Nếu là câu hỏi kiến thức, toán học hay trò chuyện thì trả lời thẳng đáp án.";
    } else {
        promptText = "Bạn là trợ lý AI thông minh Aegis Sight hỗ trợ người khiếm thị. Không cần giới thiệu bản thân."
                     "Hãy quan sát bức ảnh và mô tả ngắn gọn đồ vật hoặc quang cảnh hoặc chữ (nếu có) phía trước bằng tiếng Việt (dưới 35 từ).";
    }

    // ============================================================
    // HOÁN ĐỔI THÔNG MINH 2 VÒNG (GEMINI <-> GROQ)
    // Vòng 1: Gemini 3.5 (1 lần) -> Thất bại -> Groq Vision (3.8 <-> 3.6)
    // Vòng 2: Nếu Groq thất bại -> Thử lại Gemini 3.5 -> Thử lại Groq
    // Sau 2 vòng mà cả 2 đều không phản hồi -> Mới báo lỗi kết nối!
    // ============================================================
    static const char *kGroqModels[] = { GROQ_VISION_MODEL_A, GROQ_VISION_MODEL_B };
    static size_t groqModelIdx = 0;

    String fullReply = "";

    for (int swapRound = 1; swapRound <= 2; swapRound++) {
        if (!pipelineBusy) break;

        // 1. Thử Gemini 3.5 (1 lần duy nhất, không chờ retry)
        Serial.printf("[%6.2fs][NET] >>> [Vòng %d/2] Thử Tầng 1: Google Gemini 3.5 Flash Lite... <<<\n",
                      millis() / 1000.0f, swapRound);
        fullReply = ask_gemini_vision(GEMINI_MODEL_PRIMARY, promptText, imgB64, imgB64Len);
        if (fullReply.length() > 0) break;

        // 2. Gemini fail 1 lần -> Nhảy thẳng sang Groq Vision ngay lập tức!
        Serial.printf("[%6.2fs][NET] >>> Gemini thất bại -> Nhảy thẳng sang Tầng 2 (Groq Vision [%s])... <<<\n",
                      millis() / 1000.0f, kGroqModels[groqModelIdx]);
        bool isRateLimit = false;
        fullReply = ask_groq_vision(kGroqModels[groqModelIdx], promptText, imgB64, imgB64Len, isRateLimit);
        if (fullReply.length() > 0) break;

        // Nếu Groq bị Rate Limit -> tự động đảo model Groq (3.8 <-> 3.6) và thử lại ngay
        if (isRateLimit && pipelineBusy) {
            groqModelIdx = (groqModelIdx + 1) % 2; // Đổi sang model kia
            Serial.printf("[%6.2fs][NET] >>> Groq bị Rate Limit! Đổi sang '%s' và thử lại ngay... <<<\n",
                          millis() / 1000.0f, kGroqModels[groqModelIdx]);
            vTaskDelay(pdMS_TO_TICKS(50));
            fullReply = ask_groq_vision(kGroqModels[groqModelIdx], promptText, imgB64, imgB64Len, isRateLimit);
            if (fullReply.length() > 0) break;
        }

        if (swapRound < 2 && pipelineBusy) {
            Serial.printf("[%6.2fs][NET] >>> Hoán đổi Vòng 1 kết thúc. Bắt đầu hoán đổi Vòng 2... <<<\n", millis() / 1000.0f);
            vTaskDelay(pdMS_TO_TICKS(100));
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
