#include <Arduino.h>
#include "config.h"

#ifdef ENABLE_MIC_AI_TEST

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include "driver/i2s.h"
#include "secrets.h"
#include "config_portal.h"
#include "tone_driver.h"
#include "tts_driver.h"
#include "mbedtls/base64.h"

// ============================================================
// Mic -> Gemini (Interactions API) -> TTS -> Speaker
// Flow (runs once on boot):
//   1. Record 3s of mic audio (16kHz mono 16-bit, PSRAM)
//   2. Wrap into WAV container (Gemini needs audio/wav)
//   3. Upload via Gemini Files API -> file URI
//   4. POST /v1beta/interactions?alt=sse with text + audio parts
//   5. SSE stream -> TEXT response -> Google TTS -> speaker
// ============================================================

#define MIC_SAMPLE_RATE    16000
#define MIC_READ_CHUNK     4096
#define MIC_AI_RECORD_MS   3000   // test ghi âm 3s (thay vì 8s)

#define UPLOAD_MIME_TYPE   "audio/wav"
#define UPLOAD_DISPLAY     "question"

// ============================================================
// HTTP helpers (identical to api_test.cpp)
// ============================================================
static bool http_read_line(WiFiClientSecure &cl, String &out, int tmoMs) {
    out = "";
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)tmoMs) {
        if (cl.available()) {
            int c = cl.read();
            if (c < 0) { delay(1); continue; }
            if (c == '\n') return true;
            if (c != '\r') out += (char)c;
            t0 = millis();
        } else if (!cl.connected()) { return false; }
        else { delay(5); }
    }
    return false;
}

static bool http_skip_headers(WiFiClientSecure &cl, int tmoMs) {
    String l;
    while (true) {
        if (!http_read_line(cl, l, tmoMs)) return false;
        if (l.length() == 0) return true;
    }
}

static bool http_read_body(WiFiClientSecure &cl, uint8_t *out, size_t maxSz, int tmoMs) {
    size_t pos = 0;
    unsigned long t0 = millis();
    while (pos < maxSz && millis() - t0 < (unsigned long)tmoMs) {
        if (cl.available()) {
            int c = cl.read();
            if (c < 0) { delay(1); continue; }
            out[pos++] = (uint8_t)c;
            t0 = millis();
        } else if (!cl.connected()) { break; }
        else { delay(5); }
    }
    return pos;
}

// ============================================================
// Text Normalization (identical to api_test.cpp)
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
// Sentence boundary (identical to api_test.cpp)
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

// ============================================================
// Streaming buffer: accumulate text, speak complete sentences
// ============================================================
static String ttsSentenceBuf;
static bool ttsAudioMode = false;

static void flush_sentences(void) {
    if (ttsAudioMode) { ttsSentenceBuf = ""; return; }
    int lastBoundary = -1;
    for (int i = 0; i < (int)ttsSentenceBuf.length(); i++) {
        if (is_sentence_end(ttsSentenceBuf, i)) lastBoundary = i;
    }
    if (lastBoundary >= 0) {
        String complete = ttsSentenceBuf.substring(0, lastBoundary + 1);
        complete.trim();
        if (complete.length() > 0) {
            String norm = normalize_text(complete);
            tts_driver_speak(norm.c_str(), norm.length());
        }
        ttsSentenceBuf = ttsSentenceBuf.substring(lastBoundary + 1);
    }
}

static void flush_remaining(void) {
    if (ttsAudioMode) { ttsSentenceBuf = ""; return; }
    ttsSentenceBuf.trim();
    if (ttsSentenceBuf.length() > 0) {
        String norm = normalize_text(ttsSentenceBuf);
        tts_driver_speak(norm.c_str(), norm.length());
    }
    ttsSentenceBuf = "";
}

// ============================================================
// Read SSE event into caller-provided buffers (PSRAM recommended).
// Audio payloads are large base64 blobs, so we must NOT use String/JsonDocument here.
// ============================================================
static bool read_sse_event_raw(WiFiClientSecure &cl, char *evBuf, size_t evCap,
                               char *dtBuf, size_t dtCap, int tmoMs) {
    evBuf[0] = '\0'; dtBuf[0] = '\0';
    size_t dtPos = 0;
    bool inEvent = false;
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)tmoMs) {
        if (cl.available()) {
            int c = cl.read();
            if (c < 0) { delay(1); continue; }
            t0 = millis();
            if (c == '\n') {
                size_t len = dtPos;
                while (len > 0 && (dtBuf[len - 1] == '\r' || dtBuf[len - 1] == ' ')) len--;
                dtBuf[len] = '\0';
                if (len == 0 && inEvent) { dtPos = 0; return true; }   // blank line = end of event
                if (dtBuf[0] == ':' || len == 0) { dtPos = 0; dtBuf[0] = '\0'; continue; } // SSE comment
                if (strncmp(dtBuf, "event:", 6) == 0) {
                    const char *ev = dtBuf + 6;
                    while (*ev == ' ') ev++;
                    size_t n = strlen(ev); if (n >= evCap) n = evCap - 1;
                    memcpy(evBuf, ev, n); evBuf[n] = '\0';
                } else if (strncmp(dtBuf, "data:", 5) == 0) {
                    const char *d = dtBuf + 5;
                    while (*d == ' ') d++;
                    size_t n = strlen(d); if (n >= dtCap) n = dtCap - 1;
                    memmove(dtBuf, d, n);
                    dtBuf[n] = '\0';
                    inEvent = true;
} else {
                    // Bare payload line (some servers omit "data:" prefix),
                    // but skip pure-hex framed lines ("165", "0") of alt=sse transfer chunking
                    bool allHex = len > 0 && len <= 8;
                    for (size_t i = 0; allHex && i < len; i++) {
                        char c = dtBuf[i];
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) allHex = false;
                    }
                    if (allHex) { dtPos = 0; dtBuf[0] = '\0'; continue; }
                    size_t n = len; if (n >= dtCap) n = dtCap - 1;
                    dtBuf[n] = '\0';
                    inEvent = true;
                }
                dtPos = 0;
                continue;
            }
            if (dtPos < dtCap - 1) dtBuf[dtPos++] = (char)c;
        } else if (!cl.connected()) { return false; }
        else { delay(5); }
    }
    return false;
}

// ============================================================
// Mic recording (I2S RX, 16kHz mono 16-bit) into PSRAM
// ============================================================
static bool init_mic(void) {
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 512,
        .use_apll = false,
    };

    i2s_pin_config_t pin_cfg = {
        .bck_io_num = MIC_BCLK,
        .ws_io_num = MIC_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_DATA_IN,
    };

    esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &i2s_cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[MIC_AI] i2s_driver_install failed: 0x%x\n", err);
        return false;
    }
    err = i2s_set_pin(I2S_MIC_PORT, &pin_cfg);
    if (err != ESP_OK) {
        Serial.printf("[MIC_AI] i2s_set_pin failed: 0x%x\n", err);
        i2s_driver_uninstall(I2S_MIC_PORT);
        return false;
    }
    return true;
}

// ============================================================
// WAV wrapper: prepend RIFF header to raw PCM
// ============================================================
static void write_wav_header(uint8_t *buf, uint32_t pcmBytes) {
    uint32_t dataLen = pcmBytes;
    uint32_t riffLen = 36 + dataLen;
    uint16_t blockAlign = 2;   // 16-bit mono
    uint32_t byteRate = MIC_SAMPLE_RATE * blockAlign;

    memcpy(buf + 0, "RIFF", 4);
    buf[4] = riffLen & 0xFF; buf[5] = (riffLen >> 8) & 0xFF;
    buf[6] = (riffLen >> 16) & 0xFF; buf[7] = (riffLen >> 24) & 0xFF;
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    buf[16] = 16; buf[17] = 0; buf[18] = 0; buf[19] = 0;          // fmt chunk size
    buf[20] = 1; buf[21] = 0;                                      // PCM format
    buf[22] = 1; buf[23] = 0;                                      // 1 channel
    buf[24] = MIC_SAMPLE_RATE & 0xFF; buf[25] = (MIC_SAMPLE_RATE >> 8) & 0xFF;
    buf[26] = (MIC_SAMPLE_RATE >> 16) & 0xFF; buf[27] = (MIC_SAMPLE_RATE >> 24) & 0xFF;
    buf[28] = byteRate & 0xFF; buf[29] = (byteRate >> 8) & 0xFF;
    buf[30] = (byteRate >> 16) & 0xFF; buf[31] = (byteRate >> 24) & 0xFF;
    buf[32] = blockAlign; buf[33] = 0;
    buf[34] = 16; buf[35] = 0;                                      // bits per sample
    memcpy(buf + 36, "data", 4);
    buf[40] = dataLen & 0xFF; buf[41] = (dataLen >> 8) & 0xFF;
    buf[42] = (dataLen >> 16) & 0xFF; buf[43] = (dataLen >> 24) & 0xFF;
}

static int16_t *g_pcm = NULL;

static size_t record_audio(void) {
    uint32_t totalSamples = (uint32_t)MIC_SAMPLE_RATE * MIC_AI_RECORD_MS / 1000;
    g_pcm = (int16_t *)ps_malloc(totalSamples * sizeof(int16_t));
    if (!g_pcm) {
        Serial.println("[MIC_AI] ps_malloc failed for PCM");
        return 0;
    }

    int32_t *raw32 = (int32_t *)ps_malloc(MIC_READ_CHUNK * sizeof(int32_t));
    if (!raw32) {
        Serial.println("[MIC_AI] ps_malloc failed for raw32 chunk");
        free(g_pcm); g_pcm = NULL;
        return 0;
    }

    Serial.printf("[MIC_AI] Recording %d s (%u samples) ...\n", MIC_AI_RECORD_MS / 1000, totalSamples);
    uint32_t recorded = 0;
    unsigned long t0 = millis();

    while (recorded < totalSamples) {
        size_t chunk = (totalSamples - recorded) < MIC_READ_CHUNK ? (totalSamples - recorded) : MIC_READ_CHUNK;
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_MIC_PORT, raw32, chunk * sizeof(int32_t),
                                 &bytesRead, pdMS_TO_TICKS(500));
        if (err != ESP_OK) { Serial.printf("[MIC_AI] i2s_read err 0x%x\n", err); break; }

        size_t samplesRead = bytesRead / sizeof(int32_t);
        for (size_t i = 0; i < samplesRead; i++) {
            // INMP441: 24-bit left-justified in 32-bit slot -> take top 16 bits.
            // Do NOT shift left by 1 (overflows into sign bit and garbles loud peaks).
            g_pcm[recorded + i] = (int16_t)(raw32[i] >> 16);
        }
        recorded += samplesRead;

        if (millis() - t0 >= 500) {
            Serial.printf("[MIC_AI] %u/%u samples\n", recorded, totalSamples);
            t0 = millis();
        }
    }
    free(raw32);

    // ---- Speech conditioning: rumble/hiss removal + noise gate + gentle AGC ----
    // 2nd-order Butterworth HPF @300Hz, 16kHz: removes DC + low rumble (thunder/drill)
    const float b0 = 0.92005f, b1 = -1.84011f, b2 = 0.92005f;
    const float a1 = -1.83370f, a2 = 0.84652f;
    float hpX1 = 0, hpX2 = 0, hpY1 = 0, hpY2 = 0;
    // 1-pole LPF @3400Hz: removes hiss/static
    const float dt = 1.0f / MIC_SAMPLE_RATE;
    const float rcL = 1.0f / (2.0f * M_PI * 3400.0f);
    const float lpA = dt / (rcL + dt);
    float lpOut = 0;
    // Noise gate: duck low-level noise floor between words
    const float gateThresh = 150.0f;
    float env = 0;

    for (size_t i = 0; i < recorded; i++) {
        float x = (float)g_pcm[i];

        float y = b0 * x + b1 * hpX1 + b2 * hpX2 - a1 * hpY1 - a2 * hpY2;
        hpX2 = hpX1; hpX1 = x; hpY2 = hpY1; hpY1 = y;

        lpOut += lpA * (y - lpOut);

        float a = fabsf(lpOut);
        env = 0.95f * env + 0.05f * a;
        float out = (env < gateThresh) ? lpOut * 0.05f : lpOut;

        if (out > 32767.0f) out = 32767.0f;
        else if (out < -32768.0f) out = -32768.0f;
        g_pcm[i] = (int16_t)out;
    }

    // Diagnostic stats after filtering
    int16_t minVal = 32767, maxVal = -32768;
    double sumSq = 0;
    size_t voiced = 0;
    for (size_t i = 0; i < recorded; i++) {
        int16_t s = g_pcm[i];
        if (s < minVal) minVal = s;
        if (s > maxVal) maxVal = s;
        sumSq += (double)s * s;
        if (abs(s) > (int)gateThresh) voiced++;
    }
    float rms = sqrt(sumSq / recorded);
    int peak = (maxVal > -minVal) ? maxVal : -minVal;

    // Gentle AGC: peak to ~60% full scale, boost capped 4x, never boost hot signal
    const int targetPeak = 20000;
    float gain = 1.0f;
    if (peak > 0) {
        gain = (float)targetPeak / peak;
        if (gain > 4.0f) gain = 4.0f;
        else if (gain < 0.5f) gain = 0.5f;
    }
    for (size_t i = 0; i < recorded; i++) {
        int32_t v = (int32_t)(g_pcm[i] * gain);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        g_pcm[i] = (int16_t)v;
    }

    sumSq = 0;
    for (size_t i = 0; i < recorded; i++) sumSq += (double)g_pcm[i] * g_pcm[i];
    float rmsAfter = sqrt(sumSq / recorded);

    Serial.printf("[MIC_AI] Captured %u samples (%.2f s)\n", recorded, (float)recorded / MIC_SAMPLE_RATE);
    Serial.printf("[MIC_AI] Filtered: Min=%d Max=%d RMS=%.1f Peak=%d Voice=%u%%\n",
                  minVal, maxVal, rms, peak, (unsigned)(voiced * 100 / recorded));
    Serial.printf("[MIC_AI] AGC gain=%.2f -> RMS=%.1f\n", gain, rmsAfter);
    return recorded;
}


// ============================================================
// Gemini API — inline Base64 audio stream -> TTS to speaker
// ============================================================
static bool stream_gemini_audio(const char *apiKey, const uint8_t *wav, size_t wavLen) {
    // 1. Base64 encode WAV on PSRAM
    size_t b64Cap = 0;
    mbedtls_base64_encode(NULL, 0, &b64Cap, (const unsigned char*)wav, wavLen);
    char *b64 = (char *)ps_malloc(b64Cap + 1);
    if (!b64) {
        Serial.println("[GEMINI] ps_malloc failed for Base64 audio");
        return false;
    }
    size_t b64Len = 0;
    mbedtls_base64_encode((unsigned char*)b64, b64Cap + 1, &b64Len, (const unsigned char*)wav, wavLen);
    b64[b64Len] = '\0';
    Serial.printf("[GEMINI] Base64 audio encoded: %zu bytes\n", b64Len);

    // 2. Prepare JSON payload parts (TEXT response modality -> Google TTS -> speaker)
    String jsonPrefix = "{\"systemInstruction\":{\"parts\":[{\"text\":\"Bạn là chuyên gia hỗ trợ người mù. Người dùng đã gửi câu hỏi bằng giọng nói trong audio. Hãy nghe câu hỏi và trả lời ngắn gọn bằng tiếng Việt. KHÔNG dùng LaTeX, markdown hay ký tự đặc biệt.\"}]},\"generationConfig\":{\"responseModalities\":[\"TEXT\"],\"thinkingConfig\":{\"thinkingBudget\":0}},\"contents\":[{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
    String jsonSuffix = "\"}},{\"text\":\"Hãy trả lời câu hỏi trong audio này.\"}]}]}";

    size_t contentLength = jsonPrefix.length() + b64Len + jsonSuffix.length();

    // 3. Connect HTTPS to Gemini
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    Serial.print("[GEMINI] Connecting...");
    if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
        Serial.println(" FAIL");
        free(b64);
        return false;
    }
    Serial.println(" OK");

    // 4. Send HTTP POST request with inline audio
    client.printf("POST /v1beta/%s:streamGenerateContent?alt=sse&key=%s HTTP/1.1\r\n", GEMINI_MODEL, apiKey);
    client.printf("Host: %s\r\n", GEMINI_API_HOST);
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %zu\r\n\r\n", contentLength);

    client.print(jsonPrefix);

    // Stream base64 audio in 2048-byte chunks to prevent TCP socket overflow
    size_t offset = 0;
    while (offset < b64Len) {
        size_t chunkSize = (b64Len - offset > 2048) ? 2048 : (b64Len - offset);
        size_t written = client.write((const uint8_t *)(b64 + offset), chunkSize);
        if (written == 0) {
            Serial.printf("\n[GEMINI] Write stalled at offset %zu\n", offset);
            break;
        }
        offset += written;
        vTaskDelay(1);
    }

    client.print(jsonSuffix);
    client.flush(); // Force flush SSL output buffer to Gemini server
    free(b64); // Free base64 RAM immediately after sending

    // 5. Read HTTP status line
    String statusLine;
    if (!http_read_line(client, statusLine, 20000)) {
        Serial.println("[GEMINI] No HTTP response (timeout)");
        client.stop();
        return false;
    }
    statusLine.trim();
    Serial.printf("[GEMINI] %s\n", statusLine.c_str());
    if (!statusLine.startsWith("HTTP/1.1 200") && !statusLine.startsWith("HTTP/1.0 200")) {
        http_skip_headers(client, 3000);
        Serial.print("[GEMINI ERR BODY] ");
        while (client.available()) Serial.write(client.read());
        Serial.println();
        client.stop();
        return false;
    }

    // 6. Skip HTTP response headers
    http_skip_headers(client, 5000);

    // 7. Parse SSE stream -> in text câu trả lời + đưa qua TTS -> speaker
    Serial.print("[GEMINI] Answer: ");
    ttsSentenceBuf = "";
    ttsAudioMode = false;
    bool gotText = false;
    bool keepReading = true;

    const size_t DT_CAP = 1024 * 1024;              // 1MB PSRAM for base64 audio
    char *dtBuf = (char *)ps_malloc(DT_CAP);
    char evBuf[32];
    if (!dtBuf) {
        Serial.println("[GEMINI] ps_malloc failed for SSE data buffer");
        client.stop();
        return false;
    }

    int sseEvents = 0;
    bool streamFailed = false;
    while (keepReading) {
        while (client.available()) {
            if (!read_sse_event_raw(client, evBuf, sizeof(evBuf), dtBuf, DT_CAP, 10000)) {
                streamFailed = true;
                keepReading = false; break;
            }
            sseEvents++;
            if (strcmp(dtBuf, "[DONE]") == 0) { keepReading = false; break; }
            if (dtBuf[0] == 0) continue;

            if (strstr(dtBuf, "\"inlineData\"")) {
                // Không còn dùng AUDIO modality — bỏ qua nếu Gemini vẫn gửi audio
                continue;
            }
            // Text: in thẳng câu trả lời (không dump JSON)
            JsonDocument chunk;
            DeserializationError err = deserializeJson(chunk, dtBuf);
            if (err) {
                Serial.printf("\n[SSE] #%d bad-json len=%u: %.120s\n", sseEvents, (unsigned)strlen(dtBuf), dtBuf);
                continue;
            }
            if (chunk["error"].is<const char *>()) {
                Serial.printf("\n[SSE] #%d API error: %s\n", sseEvents, chunk["error"].as<const char *>());
            }
            const char *text = chunk["candidates"][0]["content"]["parts"][0]["text"];
            if (text && text[0] != 0) {
                gotText = true;
                Serial.print(text);
                Serial.flush();
                ttsSentenceBuf += text;
                flush_sentences();
            } else {
                Serial.printf("\n[SSE] #%d no-text: %.100s\n", sseEvents, dtBuf);
            }
        }
        if (!client.connected() && client.available() == 0) break;
        if (client.available() == 0 && keepReading) delay(5);
    }

    free(dtBuf);
    client.stop();
    Serial.printf("\n[GEMINI] events=%d text=%s%s\n", sseEvents, gotText ? "yes" : "no",
                  streamFailed ? " (stream bị cắt giữa chừng)" : "");
    if (!gotText) {
        Serial.println("[GEMINI] (Không nhận được câu trả lời: lỗi mạng / audio quá im. Sẽ thử lại)");
    }
    if (!ttsAudioMode) flush_remaining();
    Serial.println("[GEMINI] Done");
    return gotText;
}

// ============================================================
// Mic -> Speaker loopback (test): mic thô ra loa trực tiếp
// ============================================================
#ifdef ENABLE_MIC_LOOPBACK
static void run_mic_loopback(void *pv) {
    // Beep test: nếu loa phát -> PDM OK
    tone_driver_play(1000, 200, 21);
    vTaskDelay(pdMS_TO_TICKS(400));

    if (!init_mic()) {
        Serial.println("[LOOP] Mic init failed");
        vTaskDelete(NULL);
        return;
    }

    tone_driver_stream_init();
    tone_driver_stream_set_active(true);
    Serial.println("[LOOP] Mic -> Speaker live. Press trigger button to stop.");

    int32_t *raw32 = (int32_t *)ps_malloc(MIC_READ_CHUNK * sizeof(int32_t));
    int16_t *pcm16 = (int16_t *)ps_malloc(MIC_READ_CHUNK * sizeof(int16_t));
    if (!raw32 || !pcm16) {
        Serial.println("[LOOP] ps_malloc failed");
        vTaskDelete(NULL);
        return;
    }

    // HPF giống record_audio: bỏ DC + rumble ~200Hz
    const float dt = 1.0f / MIC_SAMPLE_RATE;
    const float rc = 1.0f / (2.0f * M_PI * 200.0f);
    const float hpA = rc / (rc + dt);
    float hpPrevIn = 0.0f, hpPrevOut = 0.0f;

    while (digitalRead(BTN_TRIGGER) == HIGH) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_MIC_PORT, raw32, MIC_READ_CHUNK * sizeof(int32_t),
                                 &bytesRead, pdMS_TO_TICKS(100));
        if (err != ESP_OK) break;

        size_t samplesRead = bytesRead / sizeof(int32_t);
        for (size_t i = 0; i < samplesRead; i++) {
            float x = (float)(raw32[i] >> 16);
            float y = hpA * (hpPrevOut + x - hpPrevIn);
            hpPrevIn = x;
            hpPrevOut = y;
            pcm16[i] = (int16_t)y;
        }

        if (!tone_driver_stream_write(pcm16, samplesRead)) {
            vTaskDelay(pdMS_TO_TICKS(2));   // ring buffer full, chờ tone task tiêu thụ
        }
    }

    tone_driver_stream_set_active(false);
    i2s_driver_uninstall(I2S_MIC_PORT);
    Serial.println("[LOOP] Stopped");
    vTaskDelete(NULL);
}
#endif

// ============================================================
// Main test task: record -> ask -> speak
// ============================================================
static char g_apiKey[128] = {};

static void run_mic_ai_test(void *pv) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[MIC_AI] No WiFi");
        vTaskDelete(NULL);
        return;
    }

    if (strlen(g_apiKey) == 0) {
        Serial.println("[MIC_AI] No API key");
        vTaskDelete(NULL);
        return;
    }

    if (!init_mic()) {
        Serial.println("[MIC_AI] Mic init failed");
        vTaskDelete(NULL);
        return;
    }

    size_t pcmSamples = record_audio();
    i2s_driver_uninstall(I2S_MIC_PORT);

    if (pcmSamples == 0) {
        Serial.println("[MIC_AI] No audio captured");
        vTaskDelete(NULL);
        return;
    }

    // Wrap PCM into WAV (header + payload)
    size_t pcmBytes = pcmSamples * sizeof(int16_t);
    size_t wavLen = 44 + pcmBytes;
    uint8_t *wav = (uint8_t *)ps_malloc(wavLen);
    if (!wav) {
        Serial.println("[MIC_AI] ps_malloc failed for WAV");
        vTaskDelete(NULL);
        return;
    }
    write_wav_header(wav, pcmBytes);
    memcpy(wav + 44, g_pcm, pcmBytes);
    Serial.printf("[MIC_AI] WAV ready: %zu bytes\n", wavLen);

    Serial.println("[MIC_AI] Streaming audio to Gemini AI ...");
    const int MAX_ATTEMPTS = 3;
    bool aiOk = false;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        if (stream_gemini_audio(g_apiKey, wav, wavLen)) { aiOk = true; break; }
        if (attempt < MAX_ATTEMPTS) {
            Serial.printf("[MIC_AI] Gemini call failed (attempt %d/%d), retrying...\n", attempt, MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!aiOk) Serial.println("[MIC_AI] Gemini failed after retries");

    tts_driver_wait_playback_done();

    free(wav);
    free(g_pcm);
    Serial.println("[MIC_AI] Done");
    vTaskDelete(NULL);
}

// ============================================================
// read_line từ Serial Console
// ============================================================
static void read_line(char *buf, size_t size) {
    size_t pos = 0;
    while (true) {
        while (!Serial.available()) delay(10);
        char c = Serial.read();
        if (c == '\n' || c == '\r') { Serial.println(); break; }
        if (c == '\b' && pos > 0) { pos--; Serial.print("\b \b"); continue; }
        if (pos < size - 1) { buf[pos++] = c; Serial.print(c); }
    }
    buf[pos] = '\0';
    delay(30);
    while (Serial.available()) Serial.read();
}

// ============================================================
// Entry points
// ============================================================
void setup() {
    Serial.begin(SERIAL_BAUD); delay(500);

    Serial.println("\n=============================================");
    Serial.println("  AEGIS SIGHT - Mic -> Gemini -> TTS Test");
    Serial.println("=============================================");

    if (!psramFound()) { Serial.println("[FAIL] PSRAM not found"); return; }
    Serial.printf("PSRAM: %zu KB free\n", ESP.getPsramSize() / 1024);

    if (!SPIFFS.begin(true)) { Serial.println("[SPIFFS] Mount Failed"); return; }
    Serial.println("[SPIFFS] OK");

    secrets_begin();
    if (!secrets_has_all()) {
        Serial.println("[INIT] No saved config.");
        Serial.print("SSID: ");
        char ssid[64] = {}, pass[64] = {};
        read_line(ssid, sizeof(ssid));
        if (strcmp(ssid, "/portal") != 0 && strlen(ssid) > 0) {
            Serial.print("Password: "); read_line(pass, sizeof(pass));
            secrets_set(SK_WIFI_SSID, String(ssid));
            secrets_set(SK_WIFI_PASS, String(pass));
            Serial.print("Gemini API Key: ");
            char key[128] = {}; read_line(key, sizeof(key));
            if (strlen(key) > 0) secrets_set(SK_GEMINI_KEY, String(key));
        }
        if (!secrets_has_all()) { Serial.println("[INIT] Starting config portal..."); config_portal_start(); }
    }

    {
        String ssid = secrets_get(SK_WIFI_SSID);
        String pass = secrets_get(SK_WIFI_PASS);
        String apiKey = secrets_get(SK_GEMINI_KEY);
        strncpy(g_apiKey, apiKey.c_str(), sizeof(g_apiKey) - 1);
        g_apiKey[sizeof(g_apiKey) - 1] = '\0';
        if (ssid.length() > 0) {
            Serial.printf("[WIFI] Connecting to %s ...\n", ssid.c_str());
            WiFi.disconnect(true);
            delay(100);
            WiFi.mode(WIFI_STA);
            WiFi.begin(ssid.c_str(), pass.c_str());
            int c = 0;
            while (WiFi.status() != WL_CONNECTED && c < 60) { delay(500); Serial.print("."); c++; }
            if (WiFi.status() == WL_CONNECTED) {
                IPAddress dns1(8, 8, 8, 8);
                IPAddress dns2(1, 1, 1, 1);
                WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
                Serial.printf("\n[WIFI] Connected: %s (DNS: 8.8.8.8)\n", WiFi.localIP().toString().c_str());
            } else {
                Serial.println("\n[WIFI] FAIL");
            }
        }
    }

    if (!tone_driver_init()) {
        Serial.println("[MIC_AI] Tone driver init failed!");
        return;
    }
    tone_driver_start_task();

#ifdef ENABLE_MIC_LOOPBACK
    tone_driver_stream_init();
    xTaskCreatePinnedToCore(run_mic_loopback, "mic_loopback", 8192, NULL, 4, NULL, 0);
    return;
#endif

    tone_driver_stream_init();
    tts_driver_init();
    Serial.println("[MIC_AI] Speaker + TTS ready");

    xTaskCreatePinnedToCore(run_mic_ai_test, "mic_ai_test", 16384, NULL, 1, NULL, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }

#endif
