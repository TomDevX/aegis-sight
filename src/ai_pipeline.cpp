#include <Arduino.h>
#include "config.h"
#include "ai_pipeline.h"
#include "tone_driver.h"
#include "tts_driver.h"
#include "secrets.h"
#include <ArduinoJson.h>

#ifdef ENABLE_AI_PIPELINE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"

// ============================================================
// HTTP helpers
// ============================================================
static bool http_read_line(WiFiClient &cl, char *buf, size_t sz, int tmoMs) {
    size_t pos = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)tmoMs) {
        if (cl.available()) {
            int c = cl.read();
            if (c < 0) { delay(1); continue; }
            if (c == '\n') { buf[pos] = 0; return true; }
            if (c != '\r' && pos < sz - 1) buf[pos++] = (char)c;
            t0 = millis();
        } else if (!cl.connected()) break;
        else delay(5);
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

static bool sse_read_line(WiFiClient &cl, String &out, int tmoMs) {
    out = "";
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)tmoMs) {
        if (!pipelineBusy) return false;
        if (cl.available()) {
            int c = cl.read();
            if (c < 0) { delay(1); continue; }
            if (c == '\n') return true;
            if (c != '\r') out += (char)c;
            t0 = millis();
        } else if (!cl.connected()) return false;
        else delay(5);
    }
    return false;
}

static bool read_sse_event(WiFiClient &cl, String &event, String &data, int tmoMs) {
    event = ""; data = "";
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)tmoMs) {
        if (!pipelineBusy) return false;
        if (cl.available()) {
            String line;
            if (!sse_read_line(cl, line, tmoMs)) return false;
            if (line.startsWith("event: ")) event = line.substring(7);
            else if (line.startsWith("data: ")) data = line.substring(6);
            if (line.length() == 0) return true;
            t0 = millis();
        } else if (!cl.connected()) return false;
        else delay(5);
    }
    return false;
}

static size_t http_read_body(WiFiClient &cl, uint8_t *out, size_t maxSz, int tmoMs) {
    size_t pos = 0;
    unsigned long t0 = millis();
    while (pos < maxSz && millis() - t0 < (unsigned long)tmoMs) {
        if (cl.available()) {
            int c = cl.read();
            if (c < 0) { delay(1); continue; }
            out[pos++] = (uint8_t)c;
            t0 = millis();
        } else if (!cl.connected()) break;
        else delay(5);
    }
    return pos;
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
// Sentence boundary
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
// Shared state
// ============================================================
static uint8_t *jpegBuf = NULL;
static size_t   jpegSize = 0;
static volatile bool dataReady = false;
static volatile bool pipelineBusy = false;

static bool alloc_buffers(void) {
    if (!jpegBuf) {
        jpegBuf = (uint8_t *)ps_malloc(64 * 1024);
        if (!jpegBuf) return false;
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
// Gemini Files API: upload JPEG → get file URI
// ============================================================
static bool upload_jpeg(WiFiClientSecure &cl, const char *key,
                        const uint8_t *jpeg, size_t jpegLen,
                        char *outUri, size_t outUriSz) {
    // Phase 1: initiate upload
    cl.stop();
    cl.setInsecure(); cl.setTimeout(10000);
    if (!cl.connect(GEMINI_API_HOST, GEMINI_API_PORT)) return false;

    String meta = "{\"file\":{\"displayName\":\"a\",\"mimeType\":\"image/jpeg\"}}";
    cl.printf("POST /upload/v1beta/files?key=%s HTTP/1.1\r\n", key);
    cl.printf("Host: %s\r\n", GEMINI_API_HOST);
    cl.printf("Content-Type: application/json\r\n");
    cl.printf("Content-Length: %zu\r\n\r\n%s", meta.length(), meta.c_str());

    char buf[512];
    if (!http_read_line(cl, buf, sizeof(buf), 5000)) return false; // status
    http_skip_headers(cl, 3000);

    size_t bodyLen = http_read_body(cl, (uint8_t *)buf, sizeof(buf) - 1, 3000);
    buf[bodyLen] = 0;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (err) return false;

    const char *uploadUrl = doc["uploadUrl"];
    const char *fileUri   = doc["file"]["uri"];
    if (!uploadUrl || !fileUri) return false;

    strncpy(outUri, fileUri, outUriSz - 1);

    // Phase 2: upload bytes to uploadUrl
    // Parse uploadUrl for host/path
    char uploadHost[128] = GEMINI_UPLOAD_HOST;
    const char *uploadPath = uploadUrl;
    if (strncmp(uploadUrl, "https://", 8) == 0) {
        uploadPath = uploadUrl + 8;
        const char *slash = strchr(uploadPath, '/');
        if (!slash) return false;
        size_t hostLen = slash - uploadPath;
        if (hostLen >= sizeof(uploadHost)) return false;
        memcpy(uploadHost, uploadPath, hostLen);
        uploadHost[hostLen] = 0;
        uploadPath = slash;
    }

    cl.stop();
    if (!cl.connect(uploadHost, GEMINI_API_PORT)) return false;

    cl.printf("PUT %s HTTP/1.1\r\n", uploadPath);
    cl.printf("Host: %s\r\n", uploadHost);
    cl.printf("Content-Type: image/jpeg\r\n");
    cl.printf("Content-Length: %zu\r\n\r\n", jpegLen);
    cl.write(jpeg, jpegLen);

    if (!http_read_line(cl, buf, sizeof(buf), 10000)) return false;
    http_skip_headers(cl, 3000);

    bodyLen = http_read_body(cl, (uint8_t *)buf, sizeof(buf) - 1, 5000);
    buf[bodyLen] = 0;

    JsonDocument doc2;
    err = deserializeJson(doc2, buf);
    if (err) return false;

    const char *finalUri = doc2["file"]["uri"];
    if (!finalUri) return false;
    strncpy(outUri, finalUri, outUriSz - 1);
    return true;
}

// ============================================================
// Core 1: Button → capture JPEG
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
            if (!pipelineBusy) {
                if (!alloc_buffers()) continue;
                pipelineBusy = true; dataReady = false; jpegSize = 0;
                camera_fb_t *fb = esp_camera_fb_get();
                if (fb) {
                    if (fb->len <= 64 * 1024) {
                        memcpy(jpegBuf, fb->buf, fb->len);
                        jpegSize = fb->len;
                    }
                    esp_camera_fb_return(fb);
                }
                if (jpegSize > 0) dataReady = true;
                else pipelineBusy = false;
            } else {
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
// Core 0: Upload JPEG → Interactions API → text → TTS
// ============================================================
static void ai_net_task(void *pv) {
    bool wifiTriggered = false;
    WiFiClientSecure cl;

    while (true) {
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

        // --- Step 1: Upload JPEG to Files API ---
        char fileUri[128] = {};
        if (jpegSize > 0) {
            upload_jpeg(cl, cached_api_key.c_str(), jpegBuf, jpegSize, fileUri, sizeof(fileUri));
        }

        // --- Step 2: Build interactions JSON ---
        JsonDocument doc;
        doc["model"] = GEMINI_MODEL_SHORT;
        doc["system_instruction"] = "Trả lời bằng tiếng Việt có dấu đầy đủ, tối đa 3 câu. KHÔNG dùng LaTeX, markdown hay ký tự đặc biệt.";
        doc["stream"] = true;

        JsonArray input = doc["input"].to<JsonArray>();
        JsonObject txtPart = input.add<JsonObject>();
        txtPart["type"] = "text";
        txtPart["text"] = "Hãy mô tả ngắn gọn những gì trong ảnh này.";

        if (fileUri[0]) {
            JsonObject imgPart = input.add<JsonObject>();
            imgPart["type"] = "image";
            imgPart["uri"] = fileUri;
            imgPart["mime_type"] = "image/jpeg";
        }

        // --- Step 3: Send interactions request (SSE) ---
        cl.stop();
        cl.setInsecure(); cl.setTimeout(15000);
        if (!cl.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
            pipelineBusy = false; dataReady = false; continue;
        }
        cl.setNoDelay(true);

        size_t jsonLen = measureJson(doc);
        cl.printf("POST /v1beta/interactions?alt=sse HTTP/1.1\r\n");
        cl.printf("Host: %s\r\n", GEMINI_API_HOST);
        cl.printf("X-Goog-Api-Key: %s\r\n", cached_api_key.c_str());
        cl.printf("Content-Type: application/json\r\n");
        cl.printf("Content-Length: %zu\r\n\r\n", jsonLen);
        serializeJson(doc, cl);

        // --- Step 4: Read HTTP status & skip headers ---
        char buf[512];
        if (!http_read_line(cl, buf, sizeof(buf), 10000)) {
            cl.stop(); pipelineBusy = false; dataReady = false; continue;
        }
        http_skip_headers(cl, 5000);

        if (!pipelineBusy) { cl.stop(); wifi_sleep(); pipelineBusy = false; dataReady = false; continue; }

        // --- Step 5: SSE stream → incremental TTS ---
        ttsSentenceBuf = "";
        bool keepReading = true;

        while (cl.connected() && keepReading) {
            if (!pipelineBusy) { keepReading = false; break; }
            while (cl.available()) {
                String event, data;
                if (!read_sse_event(cl, event, data, 10000)) { keepReading = false; break; }

                if (data == "[DONE]") { keepReading = false; break; }

                if (event == "interaction.completed" || event == "done") {
                    flush_remaining();
                    keepReading = false; break;
                }

                if (event == "step.stop") {
                    flush_remaining();
                    break;
                }

                if (event == "step.delta" && data.length() > 0) {
                    JsonDocument chunk;
                    DeserializationError err = deserializeJson(chunk, data);
                    if (err) continue;

                    const char *text = chunk["delta"]["text"];
                    if (!text) continue;

                    Serial.print(text);
                    Serial.flush();

                    ttsSentenceBuf += text;
                    flush_sentences();

                    if (!pipelineBusy) { keepReading = false; break; }
                }
            }
            if (!pipelineBusy) { keepReading = false; break; }
            if (cl.available() == 0 && keepReading) vTaskDelay(pdMS_TO_TICKS(5));
        }
        cl.stop();

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
    xTaskCreatePinnedToCore(ai_audio_task, "ai_audio", 4096, NULL, 4, NULL, 1);
}

#else
void ai_pipeline_start(void) {}
void ai_pipeline_stop(void) {}
bool ai_pipeline_is_busy(void) { return false; }
void ai_pipeline_net_task_start(void) {}
void ai_pipeline_audio_task_start(void) {}
#endif
