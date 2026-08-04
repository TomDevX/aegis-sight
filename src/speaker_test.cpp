#include <Arduino.h>
#include <WiFi.h>
#include "config.h"

#ifdef ENABLE_SPEAKER_TEST

#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h"

// -----------------------------------------------------------------------------
// BỘ XUẤT PWM TỐI ĐA ÂM LƯỢNG (MAX VOLUME) & TRIỆT TIÊU POP NOISE
// -----------------------------------------------------------------------------
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
        _channel = channel; // Channel 2 tránh đụng Camera (Channel 0)
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
        // Tần số PWM 31.25 kHz (8-bit)
        ledcSetup(_channel, 31250, 8);
        ledcAttachPin(_pin, _channel);
        
        // CHỐNG BỤP/NỔ KHỦNG KHIẾM ĐẦU KHÚC: 
        // Đưa DC Bias từ 0 -> 128 với bước nhảy cực mịn 1us để màng loa không bị giật
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

        // Trộn L/R thành Mono
        int32_t pcm = ((int32_t)sample[0] + (int32_t)sample[1]) / 2;
        
        // --- MAX VOLUME GAIN (x3.5) ---
        // Đẩy công suất âm thanh lên kịch biên độ 16-bit
        pcm = (pcm * 7) / 2;
        if (pcm > 32767)  pcm = 32767;
        if (pcm < -32768) pcm = -32768;

        // Chuyển PCM 16-bit signed -> Duty Cycle 8-bit unsigned (0 -> 255)
        uint8_t duty = (uint8_t)(((pcm + 32768) >> 8) & 0xFF);

        // Đồng bộ thời gian thực 24kHz
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
        
        // Xả DC Bias từ 128 -> 0 cực mịn chống nổ cuối câu
        for (int b = 128; b >= 0; b--) {
            ledcWrite(_channel, b);
            delayMicroseconds(100);
        }

        ledcDetachPin(_pin);
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW); // Khóa chân về GND khi rảnh
        return true;
    }
};

// -----------------------------------------------------------------------------
// KHAI BÁO BIẾN TOÀN CỤC
// -----------------------------------------------------------------------------
static AudioGeneratorMP3 *mp3 = NULL;
static AudioFileSourceHTTPStream *file = NULL;
static AudioFileSourceBuffer *buff = NULL;
static SeeedGrovePWMOutput *out = NULL;

static uint32_t lastLoopTime = 0;

static String sampleText = "Xin chào, đây là thử nghiệm phát âm thanh từ Google Translate trên loa Grove";

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
    String url = "http://translate.google.com/translate_tts?ie=UTF-8&q=" 
               + urlEncode(text) 
               + "&tl=" + lang 
               + "&client=tw-ob";

    Serial.printf("\n[TTS] Request URL: %s\n", url.c_str());

    file = new AudioFileSourceHTTPStream(url.c_str());
    buff = new AudioFileSourceBuffer(file, TTS_MP3_BUF_SIZE);
    
    out = new SeeedGrovePWMOutput(SPK_PWM_PIN, 2);
    out->begin();

    mp3 = new AudioGeneratorMP3();
    if (mp3->begin(buff, out)) {
        Serial.println("[TTS] Đang phát tiếng nói qua loa Grove...");
        lastLoopTime = millis();
    } else {
        Serial.println("[TTS] Lỗi khởi tạo MP3!");
    }
}

// -----------------------------------------------------------------------------
// SETUP & LOOP
// -----------------------------------------------------------------------------
void setup() {
    // Ép chân loa về LOW ngay giây đầu tiên khi vừa cấp nguồn
    pinMode(SPK_PWM_PIN, OUTPUT);
    digitalWrite(SPK_PWM_PIN, LOW);

    Serial.begin(SERIAL_BAUD);
    delay(500);

    Serial.println("\n=== THỬ NGHIỆM LOA GROVE - GOOGLE TRANSLATE TTS ===");

    WiFi.begin("HCMUT.EDU", "Suong72730109");
    Serial.print("Đang kết nối WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print(".");
    }
    Serial.printf("\nWiFi đã kết nối! IP: %s\n", WiFi.localIP().toString().c_str());

    delay(1000); 
    playGoogleTTS(sampleText, "vi");
}

void loop() {
    if (mp3 && mp3->isRunning()) {
        bool running = mp3->loop();
        if (running) {
            lastLoopTime = millis();
        }

        if (!running || (millis() - lastLoopTime > 1500)) {
            mp3->stop();
            Serial.println("[TTS] Đã phát xong!");

            delete mp3;  mp3 = NULL;
            delete buff; buff = NULL;
            delete file; file = NULL;
            delete out;  out = NULL;
            
            pinMode(SPK_PWM_PIN, OUTPUT);
            digitalWrite(SPK_PWM_PIN, LOW);
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(5000));
        Serial.println("[TTS] Tiến hành phát lại...");
        playGoogleTTS("Thử nghiệm phát lại âm thanh thành công.", "vi");
    }
}

#endif // ENABLE_SPEAKER_TEST