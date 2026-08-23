#include <Arduino.h>
#include "config.h"
#include "ai_pipeline.h"
#include "fall_detection.h"
#include "tone_driver.h"
#include "tts_driver.h"
#include "secrets.h"
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
static volatile bool dataReady = false;
static volatile bool pipelineBusy = false;

#define WAV_HEADER_SIZE       44
#define RECORD_MIN_MS         300     // tối thiểu 300ms mới coi là có câu hỏi

static bool alloc_buffers(void);

// Forward declarations
static bool   is_sentence_end(const String &buf, size_t i);
static String normalize_text(const String &text);
static void   speak_text(const String &text);

// ============================================================
// HTTP helpers
// ============================================================
static bool http_read_line(WiFiClient &cl, char *buf, size_t sz, int tmoMs) {
    size_t pos = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)tmoMs) {
        if (cl.available()) {
            int c = cl.read();
            if (c < 0) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
            if (c == '\n') { buf[pos] = 0; return true; }
            if (c != '\r' && pos < sz - 1) buf[pos++] = (char)c;
            t0 = millis();
        } else if (!cl.connected()) break;
        else vTaskDelay(pdMS_TO_TICKS(5));
    }
    buf[pos] = 0;
    return pos > 0 || cl.connected();
}

static bool http_skip_headers(WiFiClient &cl, int tmoMs) {
    char buf[256];
    while (true) {
        if (!http_read_line(cl, buf, sizeof(buf), tmoMs)) return false;
        if (buf[0] == 0) return true;
    }
}

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
// Sentence boundary ('.' bỏ qua giữa 2 chữ số, ví dụ 2.147)
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
    int lastBoundary = -1;
    for (int i = 0; i < (int)ttsSentenceBuf.length(); i++)
        if (is_sentence_end(ttsSentenceBuf, i)) lastBoundary = i;
    if (lastBoundary >= 0) {
        String complete = ttsSentenceBuf.substring(0, lastBoundary + 1);
        complete.trim();
        if (complete.length() > 0) speak_text(normalize_text(complete));
        ttsSentenceBuf = ttsSentenceBuf.substring(lastBoundary + 1);
    }
}

static void flush_remaining(void) {
    ttsSentenceBuf.trim();
    if (ttsSentenceBuf.length() > 0) speak_text(normalize_text(ttsSentenceBuf));
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
// Speak text in chunks at word boundaries
// ============================================================
static void speak_text(const String &text) {
    if (!pipelineBusy) return;
    int pos = 0, len = text.length();
    while (pos < len && pipelineBusy) {
        int end = pos + TTS_CLOUD_MAX_CHARS;
        if (end >= len) end = len;
        else {
            int sp = end;
            while (sp > pos && text[sp] != ' ') sp--;
            if (sp == pos) sp = end;
            end = sp;
        }
        String chunk = text.substring(pos, end);
        chunk.trim();
        if (chunk.length() > 0) {
            tts_driver_speak(chunk.c_str(), chunk.length());
        }
        pos = end;
        while (pos < len && text[pos] == ' ') pos++;
    }
}

// ============================================================
// WiFi
// ============================================================
struct wifi_cred { String ssid, pass; };
static wifi_cred cached_nets[3];
static bool creds_loaded = false;
static String cached_last_ssid;
static String cached_api_key;

static void wifi_creds_refresh(void) {
    cached_nets[0].ssid = secrets_get(SK_WIFI_SSID);
    cached_nets[0].pass = secrets_get(SK_WIFI_PASS);
    cached_nets[1].ssid = secrets_get(SK_WIFI_SSID2);
    cached_nets[1].pass = secrets_get(SK_WIFI_PASS2);
    cached_nets[2].ssid = secrets_get(SK_WIFI_SSID3);
    cached_nets[2].pass = secrets_get(SK_WIFI_PASS3);
    cached_last_ssid = secrets_get(SK_LAST_SSID);
    cached_api_key = secrets_get(SK_GEMINI_KEY);
    creds_loaded = true;
}

static bool try_ssid(const char *ssid, const char *pass, int tries) {
    if (!ssid || !*ssid) return false;
    WiFi.begin(ssid, pass);
    int c = 0;
    while (WiFi.status() != WL_CONNECTED && c < tries) {
        vTaskDelay(pdMS_TO_TICKS(250)); c++;
    }
    return WiFi.status() == WL_CONNECTED;
}

static bool ensure_wifi(void) {
    if (WiFi.status() == WL_CONNECTED) return true;
    if (!creds_loaded) wifi_creds_refresh();
    if (cached_nets[0].ssid.length() == 0) return false;
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
    if (!ok) { WiFi.disconnect(true); return false; }
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
// Stream dữ liệu lớn lên socket SSL theo chunk (chống crash)
// ============================================================
static bool stream_to_client(WiFiClientSecure &client, const uint8_t *data, size_t len) {
    if (!data || len == 0) return false;

    size_t offset = 0;
    while (offset < len) {
        if (!pipelineBusy) return false;
        size_t chunk = (len - offset > 1024) ? 1024 : (len - offset);
        size_t written = client.write(data + offset, chunk);
        if (written == 0) return false;
        offset += written;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return true;
}

// ============================================================
// Mic I2S cho ghi âm Hold-to-Talk (giống initMicI2S trong test:
// 32-bit ONLY_LEFT @16kHz — chuẩn INMP441)
// ============================================================
static bool rec_mic_install(void) {
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
    return true;
}

// ============================================================
// Ghi âm từ lúc NHẤN tới khi THẢ nút (tối đa AI_AUDIO_MAX_RECORD_MS)
// Xử lý mẫu giống hệt recordAudio3Seconds trong mic_ai_cam_test:
// đọc 32-bit, shift >>8 (INMP441 24-bit trong slot 32), lọc DC-offset, gain
// ============================================================
static size_t record_until_release(void) {
    if (!rec_mic_install()) {
        Serial.println("[AI] Không cài được I2S mic để ghi âm!");
        return 0;
    }

    int32_t *rawBuf = (int32_t *)ps_malloc(512 * sizeof(int32_t));
    if (!rawBuf) {
        i2s_driver_uninstall(I2S_MIC_PORT);
        return 0;
    }

    // Đọc bỏ vài block đầu để ổn định clock I2S
    size_t dummy_bytes = 0;
    i2s_read(I2S_MIC_PORT, rawBuf, 512 * sizeof(int32_t), &dummy_bytes, pdMS_TO_TICKS(100));

    size_t samples = 0;
    float dc_offset = 0.0f;
    unsigned long start = millis();

    while ((millis() - start < AI_AUDIO_MAX_RECORD_MS) &&
           (samples < AI_AUDIO_MAX_SAMPLES)) {
        // Thả nút -> kết thúc ghi âm
        if ((millis() - start > RECORD_MIN_MS) && digitalRead(BTN_TRIGGER) == HIGH) break;

        size_t bytes_read = 0;
        esp_err_t res = i2s_read(I2S_MIC_PORT, rawBuf, 512 * sizeof(int32_t),
                                 &bytes_read, portMAX_DELAY);
        if (res != ESP_OK || bytes_read == 0) continue;

        size_t count = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < count; i++) {
            if (samples >= AI_AUDIO_MAX_SAMPLES) break;

            int32_t s24 = rawBuf[i] >> 8;
            float sample = (float)s24;

            dc_offset = 0.99f * dc_offset + 0.01f * sample;
            float clean = (sample - dc_offset) * AI_AUDIO_GAIN;

            if (clean > 32767.0f) clean = 32767.0f;
            if (clean < -32768.0f) clean = -32768.0f;

            recordBuf[samples++] = (int16_t)clean;
        }
    }

    free(rawBuf);
    i2s_driver_uninstall(I2S_MIC_PORT);

    unsigned long elapsedMs = millis() - start;
    Serial.printf("[AI] Ghi âm xong: %zu samples (%lu ms)\n", samples, elapsedMs);

    // Quá ngắn -> coi như không có câu hỏi hợp lệ
    if (elapsedMs < RECORD_MIN_MS) return 0;
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
// Bấm giữ  -> ghi âm mic (auto_volume tự nhả mic vì pipelineBusy=true)
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
            if (fall_alarm_busy()) {
                // Nút đang dùng để tắt cảnh báo té ngã — bỏ qua AI
                lastBtn = btn;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (!pipelineBusy) {
                if (!alloc_buffers()) continue;

                // Reset ring buffer + dừng playback còn sót từ chu kỳ trước
                tts_driver_stop();

                pipelineBusy = true;
                dataReady = false;
                jpegSize = 0;
                recordSamples = 0;

                // Đợi auto_volume nhả mic I2S (uninstall driver của nó)
                vTaskDelay(pdMS_TO_TICKS(100));

                // 1. Ghi âm đến khi thả nút
                recordSamples = record_until_release();

                // 2. Chụp ảnh ngay khi thả nút
                if (!capture_jpeg()) {
                    pipelineBusy = false;
                    continue;
                }

                if (recordSamples > 0) {
                    dataReady = true;
                    Serial.println("[AI] Dữ liệu sẵn sàng — gửi lên Gemini...");
                } else {
                    // Không có audio vẫn gửi ảnh-only (chỉ nhìn không hỏi)
                    dataReady = true;
                    Serial.println("[AI] Không có audio — gửi ảnh-only...");
                }
            } else {
                // Bấm trong lúc đang chạy -> hủy
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
// Core 0: Gửi ảnh JPEG + audio WAV (base64 inline) tới Gemini
// generateContent — GIỐNG HỆT sendAudioImageToGemini trong test
// ============================================================
static String send_audio_image_to_gemini(void) {
    if (cached_api_key.length() == 0) {
        Serial.println("[AI] CHƯA CÓ API KEY!");
        return "";
    }
    if (jpegBuf == NULL || jpegSize == 0) {
        Serial.println("[AI] Chưa có ảnh để gửi!");
        return "";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20);

    Serial.println("[NET] Đang kết nối tới Gemini API...");
    if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
        Serial.println("[NET] Kết nối AI thất bại!");
        return "";
    }

    // 1. Tạo WAV từ PCM đã ghi (đủ dùng, không cố ép đủ 2s như test)
    uint32_t useSamples = (uint32_t)recordSamples;
    size_t pcmBytes = (size_t)useSamples * sizeof(int16_t);
    size_t wavSize = WAV_HEADER_SIZE + pcmBytes;

    uint8_t *wavBuf = NULL;
    if (useSamples > 0) {
        wavBuf = (uint8_t *)ps_malloc(wavSize);
        if (!wavBuf) {
            Serial.println("[NET] LỖI: Cấp phát PSRAM lưu WAV thất bại!");
            client.stop();
            return "";
        }
        create_wav_header(wavBuf, pcmBytes, AI_AUDIO_SAMPLE_RATE, 1, 16);
        memcpy(wavBuf + WAV_HEADER_SIZE, (const uint8_t *)recordBuf, pcmBytes);
    }

    // 2. Mã hóa Base64
    uint8_t *audB64 = NULL; size_t audB64Len = 0;
    uint8_t *imgB64 = NULL; size_t imgB64Len = 0;

    if (wavBuf) {
        Serial.println("[NET] Đang mã hóa WAV sang Base64...");
        if (!base64_encode(wavBuf, wavSize, &audB64, &audB64Len)) {
            Serial.println("[NET] LỖI: Mã hóa Base64 audio thất bại!");
            free(wavBuf); client.stop();
            return "";
        }
        free(wavBuf);
    }

    Serial.println("[NET] Đang mã hóa Ảnh JPEG sang Base64...");
    if (!base64_encode(jpegBuf, jpegSize, &imgB64, &imgB64Len)) {
        Serial.println("[NET] LỖI: Mã hóa Base64 ảnh thất bại!");
        free(audB64); client.stop();
        return "";
    }

    // 3. Đóng gói JSON inline_data (giống hệt test)
    String jsonHeader = "{\"contents\":[{\"parts\":["
                        "{\"text\":\"Bạn là trợ lý người mù. Hãy nhìn hình ảnh";
    if (audB64) jsonHeader += " và nghe âm thanh câu hỏi";
    jsonHeader += ", trả lời ngắn gọn dưới 100 từ bằng tiếng Việt.\"},"
                   "{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"";

    String jsonMid     = audB64 ? "\"}},{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"" : "";
    String jsonFooter  = "\"}}]}]}";

    size_t totalContentLength = jsonHeader.length() + imgB64Len +
                                jsonMid.length() + audB64Len + jsonFooter.length();
    String urlPath = String("/v1beta/models/") + GEMINI_MODEL_SHORT +
                     ":generateContent?key=" + cached_api_key;

    // 4. Gửi HTTP headers
    client.printf("POST %s HTTP/1.1\r\n", urlPath.c_str());
    client.printf("Host: %s\r\n", GEMINI_API_HOST);
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %zu\r\n", totalContentLength);
    client.printf("Connection: close\r\n\r\n");

    // 5. Stream JSON body
    Serial.printf("[NET] Đang stream Ảnh(%u)%s%s lên Google Server...\n",
                  (unsigned)imgB64Len,
                  audB64 ? " + Audio(" : "",
                  audB64 ? "" : "");
    if (audB64) Serial.printf("[NET]   Audio base64: %u bytes\n", (unsigned)audB64Len);

    bool streamOk = client.print(jsonHeader) != 0;
    streamOk = streamOk && stream_to_client(client, imgB64, imgB64Len);
    if (audB64) {
        client.print(jsonMid);
        streamOk = streamOk && stream_to_client(client, audB64, audB64Len);
    }
    client.print(jsonFooter);
    client.flush();

    free(imgB64);
    free(audB64);
    imgB64 = NULL; audB64 = NULL;

    if (!streamOk) {
        Serial.println("[NET] Lỗi ghi Socket SSL!");
        client.stop();
        return "";
    }

    // 6. Nhận phản hồi (full JSON — generateContent không phải SSE)
    unsigned long startWait = millis();
    while (!client.available() && (millis() - startWait < 20000)) {
        if (!pipelineBusy) { client.stop(); return ""; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!client.available()) {
        Serial.println("[NET] Hết thời gian chờ phản hồi từ AI!");
        client.stop();
        return "";
    }

    String responseBody = "";
    startWait = millis();
    while ((client.connected() || client.available()) && (millis() - startWait < 12000)) {
        if (!pipelineBusy) break;
        if (client.available()) {
            responseBody += (char)client.read();
            startWait = millis();
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    client.stop();

    // 7. Parse JSON: candidates[0].content.parts[*].text (gộp nhiều parts)
    String replyStr = "";
    int jsonStart = responseBody.indexOf('{');
    int jsonEnd = responseBody.lastIndexOf('}');

    if (jsonStart >= 0 && jsonEnd > jsonStart) {
        String jsonPayload = responseBody.substring(jsonStart, jsonEnd + 1);
        JsonDocument respDoc;
        DeserializationError error = deserializeJson(respDoc, jsonPayload);
        if (!error) {
            JsonArray parts = respDoc["candidates"][0]["content"]["parts"];
            for (JsonObject part : parts) {
                const char *txt = part["text"] | (const char*)nullptr;
                if (txt) replyStr += txt;
            }
        }
    }

    // 8. Fallback string search nếu JSON bị chunk cắt
    if (replyStr.length() == 0) {
        int textIdx = responseBody.indexOf("\"text\":");
        if (textIdx != -1) {
            int startQuote = responseBody.indexOf("\"", textIdx + 7);
            int endQuote = responseBody.lastIndexOf("\"");
            if (startQuote != -1 && endQuote > startQuote) {
                replyStr = responseBody.substring(startQuote + 1, endQuote);
            }
        }
    }

    replyStr.trim();
    if (replyStr.length() > 0) {
        Serial.printf("[NET] AI Trả lời: \"%s\"\n", replyStr.c_str());
        return replyStr;
    }

    Serial.println("[NET] Lỗi Parse câu trả lời từ AI!");
    Serial.println(responseBody.substring(0, 600));
    return "";
}

// ============================================================
// Core 0 task chính
// ============================================================
static void ai_net_task(void *pv) {
    bool wifiTriggered = false;

    while (true) {
        // Proactive Wi-Fi: bắt đầu kết nối NGAY khi user đang giữ nút ghi âm
        if (pipelineBusy && !dataReady && !wifiTriggered) {
            wifiTriggered = true; ensure_wifi(); continue;
        }
        if (!pipelineBusy) wifiTriggered = false;
        if (!pipelineBusy || !dataReady) {
            vTaskDelay(pdMS_TO_TICKS(50)); continue;
        }
        if (!ensure_wifi()) { pipelineBusy = false; dataReady = false; continue; }
        if (!creds_loaded) wifi_creds_refresh();
        if (cached_api_key.length() == 0) { pipelineBusy = false; dataReady = false; continue; }

        // --- Gửi ảnh + audio tới Gemini generateContent ---
        String reply = send_audio_image_to_gemini();

        // --- Phát câu trả lời: tách câu -> Google TTS từng câu -> loa ---
        if (reply.length() > 0 && pipelineBusy) {
            ttsSentenceBuf = reply;
            flush_sentences();
            flush_remaining();
        }

        // Đợi phát hết âm thanh còn trong ring buffer rồi mới kết thúc phiên
        tts_driver_wait_playback_done();
        tone_driver_stream_set_active(false);

        wifi_sleep();
        pipelineBusy = false; dataReady = false;
    }
}

// ============================================================
// Public API
// ============================================================
void ai_pipeline_start(void) {}
void ai_pipeline_stop(void) {
    if (!pipelineBusy) return;
    pipelineBusy = false; dataReady = false;
    tts_driver_stop();
    tone_driver_stream_set_active(false);
    wifi_sleep();
}
bool ai_pipeline_is_busy(void) { return pipelineBusy; }
void ai_pipeline_net_task_start(void) {
    xTaskCreatePinnedToCore(ai_net_task, "ai_net", 16384, NULL, 2, NULL, 0);
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
