#include <Arduino.h>
#include "config.h"
#include "ai_pipeline.h"
#include "tone_driver.h"
#include "secrets.h"
#include <string.h>
#include <ArduinoJson.h>

#ifdef ENABLE_AI_PIPELINE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include "driver/i2s.h"

// ============================================================
// Base64 helpers
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

static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64dec(const char *in, size_t ilen, uint8_t *out) {
    size_t o = 0;
    for (size_t i = 0; i + 3 < ilen; i += 4) {
        int a = b64v(in[i]), b = b64v(in[i + 1]), c = b64v(in[i + 2]), d = b64v(in[i + 3]);
        if (a < 0 || b < 0) break;
        out[o++] = (a << 2) | (b >> 4);
        if (c >= 0 && in[i + 2] != '=') {
            out[o++] = (b << 4) | (c >> 2);
            if (d >= 0 && in[i + 3] != '=') {
                out[o++] = (c << 6) | d;
            }
        }
    }
    return o;
}

// ============================================================
// WAV header builder
// ============================================================
static void make_wav_hdr(uint8_t *hdr, uint32_t databytes, uint32_t sr) {
    uint32_t fs = 36 + databytes;
    uint32_t br = sr * 2;
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &fs, 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t subchunk = 16; memcpy(hdr + 16, &subchunk, 4);
    uint16_t fmt = 1;       memcpy(hdr + 20, &fmt, 2);
    uint16_t ch = 1;        memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &sr, 4);
    memcpy(hdr + 28, &br, 4);
    uint16_t ba = 2;        memcpy(hdr + 32, &ba, 2);
    uint16_t bps = 16;      memcpy(hdr + 34, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &databytes, 4);
}

// ============================================================
// Shared buffers (PSRAM) — dynamic up to 8s
// ============================================================
static int16_t *recAudio = NULL;
static uint8_t *jpegBuf = NULL;
static size_t   jpegSize = 0;
static volatile bool dataReady = false;
static volatile size_t recordPos = 0;  // actual samples recorded

static bool alloc_buffers(void) {
    if (!recAudio) {
        size_t maxBytes = AI_AUDIO_MAX_SAMPLES * sizeof(int16_t) + 44;
        recAudio = (int16_t *)ps_malloc(maxBytes);
        if (!recAudio) { Serial.println("[AI] ps_malloc recAudio failed"); return false; }
    }
    if (!jpegBuf) {
        jpegBuf = (uint8_t *)ps_malloc(64 * 1024);
        if (!jpegBuf) { Serial.println("[AI] ps_malloc jpegBuf failed"); return false; }
    }
    return true;
}

// ============================================================
// Global state
// ============================================================
static volatile bool pipelineBusy = false;

// ============================================================
// Wi‑Fi: fast reconnect (last AP, ~2s) → fallback scan all
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
    while (WiFi.status() != WL_CONNECTED && c < tries && pipelineBusy) {
        vTaskDelay(pdMS_TO_TICKS(250)); c++;
    }
    return WiFi.status() == WL_CONNECTED;
}

static bool ensure_wifi(void) {
    if (WiFi.status() == WL_CONNECTED) return true;

    if (!creds_loaded) wifi_creds_refresh();

    if (cached_nets[0].ssid.length() == 0) {
        Serial.println("[WIFI] No SSID saved");
        return false;
    }

    WiFi.mode(WIFI_STA);
    bool ok = false;

    // Fast path: try last-connected SSID first (2s)
    if (cached_last_ssid.length()) {
        for (int i = 0; i < 3 && !ok; i++) {
            if (cached_nets[i].ssid == cached_last_ssid) {
                ok = try_ssid(cached_last_ssid.c_str(), cached_nets[i].pass.c_str(), 8);
            }
        }
    }

    // Fallback: try each known network sequentially (6s each)
    for (int i = 0; i < 3 && !ok; i++) {
        if (cached_nets[i].ssid.length() == 0) continue;
        ok = try_ssid(cached_nets[i].ssid.c_str(), cached_nets[i].pass.c_str(), 24);
    }

    if (!ok) {
        Serial.println("[WIFI] All networks failed");
        WiFi.disconnect(true);
        return false;
    }

    Serial.printf("[WIFI] Connected to %s\n", WiFi.SSID().c_str());
    cached_last_ssid = WiFi.SSID();
    secrets_set(SK_LAST_SSID, WiFi.SSID());
    return true;
}

static void wifi_shutdown(void) {
    WiFi.setSleep(true);
}

// ============================================================
// Core 1 Task: Hold-to-Talk recording + button monitor
// ============================================================
enum { AI_STATE_IDLE, AI_STATE_RECORDING, AI_STATE_WAITING };

static void ai_audio_task(void *pvParameters) {
    int16_t *chunk = (int16_t *)ps_malloc(512 * sizeof(int16_t));
    if (!chunk) { vTaskDelete(NULL); return; }

    uint8_t state = AI_STATE_IDLE;
    uint32_t lastBtn = HIGH;
    bool micOwned = false;
    uint32_t recordStartMs = 0;

    while (true) {
        uint32_t b = digitalRead(BTN_TRIGGER);

        switch (state) {

        case AI_STATE_IDLE:
            if (b == LOW && lastBtn == HIGH) {
                if (alloc_buffers()) {
                    pipelineBusy = true;
                    dataReady = false;
                    recordPos = 0;

                    // Capture JPEG immediately on button press
                    jpegSize = 0;
                    camera_fb_t *fb = esp_camera_fb_get();
                    if (fb) {
                        if (fb->len <= 64 * 1024) {
                            memcpy(jpegBuf, fb->buf, fb->len);
                            jpegSize = fb->len;
                            Serial.printf("[AI_AUDIO] JPEG %zu bytes\n", jpegSize);
                        }
                        esp_camera_fb_return(fb);
                    }

                    recordStartMs = millis();
                    state = AI_STATE_RECORDING;
                    Serial.println("[AI_AUDIO] Hold-to-Talk: recording...");
                }
            }
            break;

        case AI_STATE_RECORDING: {
            if (!micOwned) {
                i2s_config_t ic = {
                    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
                    .sample_rate = AI_AUDIO_SAMPLE_RATE,
                    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
                    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
                    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
                    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
                    .dma_buf_count = 4, .dma_buf_len = 256, .use_apll = false,
                };
                i2s_pin_config_t pc = { .bck_io_num = MIC_BCLK, .ws_io_num = MIC_LRCK,
                    .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = MIC_DATA_IN };

                if (i2s_driver_install(I2S_MIC_PORT, &ic, 0, NULL) == ESP_OK &&
                    i2s_set_pin(I2S_MIC_PORT, &pc) == ESP_OK) {
                    micOwned = true;
                } else {
                    pipelineBusy = false; state = AI_STATE_IDLE; break;
                }
                // Flush stale DMA data
                int16_t flush[256];
                size_t flushed = 0;
                i2s_read(I2S_MIC_PORT, flush, sizeof(flush), &flushed, pdMS_TO_TICKS(20));
            }

            bool stopBtn  = (b == HIGH && lastBtn == LOW);
            bool stopTime = (millis() - recordStartMs) >= AI_AUDIO_MAX_RECORD_MS;

            if (!stopBtn && !stopTime) {
                size_t br = 0;
                size_t maxSamp = AI_AUDIO_MAX_SAMPLES;
                size_t want = (maxSamp - recordPos < 512) ? (maxSamp - recordPos) : 512;
                if (want > 0) {
                    i2s_read(I2S_MIC_PORT, chunk, want * sizeof(int16_t), &br, pdMS_TO_TICKS(50));
                    size_t s = br / 2;
                    if (s > 0) {
                        memcpy(recAudio + recordPos, chunk, s * 2);
                        recordPos += s;
                    }
                }
                if (recordPos >= maxSamp) stopTime = true;
            }

            if (stopBtn || stopTime) {
                if (micOwned) {
                    i2s_driver_uninstall(I2S_MIC_PORT);
                    micOwned = false;
                }

                if (recordPos == 0) {
                    Serial.println("[AI_AUDIO] Nothing recorded — discarding");
                    pipelineBusy = false;
                    state = AI_STATE_IDLE;
                    break;
                }

                Serial.printf("[AI_AUDIO] Recorded %zu samples (%.1fs)\n",
                              recordPos, (float)recordPos / AI_AUDIO_SAMPLE_RATE);

                dataReady = true;
                state = AI_STATE_WAITING;
                Serial.println("[AI_AUDIO] Data ready for HTTP request");
            }
            break;
        }

        case AI_STATE_WAITING:
            if (b == LOW && lastBtn == HIGH) {
                Serial.println("[AI_AUDIO] Cancel — button pressed");
                ai_pipeline_stop();
            }
            if (!pipelineBusy) {
                state = AI_STATE_IDLE;
                Serial.println("[AI_AUDIO] Returning to idle");
            }
            break;
        }

        lastBtn = b;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
// Build JSON request body
// ============================================================
static String build_request(int16_t *pcm, size_t pcmSamp, const uint8_t *jpeg, size_t jpegLen) {
    JsonDocument doc;

    JsonArray contents = doc["contents"].to<JsonArray>();
    JsonObject content = contents.add<JsonObject>();
    content["role"] = "user";
    JsonArray parts = content["parts"].to<JsonArray>();

    if (jpeg && jpegLen > 0) {
        size_t b64len = ((jpegLen + 2) / 3) * 4 + 1;
        char *b64 = (char *)ps_malloc(b64len);
        if (b64) {
            b64enc(jpeg, jpegLen, b64);
            JsonObject p = parts.add<JsonObject>();
            p["inlineData"]["mimeType"] = "image/jpeg";
            p["inlineData"]["data"] = b64;
            free(b64);
        }
    }

    if (pcm && pcmSamp > 0) {
        uint32_t audioBytes = pcmSamp * 2;
        size_t wavSize = 44 + audioBytes;
        uint8_t *wav = (uint8_t *)ps_malloc(wavSize);
        if (wav) {
            make_wav_hdr(wav, audioBytes, AI_AUDIO_SAMPLE_RATE);
            memcpy(wav + 44, pcm, audioBytes);
            size_t b64len = ((wavSize + 2) / 3) * 4 + 1;
            char *b64 = (char *)ps_malloc(b64len);
            if (b64) {
                b64enc(wav, wavSize, b64);
                JsonObject p = parts.add<JsonObject>();
                p["inlineData"]["mimeType"] = "audio/wav";
                p["inlineData"]["data"] = b64;
                free(b64);
            }
            free(wav);
        }
    }

    JsonArray vals = doc["responseModalities"].to<JsonArray>();
    vals.add("audio");

    String out;
    serializeJson(doc, out);
    return out;
}

// ============================================================
// Core 0 Task: HTTP REST + SSE stream
// ============================================================
static void ai_net_task(void *pvParameters) {
    uint8_t *decBuf = (uint8_t *)ps_malloc(64 * 1024);
    if (!decBuf) { vTaskDelete(NULL); return; }

    while (true) {
        if (!pipelineBusy || !dataReady) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // --- Wi‑Fi (Multi‑WiFi + WiFiManager fallback) ---
        if (!ensure_wifi()) {
            Serial.println("[AI_NET] Wi‑Fi unavailable, aborting");
            pipelineBusy = false; dataReady = false; continue;
        }

        if (!creds_loaded) wifi_creds_refresh();

        // --- Build request body with actual recorded length ---
        size_t nSamp = recordPos;
        if (nSamp == 0) {
            Serial.println("[AI_NET] No audio recorded");
            wifi_shutdown(); pipelineBusy = false; dataReady = false; continue;
        }

        String body = build_request(recAudio, nSamp, jpegBuf, jpegSize);
        dataReady = false;

        if (body.length() == 0) {
            Serial.println("[AI_NET] Empty request body");
            wifi_shutdown(); pipelineBusy = false; continue;
        }

        // --- Send HTTP POST and stream response ---
        Serial.println("[AI_NET] Connecting to Gemini...");
        WiFiClientSecure client;
        client.setInsecure();

        if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
            Serial.println("[AI_NET] Connection failed");
            wifi_shutdown(); pipelineBusy = false; continue;
        }

        String geminiKey = cached_api_key;
        String path = "/v1beta/" + String(GEMINI_MODEL) + ":streamGenerateContent?alt=sse&key=" + geminiKey;
        String req = "POST " + path + " HTTP/1.1\r\n"
                     "Host: " + GEMINI_API_HOST + "\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + String(body.length()) + "\r\n"
                     "Connection: close\r\n\r\n" + body;

        Serial.printf("[AI_NET] Sending %d bytes request\n", body.length());
        client.print(req);

        // Read HTTP header using char buffer (no String allocation)
        bool headerDone = false;
        uint32_t timeout = millis() + 10000;
        char hdr[256];
        while (client.connected() && millis() < timeout) {
            size_t n = client.readBytesUntil('\n', (uint8_t *)hdr, sizeof(hdr) - 1);
            if (n == 0) continue;
            hdr[n] = '\0';
            while (n > 0 && (hdr[n - 1] == '\r' || hdr[n - 1] == ' ')) hdr[--n] = '\0';
            if (n == 0) { headerDone = true; break; }
        }
        if (!headerDone) {
            Serial.println("[AI_NET] Header timeout");
            client.stop(); wifi_shutdown(); pipelineBusy = false; continue;
        }

        tone_driver_stream_set_active(true);
        Serial.println("[AI_NET] Reading response stream...");

        // SSE stream using char buffer (zero String allocations per line)
        char sseLine[1024];
        while (client.connected() && pipelineBusy) {
            size_t n = client.readBytesUntil('\n', (uint8_t *)sseLine, sizeof(sseLine) - 1);
            if (n == 0) continue;
            sseLine[n] = '\0';
            while (n > 0 && (sseLine[n - 1] == '\r' || sseLine[n - 1] == ' ')) sseLine[--n] = '\0';

            if (strncmp(sseLine, "data: ", 6) != 0) continue;

            const char *json = sseLine + 6;
            if (strcmp(json, "[DONE]") == 0) break;

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, json);
            if (err) continue;

            JsonObject cand = doc["candidates"][0];
            if (cand.isNull()) continue;

            const char *finish = cand["finishReason"];
            if (finish && strcmp(finish, "STOP") == 0) break;

            JsonObject part = cand["content"]["parts"][0];
            if (part.isNull()) continue;

            const char *b64 = part["inlineData"]["data"];
            if (!b64) continue;

            size_t b64len = strlen(b64);
            size_t dlen = b64dec(b64, b64len, decBuf);
            size_t samples = dlen / 2;
            if (samples > 0) {
                tone_driver_stream_write((int16_t *)decBuf, samples);
            }
        }

        client.stop();
        Serial.println("[AI_NET] Response complete");

        tone_driver_stream_set_active(false);
        wifi_shutdown();
        pipelineBusy = false;
    }
}

// ============================================================
// Public API
// ============================================================
void ai_pipeline_start(void) {
    if (pipelineBusy) return;
    if (!alloc_buffers()) return;
    dataReady = false;
    recordPos = 0;
    pipelineBusy = true;
    Serial.println("[AI] Pipeline START — hold button to record");
}

void ai_pipeline_stop(void) {
    if (!pipelineBusy) return;
    pipelineBusy = false;
    dataReady = false;
    tone_driver_stream_set_active(false);
    wifi_shutdown();
    Serial.println("[AI] Pipeline STOP");
}

bool ai_pipeline_is_busy(void) {
    return pipelineBusy;
}

void ai_pipeline_net_task_start(void) {
    xTaskCreatePinnedToCore(ai_net_task, "ai_net", AI_NET_TASK_STACK,
                            NULL, AI_NET_TASK_PRIO, NULL, 0);
    Serial.println("[AI] NET task started on Core 0");
}

void ai_pipeline_audio_task_start(void) {
    xTaskCreatePinnedToCore(ai_audio_task, "ai_audio", AI_AUDIO_TASK_STACK,
                            NULL, AI_AUDIO_TASK_PRIO, NULL, 1);
    Serial.println("[AI] Audio task started on Core 1");
}

#else
void ai_pipeline_start(void) {}
void ai_pipeline_stop(void) {}
bool ai_pipeline_is_busy(void) { return false; }
void ai_pipeline_net_task_start(void) {}
void ai_pipeline_audio_task_start(void) {}
#endif
