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

static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
// URL Encode chuẩn UTF-8
// ============================================================
static String url_encode(const String &str) {
    String encoded;
    for (size_t i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        if (isalnum(c)) { 
            encoded += c; 
        } else if (c == ' ') { 
            encoded += "%20"; 
        } else { 
            char buf[4]; 
            snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c); 
            encoded += buf; 
        }
    }
    return encoded;
}

// ============================================================
// Text Normalization — Tẩy sạch triệt để LaTeX & Mã rác
// ============================================================
static String normalize_text(const String &text) {
    String t = text;

    // 1. Dọn dẹp mã LaTeX & ký tự toán học rác
    t.replace("\\times", " nhân ");
    t.replace("\\text{", "");
    t.replace("}", "");
    t.replace("\\", "");
    t.replace("$", "");
    t.replace("**", "");
    t.replace("*", "");
    
    // 2. Chuyển đổi công thức toán mũ
    t.replace("10^9", " tỷ");
    t.replace("10^6", " triệu");
    t.replace("^", " mũ ");

    // 3. Chuẩn hóa tên thuật ngữ tránh đọc ngọng
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
// Kiểm tra ranh giới câu (Sentence boundary)
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
// Bắn trực tiếp dữ liệu MP3 từ Google TTS sang Máy tính (Không lưu Flash)
// ============================================================
static void stream_tts_direct_to_pc(const String &text) {
    String norm = normalize_text(text);
    norm.trim();
    if (norm.length() == 0) return;
    if (norm.length() > 100) norm = norm.substring(0, 100);

    WiFiClient client;
    if (!client.connect("translate.google.com", 80, 5000)) {
        Serial.println("[TTS STREAM] Kết nối Google TTS thất bại!");
        return;
    }

    String path = "/translate_tts?ie=UTF-8&q=" + url_encode(norm) + "&sl=vi&tl=vi&client=gtx";
    
    client.printf("GET %s HTTP/1.1\r\n", path.c_str());
    client.println("Host: translate.google.com");
    client.println("User-Agent: Mozilla/5.0");
    client.println("Connection: close");
    client.println();

    // Chờ phản hồi HTTP
    unsigned long t0 = millis();
    while (!client.available() && millis() - t0 < 3000) delay(10);

    // Bỏ qua HTTP Headers
    while (client.connected() || client.available()) {
        String line = client.readStringUntil('\n');
        if (line == "\r" || line.length() == 0) break;
    }

    // Tín hiệu báo cho Script Python bắt đầu nhận luồng MP3
    Serial.println("\n<<<START_MP3_TRANSFER>>>");
    Serial.flush();

    size_t totalBytes = 0;
    uint8_t buf[512];

    // Bắn thẳng dữ liệu nhận từ Wi-Fi ra Serial (RAM buffer tạm chỉ 512 bytes)
    while (client.connected() || client.available()) {
        int len = client.read(buf, sizeof(buf));
        if (len > 0) {
            Serial.write(buf, len); // Stream thẳng qua USB-Serial
            totalBytes += len;
        }
    }

    client.stop();

    // Tín hiệu báo kết thúc luồng
    Serial.println("\n<<<END_MP3_TRANSFER>>>");
    Serial.flush();

    Serial.printf("[TTS STREAM SUCCESS] Đã stream %zu bytes thẳng sang PC!\n", totalBytes);
}

// ============================================================
// Safe read one line từ WiFiClientSecure (SSE)
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

// ============================================================
// Gemini SSE Streaming — Tải văn bản & Tạo File TTS
// ============================================================
static void stream_gemini(const char *apiKey, const char *question) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            Serial.printf("[RETRY #%d]\n", attempt);
            delay(2000);
        }

        WiFiClientSecure client;
        client.setInsecure();

        Serial.print("[GEMINI] Connecting...");
        if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT, 10000)) {
            Serial.println(" FAIL");
            continue;
        }
        Serial.println(" OK");
        client.setNoDelay(true);

        // Đưa yêu cầu xuống DƯỚI câu hỏi (đặt ở cuối prompt)
        String promptFinal = String(question) + 
            "Trả lời ngắn gọn và không dùng markdown hay latex";

        JsonDocument doc;
        JsonObject content0 = doc["contents"].add<JsonObject>();
        content0["role"] = "user"; // Thêm role để Gemini v1beta nhận diện đúng cấu trúc
        JsonArray reqParts = content0["parts"].to<JsonArray>();
        JsonObject reqPart0 = reqParts.add<JsonObject>();
        reqPart0["text"] = promptFinal;

        String bodyStr;
        serializeJson(doc, bodyStr);

        // Send HTTP POST
        client.printf("POST /v1beta/%s:streamGenerateContent?alt=sse&key=%s HTTP/1.1\r\n",
                      GEMINI_MODEL, apiKey);
        client.printf("Host: %s\r\n", GEMINI_API_HOST);
        client.println("Content-Type: application/json");
        client.printf("Content-Length: %d\r\n", bodyStr.length());
        client.println("Connection: close");
        client.println();
        client.print(bodyStr);

        // Read HTTP status
        unsigned long t0 = millis();
        while (!client.available() && client.connected() && millis() - t0 < 20000) {
            delay(10);
        }

        if (!client.available()) {
            Serial.println("[GEMINI] No response — retrying");
            client.stop();
            continue;
        }

        String statusLine;
        read_sse_line(client, statusLine, 5000);
        statusLine.trim();
        Serial.printf("[GEMINI] %s\n", statusLine.c_str());

        if (!statusLine.startsWith("HTTP/1.1 200") && !statusLine.startsWith("HTTP/1.0 200")) {
            while (client.available()) Serial.write(client.read());
            client.stop();
            return;
        }

        // Discard headers
        while (client.connected() || client.available()) {
            String l;
            if (!read_sse_line(client, l, 3000)) break;
            l.trim();
            if (l.length() == 0) break;
        }

        // Đọc toàn bộ Text Gemini
        String fullText;
        bool sentGeminiDone = false;

        while (client.connected() && !sentGeminiDone) {
            while (client.available()) {
                String line;
                if (!read_sse_line(client, line, 3000)) break;
                line.trim();
                if (!line.startsWith("data: ")) continue;

                String data = line.substring(6);
                if (data == "[DONE]") {
                    Serial.println("\n[GEMINI] [DONE]");
                    sentGeminiDone = true;
                    break;
                }

                JsonDocument chunkDoc;
                DeserializationError err = deserializeJson(chunkDoc, data);
                if (err) continue;

                JsonArray parts = chunkDoc["candidates"][0]["content"]["parts"].as<JsonArray>();
                if (!parts) continue;

                for (JsonVariant part : parts) {
                    const char *text = part["text"];
                    if (text) {
                        fullText += text;
                        Serial.print(text);
                        Serial.flush();
                    }
                }
            }
            if (!client.available()) delay(5);
        }

        // Đóng Socket SSL ngay lập tức
        client.stop();
        Serial.println("\n[GEMINI] Socket closed & TLS freed.");

        // Tiến hành tạo File MP3 duy nhất lưu trên SPIFFS
        if (fullText.length() > 0) {
            stream_tts_direct_to_pc(fullText);
        }

        Serial.println("[GEMINI] Done");
        return; // Success
    }
}

// ============================================================
// Serial Configuration — WiFi
// ============================================================
static bool serial_configure_wifi(void) {
    char ssid[64] = {}, pass[64] = {};

    Serial.println("\n=== Cấu hình WiFi ===");
    Serial.print("SSID: ");
    read_line(ssid, sizeof(ssid));
    if (strlen(ssid) == 0) { Serial.println("[SKIP]"); return false; }

    Serial.print("Password: ");
    read_line(pass, sizeof(pass));

    secrets_set(SK_WIFI_SSID, String(ssid));
    secrets_set(SK_WIFI_PASS, String(pass));
    Serial.printf("[NVS] Saved SSID=%s\n", ssid);

    // Connect
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    Serial.printf("[WIFI] Connecting to %s ...", ssid);
    int c = 0;
    while (WiFi.status() != WL_CONNECTED && c < 40) {
        delay(500); Serial.print("."); c++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] OK: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("\n[WIFI] FAIL — check SSID/password");
    return false;
}

// ============================================================
// Serial Configuration — Gemini API Key
// ============================================================
static void serial_configure_apikey(char *out, size_t outSize) {
    char key[128] = {};
    Serial.println("\n=== Cấu hình Gemini API Key ===");
    Serial.print("API Key: ");
    read_line(key, sizeof(key));
    if (strlen(key) == 0) { Serial.println("[SKIP]"); return; }

    secrets_set(SK_GEMINI_KEY, String(key));
    strncpy(out, key, outSize - 1);
    out[outSize - 1] = '\0';
    Serial.println("[NVS] API Key saved");
}

// ============================================================
// Main test task
// ============================================================
static char g_apiKey[128] = {};

static void run_api_test(void *pv) {
    while (true) {
        // Kiểm tra WiFi + API Key trước mỗi vòng
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("\n[!] WiFi disconnected");
            if (!serial_configure_wifi()) {
                Serial.println("[!] Enter /wifi to reconfigure later");
            }
        }
        if (strlen(g_apiKey) == 0) {
            serial_configure_apikey(g_apiKey, sizeof(g_apiKey));
        }

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
        if (strcmp(question, "/wifi") == 0) {
            serial_configure_wifi();
            continue;
        }
        if (strcmp(question, "/apikey") == 0) {
            serial_configure_apikey(g_apiKey, sizeof(g_apiKey));
            continue;
        }
        if (strlen(question) == 0) continue;

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[!] No WiFi — use /wifi to connect first");
            continue;
        }
        if (strlen(g_apiKey) == 0) {
            Serial.println("[!] No API Key — use /apikey to set first");
            continue;
        }

        Serial.println("\n--- [Streaming] Gemini → Save MP3 ---");
        stream_gemini(g_apiKey, question);
    }

    Serial.println("\nTest complete. Reset to restart.");
    vTaskDelete(NULL);
}

// ============================================================
// Entry points
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=============================================");
    Serial.println("  AEGIS SIGHT - Gemini to MP3 File Generator");
    Serial.println("=============================================");

    if (!psramFound()) {
        Serial.println("[FAIL] PSRAM not found");
        return;
    }
    Serial.printf("PSRAM: %zu KB free\n", ESP.getPsramSize() / 1024);

    if(!SPIFFS.begin(true)){
        Serial.println("[SPIFFS] Mount Failed");
        return;
    }
    Serial.println("[SPIFFS] Mounted successfully");

    secrets_begin();
    if (!secrets_has_all()) {
        Serial.println("[INIT] No saved config.");
        Serial.println("  Type WiFi SSID below, or /portal to use web setup:");
        Serial.print("SSID: ");
        char ssid[64] = {}, pass[64] = {};
        read_line(ssid, sizeof(ssid));
        if (strcmp(ssid, "/portal") != 0 && strlen(ssid) > 0) {
            Serial.print("Password: ");
            read_line(pass, sizeof(pass));
            secrets_set(SK_WIFI_SSID, String(ssid));
            secrets_set(SK_WIFI_PASS, String(pass));
            Serial.print("Gemini API Key: ");
            char key[128] = {};
            read_line(key, sizeof(key));
            if (strlen(key) > 0) secrets_set(SK_GEMINI_KEY, String(key));
        }
        if (!secrets_has_all()) {
            Serial.println("[INIT] Starting config portal...");
            config_portal_start();
        }
    }

    {
        String ssid   = secrets_get(SK_WIFI_SSID);
        String pass   = secrets_get(SK_WIFI_PASS);
        String apiKey = secrets_get(SK_GEMINI_KEY);
        strncpy(g_apiKey, apiKey.c_str(), sizeof(g_apiKey) - 1);
        g_apiKey[sizeof(g_apiKey) - 1] = '\0';

        if (ssid.length() > 0) {
            Serial.printf("[WIFI] Connecting to %s ...\n", ssid.c_str());
            WiFi.mode(WIFI_STA);
            WiFi.begin(ssid.c_str(), pass.c_str());
            int c = 0;
            while (WiFi.status() != WL_CONNECTED && c < 40) {
                delay(500); Serial.print("."); c++;
            }
            if (WiFi.status() == WL_CONNECTED)
                Serial.printf("\n[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());
            else
                Serial.println("\n[WIFI] FAIL — use /wifi command later");
        }
    }

    xTaskCreatePinnedToCore(run_api_test, "api_test", 16384, NULL, 1, NULL, 1);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

#endif