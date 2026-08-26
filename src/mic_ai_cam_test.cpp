#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "config.h"
#include "driver/i2s.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "mbedtls/base64.h" // Thư viện Base64 chuẩn của ESP-IDF

#if defined(ENABLE_MIC_AI_CAM_TEST)

#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h"

// ============================================================================
//  USER CONFIGURATION HEADER
// ============================================================================
#define USER_WIFI_SSID        ""
#define USER_WIFI_PASS        ""

#define USER_GEMINI_API_KEY   ""

#define RECORD_TIME_SEC       3
#define AUDIO_GAIN            2.0f
#define JPEG_QUALITY          70
#define TARGET_GEMINI_MODEL   "gemini-3.5-flash-lite"

// ============================================================================
#define TOTAL_SAMPLES         (AI_AUDIO_SAMPLE_RATE * RECORD_TIME_SEC)
#define WAV_HEADER_SIZE       44

static int16_t *audioRecordBuf = NULL;
static uint8_t *jpgBuf = NULL;
static size_t jpgLen = 0;
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
// 2. LOA QUA MAX98357A I2S CLASS-D AMPLIFIER OUTPUT CLASS
//    DIN=GPIO39, LRC=GPIO40, BCLK=GPIO41 (I2S_SPK_PORT)
//    ESP32-S3 làm Master TX, MAX98357A decode I2S -> analog -> loa Grove
// ============================================================
class Max98357I2SOutput : public AudioOutput {
private:
    int32_t _sample_rate;
    bool _installed;

    bool installDriver() {
        i2s_config_t i2s_cfg = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = (uint32_t)_sample_rate,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 8,
            .dma_buf_len = 256,
            .use_apll = false,
        };

        i2s_pin_config_t pin_cfg = {
            .bck_io_num = AMP_I2S_BCLK,
            .ws_io_num = AMP_I2S_LRC,
            .data_out_num = AMP_I2S_DIN,
            .data_in_num = I2S_PIN_NO_CHANGE,
        };

        if (i2s_driver_install(I2S_SPK_PORT, &i2s_cfg, 0, NULL) != ESP_OK) return false;
        if (i2s_set_pin(I2S_SPK_PORT, &pin_cfg) != ESP_OK) {
            i2s_driver_uninstall(I2S_SPK_PORT);
            return false;
        }
        i2s_zero_dma_buffer(I2S_SPK_PORT);
        return true;
    }

public:
    Max98357I2SOutput() {
        _sample_rate = 24000;
        _installed = false;
    }

    virtual bool SetRate(int hz) override {
        if (hz <= 0) return false;
        _sample_rate = hz;
        if (_installed) {
            // MAX98357A tự đồng hồ theo BCLK, chỉ cần đổi sample rate master
            if (i2s_set_sample_rates(I2S_SPK_PORT, hz) != ESP_OK) return false;
        }
        return true;
    }

    virtual bool begin() override {
        if (!_installed) {
            if (!installDriver()) {
                Serial.println("[AMP] Lỗi khởi tạo I2S cho MAX98357A!");
                return false;
            }
            _installed = true;
        }
        // Im lặng ban đầu để tránh tiếng "pop" khi bật amp
        size_t written = 0;
        static const int16_t silence[64] = {0};
        for (int i = 0; i < 16; i++) {
            i2s_write(I2S_SPK_PORT, silence, sizeof(silence), &written, portMAX_DELAY);
        }
        Serial.printf("[AMP] MAX98357A sẵn sàng (I2S %ld Hz, DIN=%d LRC=%d BCLK=%d)\n",
                      _sample_rate, AMP_I2S_DIN, AMP_I2S_LRC, AMP_I2S_BCLK);
        return true;
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!_installed) return true;

        // Trộn stereo -> mono rồi phát trên cả 2 kênh (loa Grove 1 chiều)
        int32_t mono = ((int32_t)sample[0] + (int32_t)sample[1]) / 2;
        int16_t frame[2] = { (int16_t)mono, (int16_t)mono };

        size_t written = 0;
        // Block chờ DMA trống — driver MP3 gọi lại liên tục nên không cần busy-wait
        if (i2s_write(I2S_SPK_PORT, frame, sizeof(frame), &written, portMAX_DELAY) != ESP_OK) {
            return false;
        }
        return true;
    }

    virtual bool stop() override {
        if (_installed) {
            // Phát im lặng để xả DMA, tránh tắt đột ngột gây tiếng "click"
            size_t written = 0;
            static const int16_t silence[64] = {0};
            for (int i = 0; i < 8; i++) {
                i2s_write(I2S_SPK_PORT, silence, sizeof(silence), &written, portMAX_DELAY);
            }
            i2s_driver_uninstall(I2S_SPK_PORT);
            _installed = false;
        }
        return true;
    }
};

static AudioGeneratorMP3 *mp3 = NULL;
static AudioFileSourceHTTPStream *file = NULL;
static AudioFileSourceBuffer *buff = NULL;
static Max98357I2SOutput *out = NULL;

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
// 5. KHỞI TẠO CAMERA (GC2145 - RGB565 -> JPEG bằng fmt2jpg)
// ============================================================
static bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_Y2;
    config.pin_d1       = CAM_Y3;
    config.pin_d2       = CAM_Y4;
    config.pin_d3       = CAM_Y5;
    config.pin_d4       = CAM_Y6;
    config.pin_d5       = CAM_Y7;
    config.pin_d6       = CAM_Y8;
    config.pin_d7       = CAM_Y9;
    config.pin_xclk     = CAM_XCLK;
    config.pin_pclk     = CAM_PCLK;
    config.pin_vsync    = CAM_VSYNC;
    config.pin_href     = CAM_HREF;
    config.pin_sccb_sda = CAM_SIOD;
    config.pin_sccb_scl = CAM_SIOC;
    config.pin_pwdn     = CAM_PWDN;
    config.pin_reset    = CAM_RESET;

    config.xclk_freq_hz = 15000000;
    config.pixel_format = PIXFORMAT_RGB565;   
    config.frame_size   = FRAMESIZE_QVGA;     // 320x240
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init thất bại! (0x%x)\n", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_exposure_ctrl(s, 1);   
        s->set_gain_ctrl(s, 1);       
        s->set_whitebal(s, 1);        
        s->set_aec2(s, 1);            
    }
    Serial.println("[CAM] Camera khởi tạo thành công (QVGA RGB565)");
    return true;
}

// ============================================================
// 6. CHỤP ẢNH VÀ NÉN JPEG PHẦN MỀM (fmt2jpg)
// ============================================================
static bool captureJpeg() {
    if (jpgBuf) {
        free(jpgBuf);
        jpgBuf = NULL;
        jpgLen = 0;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[CAM] Lỗi esp_camera_fb_get!");
        return false;
    }

    uint16_t w = fb->width;
    uint16_t h = fb->height;
    bool ok = fmt2jpg(fb->buf, fb->len, w, h, PIXFORMAT_RGB565, JPEG_QUALITY, &jpgBuf, &jpgLen);
    esp_camera_fb_return(fb);

    if (!ok || jpgLen == 0) {
        Serial.println("[CAM] Lỗi nén JPEG (fmt2jpg)!");
        return false;
    }

    Serial.printf("[CAM] Đã chụp: %ux%u -> JPEG %u bytes\n", w, h, (unsigned)jpgLen);
    return true;
}

// ============================================================
// 7. MÃ HÓA BASE64 CHUẨN (SỬ DỤNG MBEDTLS AN TOÀN TUYỆT ĐỐI)
// ============================================================
static bool base64Encode(const uint8_t *data, size_t length, uint8_t **outBuf, size_t *outLen) {
    if (!data || length == 0) return false;

    size_t expected_len = ((length + 2) / 3) * 4 + 1;
    uint8_t *buf = (uint8_t *)ps_malloc(expected_len);
    if (!buf) return false;

    size_t olen = 0;
    int ret = mbedtls_base64_encode(buf, expected_len, &olen, data, length);
    if (ret != 0) {
        free(buf);
        return false;
    }

    buf[olen] = '\0';
    *outBuf = buf;
    *outLen = olen;
    return true;
}

// ============================================================
// 8. PHÁT TIẾNG QUA GOOGLE TTS LOA GROVE
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
    out = new Max98357I2SOutput();
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

    Serial.println("[TTS] Phát xong câu trả lời.");
}

// ============================================================
// 9. STREAM BASE64 TỪ PSRAM LÊN SOCKET SSL (KÈM KIỂM TRA NULL)
// ============================================================
static bool streamToClient(WiFiClientSecure &client, const uint8_t *data, size_t len) {
    if (!data || len == 0) return false; // Chống crash LoadProhibited

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > 1024) ? 1024 : (len - offset);
        size_t written = client.write(data + offset, chunk);
        if (written == 0) return false;
        offset += written;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return true;
}

// ============================================================
// 10. GỬI ẢNH + ÂM THANH LÊN GEMINI API
// ============================================================
static String sendAudioImageToGemini() {
    if (active_api_key.length() == 0 || active_api_key == "AIzaSy...") {
        Serial.println("\n[AI] LỖI CHÍ MẠNG: CHƯA ĐIỀN API KEY!");
        return "";
    }

    if (jpgBuf == NULL || jpgLen == 0) {
        Serial.println("[AI] Chưa có ảnh để gửi!");
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

    // 1. Tạo WAV 2 giây từ PCM
    size_t rawPcmSize = (AI_AUDIO_SAMPLE_RATE * 2) * sizeof(int16_t);
    size_t pcmSize = (rawPcmSize / 3) * 3;
    size_t wavSize = WAV_HEADER_SIZE + pcmSize;

    while (wavSize % 3 != 0) {
        pcmSize -= 2;
        wavSize = WAV_HEADER_SIZE + pcmSize;
    }

    uint8_t *wavBuf = (uint8_t *)ps_malloc(wavSize);
    if (!wavBuf) {
        Serial.println("[2/3] LỖI: Cấp phát PSRAM lưu WAV thất bại!");
        client.stop();
        return "";
    }

    createWavHeader(wavBuf, pcmSize, AI_AUDIO_SAMPLE_RATE, 1, 16);
    memcpy(wavBuf + WAV_HEADER_SIZE, audioRecordBuf, pcmSize);

    // 2. Mã hóa WAV -> Base64
    Serial.println("[2/3] Đang mã hóa WAV sang Base64...");
    uint8_t *audB64 = NULL; size_t audB64Len = 0;
    if (!base64Encode(wavBuf, wavSize, &audB64, &audB64Len)) {
        Serial.println("[2/3] LỖI: Mã hóa Base64 audio thất bại!");
        free(wavBuf);
        client.stop();
        return "";
    }
    free(wavBuf);

    // 3. Mã hóa Ảnh JPEG -> Base64
    Serial.println("[2/3] Đang mã hóa Ảnh JPEG sang Base64...");
    uint8_t *imgB64 = NULL; size_t imgB64Len = 0;
    if (!base64Encode(jpgBuf, jpgLen, &imgB64, &imgB64Len)) {
        Serial.println("[2/3] LỖI: Mã hóa Base64 ảnh thất bại!");
        free(audB64);
        client.stop();
        return "";
    }

    // 4. Đóng gói JSON
    String jsonHeader = "{\"contents\":[{\"parts\":["
                        "{\"text\":\"Bạn là trợ lý người mù. Hãy nhìn hình ảnh và nghe âm thanh câu hỏi, trả lời ngắn gọn dưới 100 từ bằng tiếng Việt.\"},"
                        "{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"";
    String jsonMid     = "\"}},{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
    String jsonFooter  = "\"}}]}]}";

    size_t totalContentLength = jsonHeader.length() + imgB64Len + jsonMid.length() + audB64Len + jsonFooter.length();
    String urlPath = String("/v1beta/models/") + TARGET_GEMINI_MODEL + ":generateContent?key=" + active_api_key;

    // 5. Gửi HTTP Headers
    client.printf("POST %s HTTP/1.1\r\n", urlPath.c_str());
    client.printf("Host: %s\r\n", GEMINI_API_HOST);
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %zu\r\n", totalContentLength);
    client.printf("Connection: close\r\n\r\n");

    // 6. Stream JSON
    Serial.printf("[2/3] Đang stream Ảnh(%u) + Audio(%u) lên Google Server...\n",
                  (unsigned)imgB64Len, (unsigned)audB64Len);

    bool streamOk = true;
    client.print(jsonHeader);
    streamOk = streamOk && streamToClient(client, imgB64, imgB64Len);
    client.print(jsonMid);
    streamOk = streamOk && streamToClient(client, audB64, audB64Len);
    client.print(jsonFooter);
    client.flush();

    free(imgB64);
    free(audB64);

    if (!streamOk) {
        Serial.println("[2/3] Lỗi ghi Socket SSL!");
        client.stop();
        return "";
    }

    // 7. Nhận phản hồi từ Google
    unsigned long startWait = millis();
    while (!client.available() && (millis() - startWait < 20000)) {
        delay(100);
    }

    if (!client.available()) {
        Serial.println("[2/3] Hết thời gian chờ phản hồi từ AI!");
        client.stop();
        return "";
    }

    String responseBody = "";
    startWait = millis();
    while ((client.connected() || client.available()) && (millis() - startWait < 12000)) {
        if (client.available()) {
            responseBody += (char)client.read();
            startWait = millis();
        } else {
            delay(5);
        }
    }
    client.stop();

    // 8. Parse JSON & Trích xuất câu trả lời
    String replyStr = "";
    int jsonStart = responseBody.indexOf('{');
    int jsonEnd = responseBody.lastIndexOf('}');
    
    if (jsonStart >= 0 && jsonEnd > jsonStart) {
        String jsonPayload = responseBody.substring(jsonStart, jsonEnd + 1);
        JsonDocument respDoc;
        DeserializationError error = deserializeJson(respDoc, jsonPayload);
        
        if (!error && !respDoc["candidates"][0]["content"]["parts"][0]["text"].isNull()) {
            replyStr = respDoc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
        }
    }

    // Fallback Regex/String Search nếu JSON dính Chunk
    if (replyStr.length() == 0) {
        int textIdx = responseBody.indexOf("\"text\":");
        if (textIdx != -1) {
            int startQuote = responseBody.indexOf("\"", textIdx + 7);
            int endQuote = responseBody.indexOf("\"", startQuote + 1);
            if (startQuote != -1 && endQuote != -1) {
                replyStr = responseBody.substring(startQuote + 1, endQuote);
            }
        }
    }

    if (replyStr.length() > 0) {
        replyStr.trim();
        Serial.printf("[2/3] AI Trả lời: \"%s\"\n", replyStr.c_str());
        return replyStr;
    }

    Serial.println("[2/3] Lỗi Parse câu trả lời từ AI!");
    Serial.println(responseBody.substring(0, 600));
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
    Serial.begin(SERIAL_BAUD);
    delay(500);

    Serial.println("\n=======================================================");
    Serial.println(" AEGIS SIGHT - MIC + CAM -> GEMINI -> TTS -> MAX98357A ");
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

    if (!initCamera()) {
        Serial.println("[CAM] Thất bại! Dừng chương trình.");
        return;
    }

    if (!initMicI2S()) {
        Serial.println("[MIC] Khởi tạo Mic I2S Thất bại!");
        return;
    }

    if (!autoConnectWiFi()) {
        Serial.println("[WIFI] Thất bại! Dừng chương trình.");
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
    if (!recordAudio3Seconds()) return;

    // 2. Chụp ảnh từ Camera (RGB565 -> JPEG)
    if (!captureJpeg()) return;

    // 3. Gửi Ảnh + Âm thanh lên Gemini API
    String aiReply = sendAudioImageToGemini();

    // 4. Phát âm thanh câu trả lời ra loa
    if (aiReply.length() > 0) {
        playGoogleTTS(aiReply, "vi");
    } else {
        Serial.println("[AI] Không nhận được câu trả lời từ Gemini.");
    }

    Serial.println("\n--- Hoàn thành 1 chu kỳ. Chờ 5 giây cho chu kỳ tiếp theo ---");
    delay(5000);
}

#endif // ENABLE_MIC_AI_CAM_TEST