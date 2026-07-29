#include <Arduino.h>
#include "config.h"

#ifdef ENABLE_API_TEST

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include "secrets.h"
#include "config_portal.h"

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
// HTTP helpers
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

// ============================================================
// URL Encode
// ============================================================
static String url_encode(const String &str) {
    String encoded;
    for (size_t i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        if (isalnum(c)) { encoded += c; }
        else if (c == ' ') { encoded += "%20"; }
        else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c); encoded += buf; }
    }
    return encoded;
}

// ============================================================
// Text Normalization
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
// Stream MP3 từ Google TTS sang PC (chunk tối đa 200 ký tự)
// ============================================================
static void stream_tts_chunk_to_pc(const String &text) {
    String norm = normalize_text(text);
    norm.trim();
    if (norm.length() == 0) return;

    // Split at word boundaries if > 200 chars
    int pos = 0;
    while (pos < (int)norm.length()) {
        int end = pos + 200;
        if (end >= (int)norm.length()) end = norm.length();
        else {
            int sp = end;
            while (sp > pos && norm[sp] != ' ') sp--;
            if (sp == pos) sp = end;
            end = sp;
        }
        String chunk = norm.substring(pos, end);
        chunk.trim();
        if (chunk.length() == 0) { pos = end + 1; continue; }

        WiFiClient tts;
        if (!tts.connect("translate.google.com", 80, 5000)) {
            Serial.println("[TTS] Connect fail"); return;
        }
        String path = "/translate_tts?ie=UTF-8&q=" + url_encode(chunk) + "&tl=vi&sl=vi&client=gtx";
        tts.printf("GET %s HTTP/1.1\r\nHost: translate.google.com\r\nUser-Agent: Mozilla/5.0\r\nConnection: close\r\n\r\n", path.c_str());

        unsigned long t0 = millis();
        while (!tts.available() && millis() - t0 < 3000) delay(10);
        while (tts.connected() || tts.available()) {
            String l = tts.readStringUntil('\n');
            if (l == "\r" || l.length() == 0) break;
        }

        Serial.println("\n<<<START_MP3_TRANSFER>>>");
        Serial.flush();

        uint8_t buf[512];
        size_t total = 0;
        while (tts.connected() || tts.available()) {
            int len = tts.read(buf, sizeof(buf));
            if (len > 0) { Serial.write(buf, len); total += len; }
        }
        tts.stop();

        Serial.println("\n<<<END_MP3_TRANSFER>>>");
        Serial.flush();
        Serial.printf("[TTS] %zu bytes\n", total);

        pos = end;
        while (pos < (int)norm.length() && norm[pos] == ' ') pos++;
    }
}

// ============================================================
// Streaming buffer: accumulate text, flush complete sentences
// ============================================================
static String ttsSentenceBuf;

static void flush_sentences(void) {
    int lastBoundary = -1;
    for (int i = 0; i < (int)ttsSentenceBuf.length(); i++) {
        if (is_sentence_end(ttsSentenceBuf, i)) lastBoundary = i;
    }
    if (lastBoundary >= 0) {
        String complete = ttsSentenceBuf.substring(0, lastBoundary + 1);
        complete.trim();
        if (complete.length() > 0) stream_tts_chunk_to_pc(complete);
        ttsSentenceBuf = ttsSentenceBuf.substring(lastBoundary + 1);
    }
}

static void flush_remaining(void) {
    ttsSentenceBuf.trim();
    if (ttsSentenceBuf.length() > 0) stream_tts_chunk_to_pc(ttsSentenceBuf);
    ttsSentenceBuf = "";
}

// ============================================================
// Read SSE event (handles event: + data: lines)
// ============================================================
static bool read_sse_event(WiFiClientSecure &cl, String &event, String &data, int tmoMs) {
    event = ""; data = "";
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)tmoMs) {
        if (cl.available()) {
            String line;
            if (!http_read_line(cl, line, tmoMs)) return false;
            if (line.startsWith("event: ")) event = line.substring(7);
            else if (line.startsWith("data: ")) data = line.substring(6);
            if (line.length() == 0) return true; // end of SSE event
            t0 = millis();
        } else if (!cl.connected()) { return false; }
        else { delay(5); }
    }
    return false;
}

// ============================================================
// Gemini Interactions API — SSE streaming → TTS từng chunk ra PC
// ============================================================
static void stream_gemini(const char *apiKey, const char *question) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) { Serial.printf("[RETRY #%d]\n", attempt); delay(2000); }

        WiFiClientSecure client;
        client.setInsecure();

        Serial.print("[GEMINI] Connecting...");
        if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT, 10000)) {
            Serial.println(" FAIL"); continue;
        }
        Serial.println(" OK");
        client.setNoDelay(true);

        // Build JSON body for Interactions API
        JsonDocument doc;
        doc["model"] = GEMINI_MODEL_SHORT;
        doc["system_instruction"] = "Bạn là chuyên gia hỗ trợ người mù. Bạn cần trả lời ngắn gọn nhưng vẫn đầy đủ ý nhất có thể để người mù biết tình hình. Không sử dụng markdown";
        doc["stream"] = true;
        JsonArray input = doc["input"].to<JsonArray>();
        JsonObject txt = input.add<JsonObject>();
        txt["type"] = "text";
        txt["text"] = String(question);

        String bodyStr;
        serializeJson(doc, bodyStr);

        // Send POST with ?alt=sse for streaming
        client.printf("POST /v1beta/interactions?alt=sse HTTP/1.1\r\n");
        client.printf("Host: %s\r\n", GEMINI_API_HOST);
        client.printf("X-Goog-Api-Key: %s\r\n", apiKey);
        client.printf("Content-Type: application/json\r\n");
        client.printf("Content-Length: %zu\r\n\r\n", bodyStr.length());
        client.print(bodyStr);

        // Read HTTP status
        String statusLine;
        if (!http_read_line(client, statusLine, 10000)) {
            client.stop(); continue;
        }
        statusLine.trim();
        Serial.printf("[GEMINI] %s\n", statusLine.c_str());
        if (!statusLine.startsWith("HTTP/1.1 200") && !statusLine.startsWith("HTTP/1.0 200")) {
            while (client.available()) Serial.write(client.read());
            client.stop(); return;
        }

        // Skip headers
        http_skip_headers(client, 5000);

        // SSE stream → incremental TTS
        ttsSentenceBuf = "";
        bool keepReading = true;

        while (client.connected() && keepReading) {
            while (client.available()) {
                String event, data;
                if (!read_sse_event(client, event, data, 10000)) { keepReading = false; break; }

                if (data == "[DONE]") { keepReading = false; break; }

                if (event == "interaction.completed" || event == "done") {
                    flush_remaining();
                    keepReading = false;
                    break;
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
                }
            }
            if (client.available() == 0 && keepReading) delay(5);
        }

        client.stop();
        flush_remaining();
        Serial.println("\n[GEMINI] Done");
        return;
    }
}

// ============================================================
// Serial Configuration — WiFi
// ============================================================
static bool serial_configure_wifi(void) {
    char ssid[64] = {}, pass[64] = {};
    Serial.println("\n=== Cấu hình WiFi ===");
    Serial.print("SSID: "); read_line(ssid, sizeof(ssid));
    if (strlen(ssid) == 0) { Serial.println("[SKIP]"); return false; }
    Serial.print("Password: "); read_line(pass, sizeof(pass));
    secrets_set(SK_WIFI_SSID, String(ssid));
    secrets_set(SK_WIFI_PASS, String(pass));
    Serial.printf("[NVS] Saved SSID=%s\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    Serial.printf("[WIFI] Connecting to %s ...", ssid);
    int c = 0;
    while (WiFi.status() != WL_CONNECTED && c < 40) { delay(500); Serial.print("."); c++; }
    if (WiFi.status() == WL_CONNECTED) { Serial.printf("\n[WIFI] OK: %s\n", WiFi.localIP().toString().c_str()); return true; }
    Serial.println("\n[WIFI] FAIL");
    return false;
}

// ============================================================
// Serial Configuration — Gemini API Key
// ============================================================
static void serial_configure_apikey(char *out, size_t outSize) {
    char key[128] = {};
    Serial.println("\n=== Cấu hình Gemini API Key ===");
    Serial.print("API Key: "); read_line(key, sizeof(key));
    if (strlen(key) == 0) { Serial.println("[SKIP]"); return; }
    secrets_set(SK_GEMINI_KEY, String(key));
    strncpy(out, key, outSize - 1); out[outSize - 1] = '\0';
    Serial.println("[NVS] API Key saved");
}

// ============================================================
// Main test task
// ============================================================
static char g_apiKey[128] = {};

static void run_api_test(void *pv) {
    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("\n[!] WiFi disconnected");
            if (!serial_configure_wifi()) Serial.println("[!] Enter /wifi to reconfigure later");
        }
        if (strlen(g_apiKey) == 0) serial_configure_apikey(g_apiKey, sizeof(g_apiKey));

        Serial.println("\n--- Commands: /wifi /apikey /help /quit ---");
        Serial.print("> ");
        char question[256] = {};
        read_line(question, sizeof(question));

        if (strcmp(question, "quit") == 0) break;
        if (strcmp(question, "/help") == 0) {
            Serial.println("  /wifi       — Change WiFi SSID/PASS");
            Serial.println("  /apikey     — Change Gemini API Key");
            Serial.println("  /quit       — Exit test");
            Serial.println("  <question>  — Ask Gemini");
            continue;
        }
        if (strcmp(question, "/wifi") == 0) { serial_configure_wifi(); continue; }
        if (strcmp(question, "/apikey") == 0) { serial_configure_apikey(g_apiKey, sizeof(g_apiKey)); continue; }
        if (strlen(question) == 0) continue;
        if (WiFi.status() != WL_CONNECTED) { Serial.println("[!] No WiFi — use /wifi first"); continue; }
        if (strlen(g_apiKey) == 0) { Serial.println("[!] No API Key — use /apikey first"); continue; }

        Serial.println("\n--- [Interactions API + Streaming TTS] ---");
        stream_gemini(g_apiKey, question);
    }
    Serial.println("\nTest complete. Reset to restart.");
    vTaskDelete(NULL);
}

// ============================================================
// Entry points
// ============================================================
void setup() {
    Serial.begin(115200); delay(500);

    Serial.println("\n=============================================");
    Serial.println("  AEGIS SIGHT - Interactions API + TTS Test");
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
            WiFi.mode(WIFI_STA);
            WiFi.begin(ssid.c_str(), pass.c_str());
            int c = 0;
            while (WiFi.status() != WL_CONNECTED && c < 40) { delay(500); Serial.print("."); c++; }
            if (WiFi.status() == WL_CONNECTED) Serial.printf("\n[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());
            else Serial.println("\n[WIFI] FAIL");
        }
    }

    xTaskCreatePinnedToCore(run_api_test, "api_test", 16384, NULL, 1, NULL, 1);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }

#endif
