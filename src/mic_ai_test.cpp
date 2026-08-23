#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "config.h"
#include "driver/i2s.h"

#if defined(ENABLE_MIC_AI_TEST)

#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h"

// ============================================================================
//  USER CONFIGURATION HEADER
// ============================================================================
#define USER_WIFI_SSID        "HCMUT.EDU"
#define USER_WIFI_PASS        "Suong72730109"

#define USER_GEMINI_API_KEY   "API"

#define RECORD_TIME_SEC       3               
#define AUDIO_GAIN            2.0f            
#define TARGET_GEMINI_MODEL   "gemini-3.1-flash-lite" 

// ============================================================================
#define TOTAL_SAMPLES         (AI_AUDIO_SAMPLE_RATE * RECORD_TIME_SEC) 
#define WAV_HEADER_SIZE       44

static int16_t *audioRecordBuf = NULL;
static String active_ssid = "";
static String active_pass = "";
static String active_api_key = "";

// ============================================================
// 1. HÀM TẠO WAV HEADER 44 BYTES
// ============================================================
static void createWavHeader(uint8_t *header, uint32_t pcmDataLen, uint32_t sampleRate, uint16_t numChannels, uint16_t bitsPerSample) {
    uint32_t totalDataLen = pcmDataLen + 36;
    uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    uint16_t blockAlign = numChannels * (bitsPerSample / 8);

    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = (uint8_t)(totalDataLen & 0xff);
    header[5] = (uint8_t)((totalDataLen >> 8) & 0xff);
    header[6] = (uint8_t)((totalDataLen >> 16) & 0xff);
    header[7] = (uint8_t)((totalDataLen >> 24) & 0xff);
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';

    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0;
    header[22] = (uint8_t)numChannels; header[23] = 0;
    header[24] = (uint8_t)(sampleRate & 0xff);
    header[25] = (uint8_t)((sampleRate >> 8) & 0xff);
    header[26] = (uint8_t)((sampleRate >> 16) & 0xff);
    header[27] = (uint8_t)((sampleRate >> 24) & 0xff);
    header[28] = (uint8_t)(byteRate & 0xff);
    header[29] = (uint8_t)((byteRate >> 8) & 0xff);
    header[30] = (uint8_t)((byteRate >> 16) & 0xff);
    header[31] = (uint8_t)((byteRate >> 24) & 0xff);
    header[32] = (uint8_t)blockAlign; header[33] = 0;
    header[34] = (uint8_t)bitsPerSample; header[35] = 0;

    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)(pcmDataLen & 0xff);
    header[41] = (uint8_t)((pcmDataLen >> 8) & 0xff);
    header[42] = (uint8_t)((pcmDataLen >> 16) & 0xff);
    header[43] = (uint8_t)((pcmDataLen >> 24) & 0xff);
}

// ============================================================
// 2. LOA GROVE PWM OUTPUT CLASS
// ============================================================
class SeeedGrovePWMOutput : public AudioOutput {
private:
    uint8_t _pin;
    uint8_t _channel;
    uint32_t _sample_rate;
    uint32_t _last_sample_us;
    uint32_t _sample_interval_us;
    bool _is_active;

public:
    SeeedGrovePWMOutput(uint8_t pin, uint8_t channel = 2) {
        _pin = pin;
        _channel = channel;
        _sample_rate = 24000;
        _sample_interval_us = 1000000 / _sample_rate;
        _last_sample_us = 0;
        _is_active = false;
    }

    virtual bool SetRate(int hz) override {
        if (hz > 0) {
            _sample_rate = hz;
            _sample_interval_us = 1000000 / _sample_rate;
        }
        return true;
    }

    virtual bool begin() override {
        ledcSetup(_channel, 31250, 8);
        ledcAttachPin(_pin, _channel);
        
        for (int b = 0; b <= 128; b++) {
            ledcWrite(_channel, b);
            delayMicroseconds(100);
        }

        _last_sample_us = micros();
        _is_active = true;
        return true;
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!_is_active) return true;

        int32_t pcm = ((int32_t)sample[0] + (int32_t)sample[1]) / 2;
        pcm = (pcm * 7) / 2;
        if (pcm > 32767)  pcm = 32767;
        if (pcm < -32768) pcm = -32768;

        uint8_t duty = (uint8_t)(((pcm + 32768) >> 8) & 0xFF);

        while ((micros() - _last_sample_us) < _sample_interval_us) {
            #if defined(ESP32)
            NOP();
            #endif
        }
        _last_sample_us = micros();

        ledcWrite(_channel, duty);
        return true;
    }

    virtual bool stop() override {
        _is_active = false;
        for (int b = 128; b >= 0; b--) {
            ledcWrite(_channel, b);
            delayMicroseconds(100);
        }
        ledcDetachPin(_pin);
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        return true;
    }
};

static AudioGeneratorMP3 *mp3 = NULL;
static AudioFileSourceHTTPStream *file = NULL;
static AudioFileSourceBuffer *buff = NULL;
static SeeedGrovePWMOutput *out = NULL;

// ============================================================
// 3. KHỞI TẠO MICRO I2S (INMP441)
// ============================================================
static bool initMicI2S() {
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
    if (i2s_set_pin(I2S_MIC_PORT, &pin_cfg) != ESP_OK) return false;
    return true;
}

// ============================================================
// 4. THU ÂM 3 GIÂY TỪ MIC INMP441
// ============================================================
static bool recordAudio3Seconds() {
    Serial.println("\n[MIC] >>> BẮT ĐẦU GHI ÂM (NÓI TRONG 3 GIÂY) <<<");
    
    int32_t *rawBuf = (int32_t *)ps_malloc(512 * sizeof(int32_t));
    if (!rawBuf) return false;

    size_t samplesRecorded = 0;
    float dc_offset = 0.0f;

    size_t dummy_bytes = 0;
    i2s_read(I2S_MIC_PORT, rawBuf, 512 * sizeof(int32_t), &dummy_bytes, 100);

    unsigned long start_time = millis();
    while (samplesRecorded < TOTAL_SAMPLES && (millis() - start_time < (RECORD_TIME_SEC * 1000 + 500))) {
        size_t bytes_read = 0;
        esp_err_t res = i2s_read(I2S_MIC_PORT, rawBuf, 512 * sizeof(int32_t), &bytes_read, portMAX_DELAY);
        
        if (res == ESP_OK && bytes_read > 0) {
            size_t count = bytes_read / sizeof(int32_t);
            for (size_t i = 0; i < count; i++) {
                if (samplesRecorded >= TOTAL_SAMPLES) break;
                
                int32_t s24 = rawBuf[i] >> 8;
                float sample = (float)s24;
                
                dc_offset = 0.99f * dc_offset + 0.01f * sample;
                float clean = (sample - dc_offset) * AUDIO_GAIN;

                if (clean > 32767.0f) clean = 32767.0f;
                if (clean < -32768.0f) clean = -32768.0f;

                audioRecordBuf[samplesRecorded++] = (int16_t)clean;
            }
        }
    }

    free(rawBuf);
    Serial.printf("[MIC] Hoàn tất ghi âm: %zu samples (%lu ms)\n", samplesRecorded, millis() - start_time);
    return samplesRecorded > 0;
}

// ============================================================
// 5. MÃ HÓA BASE64 ĐÃ ĐƯỢC CHUẨN HÓA (FIX HOÀN TOÀN LỖI PADDING)
// ============================================================
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64Encode(const uint8_t *data, size_t length) {
    String encoded = "";
    encoded.reserve(((length + 2) / 3) * 4);
    
    int i = 0;
    uint8_t array_3[3];
    uint8_t array_4[4];

    while (length--) {
        array_3[i++] = *(data++);
        if (i == 3) {
            array_4[0] = (array_3[0] & 0xfc) >> 2;
            array_4[1] = ((array_3[0] & 0x03) << 4) + ((array_3[1] & 0xf0) >> 4);
            array_4[2] = ((array_3[1] & 0x0f) << 2) + ((array_3[2] & 0xc0) >> 6);
            array_4[3] = array_3[2] & 0x3f;

            for (i = 0; (i < 4); i++) encoded += base64_chars[array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (int j = i; j < 3; j++) array_3[j] = '\0';
        array_4[0] = (array_3[0] & 0xfc) >> 2;
        array_4[1] = ((array_3[0] & 0x03) << 4) + ((array_3[1] & 0xf0) >> 4);
        array_4[2] = ((array_3[1] & 0x0f) << 2) + ((array_3[2] & 0xc0) >> 6);

        for (int j = 0; (j < i + 1); j++) encoded += base64_chars[array_4[j]];
        while ((i++ < 3)) encoded += '=';
    }
    return encoded;
}

// ============================================================
// 6. PHÁT TIẾNG QUA GOOGLE TTS LOA GROVE
// ============================================================
static String urlEncode(String str) {
    String encoded = "";
    char c, code0, code1;
    for (size_t i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (isalnum(c)) {
            encoded += c;
        } else {
            code0 = (c >> 4) & 0xF;
            code1 = c & 0xF;
            encoded += '%';
            encoded += (char)(code0 < 10 ? code0 + '0' : code0 - 10 + 'A');
            encoded += (char)(code1 < 10 ? code1 + '0' : code1 - 10 + 'A');
        }
    }
    return encoded;
}

static void playGoogleTTS(String text, String lang = "vi") {
    if (text.length() == 0) return;

    String url = "http://translate.google.com/translate_tts?ie=UTF-8&q=" 
               + urlEncode(text) 
               + "&tl=" + lang 
               + "&client=tw-ob";

    Serial.printf("\n[TTS] Đang phát câu trả lời: \"%s\"\n", text.c_str());

    file = new AudioFileSourceHTTPStream(url.c_str());
    buff = new AudioFileSourceBuffer(file, TTS_MP3_BUF_SIZE);
    out = new SeeedGrovePWMOutput(SPK_PWM_PIN, 2);
    out->begin();

    mp3 = new AudioGeneratorMP3();
    if (mp3->begin(buff, out)) {
        uint32_t lastLoop = millis();
        while (mp3->isRunning()) {
            if (mp3->loop()) {
                lastLoop = millis();
            }
            if (millis() - lastLoop > 1500) {
                mp3->stop();
                break;
            }
        }
    }

    delete mp3;  mp3 = NULL;
    delete buff; buff = NULL;
    delete file; file = NULL;
    delete out;  out = NULL;

    pinMode(SPK_PWM_PIN, OUTPUT);
    digitalWrite(SPK_PWM_PIN, LOW);
    Serial.println("[TTS] Phát xong câu trả lời.");
}

// ============================================================
// 7. GỬI CÂU HỎI LÊN GEMINI API (TỐI ƯU KHÔNG LỖI SSL/JSON)
// ============================================================
static String sendAudioToGemini() {
    if (active_api_key.length() == 0 || active_api_key == "AIzaSy...") {
        Serial.println("\n[AI] LỖI CHÍ MẠNG: CHƯA ĐIỀN API KEY!");
        return "";
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20);

    Serial.println("[2/3] Đang kết nối tới Gemini API...");
    if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
        Serial.println("[2/3] Kết nối AI thất bại!");
        return "";
    }

    // 1. Ép kích thước PCM về bội số của 3
    size_t rawPcmSize = (AI_AUDIO_SAMPLE_RATE * 2) * sizeof(int16_t); // Ép ghi âm 2 giây = 64000 samples
    size_t pcmSize = (rawPcmSize / 3) * 3;
    size_t wavSize = WAV_HEADER_SIZE + pcmSize;
    
    while (wavSize % 3 != 0) {
        pcmSize -= 2;
        wavSize = WAV_HEADER_SIZE + pcmSize;
    }

    uint8_t *wavBuf = (uint8_t *)ps_malloc(wavSize);
    if (!wavBuf) {
        Serial.println("[2/3] LỖI: Cấp phát PSRAM thất bại!");
        client.stop();
        return "";
    }

    createWavHeader(wavBuf, pcmSize, AI_AUDIO_SAMPLE_RATE, 1, 16);
    memcpy(wavBuf + WAV_HEADER_SIZE, audioRecordBuf, pcmSize);

    // 2. Mã hóa Base64
    Serial.println("[2/3] Đang mã hóa WAV sang Base64 chuẩn...");
    String audioBase64 = base64Encode(wavBuf, wavSize);
    free(wavBuf);
    audioBase64.replace("=", "");

    // 3. Đóng gói Header & Footer
    String jsonHeader = "{\"contents\":[{\"parts\":["
                        "{\"text\":\"Bạn là trợ lý người mù. Hãy nghe âm thanh này và trả lời ngắn gọn dưới 20 từ bằng tiếng Việt.\"},"
                        "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
    String jsonFooter = "\"}}]}]}";

    size_t totalContentLength = jsonHeader.length() + audioBase64.length() + jsonFooter.length();
    String urlPath = String("/v1beta/models/") + TARGET_GEMINI_MODEL + ":generateContent?key=" + active_api_key;

    // 4. Gửi HTTP Headers
    client.printf("POST %s HTTP/1.1\r\n", urlPath.c_str());
    client.printf("Host: %s\r\n", GEMINI_API_HOST);
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %zu\r\n", totalContentLength);
    client.printf("Connection: close\r\n\r\n");

    // 5. Gửi JSON Header
    client.print(jsonHeader);

    // 6. Stream Base64 theo từng Chunk 1024 bytes (Tăng thời gian delay cho TCP Stack)
    Serial.println("[2/3] Đang stream chuỗi Base64 lên Google Server...");
    size_t totalB64Len = audioBase64.length();
    size_t b64Offset = 0;

    while (b64Offset < totalB64Len) {
        size_t chunkSize = (totalB64Len - b64Offset > 1024) ? 1024 : (totalB64Len - b64Offset);
        String chunkStr = audioBase64.substring(b64Offset, b64Offset + chunkSize);
        
        size_t written = client.print(chunkStr);
        if (written == 0) {
            Serial.println("[2/3] Lỗi ghi Socket SSL!");
            client.stop();
            return "";
        }

        b64Offset += chunkSize;
        vTaskDelay(pdMS_TO_TICKS(15)); // Cho phép WiFi & SSL Stack xả đệm
    }

    // 7. Gửi JSON Footer
    client.print(jsonFooter);
    client.flush();

    // 8. Nhận phản hồi (header HTTP + body). Server đóng TLS ngay sau khi gửi xong
    //    vì client gửi "Connection: close" — lỗi SSL (-76) chỉ là close_notify, không phải lỗi.
    unsigned long startWait = millis();
    while (!client.available() && (millis() - startWait < 15000)) {
        delay(100);
    }

    if (!client.available()) {
        Serial.println("[2/3] Hết thời gian chờ phản hồi từ AI!");
        client.stop();
        return "";
    }

    // Đọc hết bytes còn lại trong buffer (kể cả sau khi TLS báo đóng)
    String responseBody = "";
    startWait = millis();
    while ((client.connected() || client.available()) && (millis() - startWait < 10000)) {
        if (client.available()) {
            responseBody += (char)client.read();
            startWait = millis();
        } else {
            delay(5);
        }
    }
    client.stop();

    // Gemini trả HTTP/1.1 chunked transfer-encoding -> body thô chứa dòng
    // độ dài (318) và CRLF quanh JSON. Chỉ lấy phần nằm giữa `{` đầu và `}` cuối.
    int jsonStart = responseBody.indexOf('{');
    int jsonEnd = responseBody.lastIndexOf('}');
    if (jsonStart < 0 || jsonEnd <= jsonStart) {
        Serial.printf("[2/3] Không tìm thấy JSON trong phản hồi (len=%u)\n", responseBody.length());
        Serial.println(responseBody);
        return "";
    }
    String jsonPayload = responseBody.substring(jsonStart, jsonEnd + 1);

    // 9. Parse JSON
    JsonDocument respDoc;
    DeserializationError error = deserializeJson(respDoc, jsonPayload);
    if (error) {
        Serial.printf("[2/3] Lỗi Parse JSON: %s\n", error.c_str());
        Serial.println(jsonPayload);
        return "";
    }

    if (!respDoc["error"].isNull()) {
        const char *errMessage = respDoc["error"]["message"];
        Serial.printf("[2/3] Gemini API Error: %s\n", errMessage ? errMessage : "Unknown Error");
        return "";
    }

    const char *aiText = respDoc["candidates"][0]["content"]["parts"][0]["text"];
    if (aiText) {
        String replyStr = String(aiText);
        replyStr.trim();
        Serial.printf("[2/3] AI Trả lời: \"%s\"\n", replyStr.c_str());
        return replyStr;
    }

    return "";
}

// ============================================================
// HÀM HỖ TRỢ KẾT NỐI WIFI TỰ ĐỘNG
// ============================================================
static bool autoConnectWiFi() {
    if (strlen(USER_WIFI_SSID) > 0) {
        active_ssid = USER_WIFI_SSID;
        active_pass = USER_WIFI_PASS;
        Serial.printf("[WIFI] Kết nối Wi-Fi thủ công: \"%s\"...\n", active_ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(active_ssid.c_str(), active_pass.c_str());
        
        int retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 30) {
            delay(300); Serial.print("."); retry++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WIFI] Kết nối thành công! IP: %s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        Serial.println("\n[WIFI] Thất bại!");
    }

    struct WiFiCred { const char* ssid; const char* pass; };
    WiFiCred creds[] = {
        {WIFI_SSID,  WIFI_PASS},
        {WIFI_SSID2, WIFI_PASS2},
        {WIFI_SSID3, WIFI_PASS3}
    };

    WiFi.mode(WIFI_STA);
    for (int i = 0; i < 3; i++) {
        if (creds[i].ssid != NULL && strlen(creds[i].ssid) > 0) {
            Serial.printf("[WIFI] Thử kết nối config.h #%d: \"%s\"...\n", i + 1, creds[i].ssid);
            WiFi.begin(creds[i].ssid, creds[i].pass);
            
            int retry = 0;
            while (WiFi.status() != WL_CONNECTED && retry < 25) {
                delay(300); Serial.print("."); retry++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                active_ssid = String(creds[i].ssid);
                active_pass = String(creds[i].pass);
                Serial.printf("\n[WIFI] Kết nối thành công! IP: %s\n", WiFi.localIP().toString().c_str());
                return true;
            }
            Serial.println("\n[WIFI] Thất bại!");
        }
    }

    return false;
}

// ============================================================
// SETUP & LOOP CHÍNH
// ============================================================
void setup() {
    pinMode(SPK_PWM_PIN, OUTPUT);
    digitalWrite(SPK_PWM_PIN, LOW);

    Serial.begin(SERIAL_BAUD);
    delay(500);

    Serial.println("\n=======================================================");
    Serial.println("  AEGIS SIGHT - MIC AUDIO -> GEMINI AI -> GOOGLE TTS  ");
    Serial.println("=======================================================");

    if (strlen(USER_GEMINI_API_KEY) > 0 && String(USER_GEMINI_API_KEY) != "AIzaSy...") {
        active_api_key = USER_GEMINI_API_KEY;
    } else {
        active_api_key = GEMINI_API_KEY;
    }

    if (!psramFound()) {
        Serial.println("[FATAL] Không tìm thấy PSRAM!");
        return;
    }

    audioRecordBuf = (int16_t *)ps_malloc(TOTAL_SAMPLES * sizeof(int16_t));
    if (!audioRecordBuf) {
        Serial.println("[FATAL] Cấp phát PSRAM lưu Audio thất bại!");
        return;
    }

    if (!autoConnectWiFi()) {
        Serial.println("[WIFI] Thất bại! Dừng chương trình.");
        return;
    }

    if (!initMicI2S()) {
        Serial.println("[MIC] Khởi tạo Mic I2S Thất bại!");
        return;
    }

    Serial.println("\n[SYSTEM] Hệ thống sẵn sàng! Bắt đầu tiến trình tự động sau 2s...");
    delay(2000);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Mất kết nối, đang thử kết nối lại...");
        WiFi.begin(active_ssid.c_str(), active_pass.c_str());
        delay(2000);
        return;
    }

    // 1. Thu âm từ Mic INMP441 trong 3s
    if (recordAudio3Seconds()) {
        
        // 2. Gửi Audio thu được lên Gemini API
        String aiReply = sendAudioToGemini();
        
        // 3. Phát âm thanh câu trả lời ra loa
        if (aiReply.length() > 0) {
            playGoogleTTS(aiReply, "vi");
        } else {
            Serial.println("[AI] Không nhận được câu trả lời từ Gemini.");
        }
    }

    Serial.println("\n--- Hoàn thành 1 chu kỳ. Chờ 5 giây cho chu kỳ tiếp theo ---");
    delay(5000);
}

#endif // ENABLE_MIC_AI_TEST