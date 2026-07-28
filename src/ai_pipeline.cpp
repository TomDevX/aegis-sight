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
// Base64
// ============================================================
static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64enc(const uint8_t *d, size_t n, char *out) {
    size_t p = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = ((uint32_t)d[i] << 16) | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out[p++] = b64c[(v >> 18) & 0x3F];
        out[p++] = b64c[(v >> 12) & 0x3F];
        out[p++] = i + 1 < n ? b64c[(v >> 6) & 0x3F] : '=';
        out[p++] = i + 2 < n ? b64c[v & 0x3F] : '=';
    }
    out[p] = '\0';
}

// ============================================================
// Text normalization (from api_test)
// ============================================================
static String normalize_text(const String &text) {
    String t = text;
    t.replace("\\times", " nhân ");
    t.replace("\\text{", "");
    t.replace("}", "");
    t.replace("\\", "");
    t.replace("$", "");
    t.replace("**", "");
    t.replace("*", "");
    t.replace("10^9", " tỷ");
    t.replace("10^6", " triệu");
    t.replace("^", " mũ ");
    t.replace("ESP32-S3", "E ét pi 32 ét ba");
    t.replace("ESP32", "E ét pi 32");
    t.replace("WiFi", "Wai fai");
    t.replace("I2S", "I hai S");
    t.replace("I2C", "I hai C");
    t.replace("API", "A P I");
    t.replace("AI", "A I");
    return t;
}

// ============================================================
// Sentence boundary
// ============================================================
static bool is_sentence_end(const String &buf, size_t i) {
    char c = buf[i];
    if (c == '?' || c == '!' || c == '\n') return true;
    if (c == '.') {
        bool prevIsDigit = (i > 0 && isdigit(buf[i-1]));
        bool nextIsDigit = (i + 1 < buf.length() && isdigit(buf[i+1]));
        return !(prevIsDigit && nextIsDigit);
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
        if (!jpegBuf) { Serial.println("[AI] jpegBuf ps_malloc failed"); return false; }
    }
    return true;
}

// ============================================================
// WiFi: fast reconnect
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
        for (int i = 0; i < 3 && !ok; i++) {
            if (cached_nets[i].ssid == cached_last_ssid)
                ok = try_ssid(cached_last_ssid.c_str(), cached_nets[i].pass.c_str(), 8);
        }
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

static void wifi_sleep(void) {
    WiFi.setSleep(true);
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

                pipelineBusy = true;
                dataReady = false;
                jpegSize = 0;

                camera_fb_t *fb = esp_camera_fb_get();
                if (fb) {
                    if (fb->len <= 64 * 1024) {
                        memcpy(jpegBuf, fb->buf, fb->len);
                        jpegSize = fb->len;
                    }
                    esp_camera_fb_return(fb);
                }

                if (jpegSize > 0) {
                    dataReady = true;
                } else {
                    pipelineBusy = false;
                }
            } else {
                tts_driver_stop();
                tone_driver_stream_set_active(false);
                pipelineBusy = false;
                dataReady = false;
            }
        }

        lastBtn = btn;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================
// Core 0: Gemini SSE → text → normalize → TTS per sentence
// ============================================================
static bool read_sse_line(WiFiClientSecure &client, String &out, int timeoutMs) {
    out = "";
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)timeoutMs) {
        if (client.available()) {
            int c = client.read();
            if (c < 0) { delay(1); continue; }
            if (c == '\n') return true;
            if (c != '\r') out += (char)c;
            t0 = millis();
        } else if (!client.connected()) {
            return false;
        } else {
            delay(5);
        }
    }
    return false;
}

static void ai_net_task(void *pv) {
    bool wifiTriggered = false;

    while (true) {
        if (pipelineBusy && !dataReady && !wifiTriggered) {
            wifiTriggered = true;
            ensure_wifi();
            continue;
        }
        if (!pipelineBusy) wifiTriggered = false;

        if (!pipelineBusy || !dataReady) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!ensure_wifi()) {
            pipelineBusy = false; dataReady = false; continue;
        }
        if (!creds_loaded) wifi_creds_refresh();
        if (cached_api_key.length() == 0) {
            pipelineBusy = false; dataReady = false; continue;
        }

        // --- Build JSON ---
        JsonDocument doc;
        JsonObject sysInst = doc["systemInstruction"].to<JsonObject>();
        JsonArray sysParts = sysInst["parts"].to<JsonArray>();
        sysParts[0]["text"] = "Trả lời bằng tiếng Việt có dấu đầy đủ, tối đa 3 câu. KHÔNG dùng LaTeX, markdown hay ký tự đặc biệt.";

        JsonArray contents = doc["contents"].to<JsonArray>();
        JsonObject content = contents.add<JsonObject>();
        content["role"] = "user";
        JsonArray parts = content["parts"].to<JsonArray>();
        parts[0]["text"] = "Hãy mô tả ngắn gọn những gì trong ảnh này.";

        if (jpegSize > 0) {
            size_t b64len = ((jpegSize + 2) / 3) * 4 + 1;
            char *b64 = (char *)ps_malloc(b64len);
            if (b64) {
                b64enc(jpegBuf, jpegSize, b64);
                JsonObject img = parts.add<JsonObject>();
                img["inlineData"]["mimeType"] = "image/jpeg";
                img["inlineData"]["data"] = b64;
                free(b64);
            }
        }

        size_t jsonLen = measureJson(doc);

        // --- Gemini SSL ---
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(10000);

        if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
            pipelineBusy = false; dataReady = false; continue;
        }
        client.setNoDelay(true);

        String path = "/v1beta/" + String(GEMINI_MODEL) + ":streamGenerateContent?alt=sse&key=" + cached_api_key;

        String hdr = "POST " + path + " HTTP/1.1\r\n"
                     "Host: " + GEMINI_API_HOST + "\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + String(jsonLen) + "\r\n"
                     "Connection: close\r\n\r\n";
        client.print(hdr);
        serializeJson(doc, client);

        // --- Read HTTP status ---
        String statusLine;
        read_sse_line(client, statusLine, 5000);
        statusLine.trim();

        if (!statusLine.startsWith("HTTP/1.1 200") && !statusLine.startsWith("HTTP/1.0 200")) {
            client.stop(); pipelineBusy = false; dataReady = false; continue;
        }

        // Discard headers
        while (client.connected()) {
            String l;
            if (!read_sse_line(client, l, 3000)) break;
            l.trim();
            if (l.length() == 0) break;
        }

        // --- SSE stream: accumulate text ---
        String fullText;
        bool streamDone = false;

        while (client.connected() && !streamDone) {
            while (client.available()) {
                String line;
                if (!read_sse_line(client, line, 3000)) break;
                line.trim();
                if (!line.startsWith("data: ")) continue;

                String data = line.substring(6);
                if (data == "[DONE]") { streamDone = true; break; }

                JsonDocument chunk;
                DeserializationError err = deserializeJson(chunk, data);
                if (err) continue;

                JsonArray cands = chunk["candidates"].as<JsonArray>();
                if (cands.isNull()) continue;

                const char *finish = cands[0]["finishReason"];
                if (finish && strcmp(finish, "STOP") == 0) { streamDone = true; break; }

                const char *text = cands[0]["content"]["parts"][0]["text"];
                if (text) fullText += text;
            }
            if (!client.available()) delay(5);
        }

        client.stop(); // Free TLS RAM before TTS

        // --- Normalize → split sentences → TTS each ---
        if (fullText.length() > 0) {
            String norm = normalize_text(fullText);

            int start = 0;
            for (int i = 0; i <= (int)norm.length() && pipelineBusy; i++) {
                if (i == (int)norm.length() || is_sentence_end(norm, i)) {
                    int end = (i == (int)norm.length()) ? i : i + 1;
                    String sentence = norm.substring(start, end);
                    sentence.trim();
                    if (sentence.length() > 0) {
                        tts_driver_speak(sentence.c_str(), sentence.length());
                        tts_driver_wait_playback_done();
                        if (!pipelineBusy) break; // cancelled
                    }
                    start = i + 1;
                }
            }
        }

        wifi_sleep();
        pipelineBusy = false;
        dataReady = false;
    }
}

// ============================================================
// Public API
// ============================================================
void ai_pipeline_start(void) {}
void ai_pipeline_stop(void) {
    if (!pipelineBusy) return;
    pipelineBusy = false;
    dataReady = false;
    tts_driver_stop();
    tone_driver_stream_set_active(false);
    wifi_sleep();
}

bool ai_pipeline_is_busy(void) { return pipelineBusy; }

void ai_pipeline_net_task_start(void) {
    xTaskCreatePinnedToCore(ai_net_task, "ai_net", 16384,
                            NULL, 2, NULL, 0);
}

void ai_pipeline_audio_task_start(void) {
    xTaskCreatePinnedToCore(ai_audio_task, "ai_audio", 4096,
                            NULL, 4, NULL, 1);
}

#else
void ai_pipeline_start(void) {}
void ai_pipeline_stop(void) {}
bool ai_pipeline_is_busy(void) { return false; }
void ai_pipeline_net_task_start(void) {}
void ai_pipeline_audio_task_start(void) {}
#endif
