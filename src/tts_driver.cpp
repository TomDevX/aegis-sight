#include "tts_driver.h"
#include "tone_driver.h"

#ifdef ENABLE_TTS_CLOUD

#include <WiFi.h>
#include "AudioFileSourcePROGMEM.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h"

#ifndef TTS_MP3_BUF_SIZE
#define TTS_MP3_BUF_SIZE      (48 * 1024) // 48KB RAM buffer cho file MP3
#endif

static volatile bool ttsBusy = false;
static volatile bool ttsCancel = false;

static String url_encode_str(const String &str) {
    String encoded = "";
    char c;
    char code0, code1;
    for (size_t i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (c == ' ') {
            encoded += '+';
        } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            code0 = (c >> 4) & 0xf;
            code1 = c & 0xf;
            encoded += '%';
            encoded += (char)(code0 > 9 ? code0 + 'A' - 10 : code0 + '0');
            encoded += (char)(code1 > 9 ? code1 + 'A' - 10 : code1 + '0');
        }
    }
    return encoded;
}

// AudioOutput chuyển tiếp mẫu PCM từ AudioGeneratorMP3 vào stream ring buffer
class AudioOutputToToneDriver : public AudioOutput {
private:
    int16_t monoChunk[128];
    size_t chunkIdx;

public:
    AudioOutputToToneDriver() : chunkIdx(0) {}

    virtual bool begin() override {
        chunkIdx = 0;
        tone_driver_stream_set_active(true);
        return true;
    }

    virtual bool SetRate(int hz) override {
        return true;
    }

    virtual bool SetBitsPerSample(int bits) override {
        return true;
    }

    virtual bool SetChannels(int channels) override {
        return true;
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        // Gộp 2 kênh stereo thành mono chuẩn
        int16_t mono = (int16_t)(((int32_t)sample[0] + (int32_t)sample[1]) >> 1);
        monoChunk[chunkIdx++] = mono;

        if (chunkIdx >= 128) {
            size_t written = 0;
            while (written < 128 && !ttsCancel) {
                if (tone_driver_stream_write(monoChunk + written, 128 - written)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            chunkIdx = 0;
        }
        return true;
    }

    virtual bool stop() override {
        if (chunkIdx > 0 && !ttsCancel) {
            size_t written = 0;
            while (written < chunkIdx && !ttsCancel) {
                if (tone_driver_stream_write(monoChunk + written, chunkIdx - written)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            chunkIdx = 0;
        }
        return true;
    }
};

bool tts_driver_init(void) {
    return true;
}

void tts_driver_speak(const char *text, size_t len) {
    if (!text || len == 0) return;
    ttsBusy = true;
    ttsCancel = false;

    // 1. Cấp phát buffer tải MP3
    uint8_t *mp3Buf = (uint8_t *)ps_malloc(TTS_MP3_BUF_SIZE);
    if (!mp3Buf) mp3Buf = (uint8_t *)malloc(TTS_MP3_BUF_SIZE);
    if (!mp3Buf) { ttsBusy = false; return; }

    String enc = url_encode_str(String(text));
    String path = "/translate_tts?ie=UTF-8&tl=vi&client=tw-ob&q=" + enc;

    Serial.printf("[TTS] Đang tải & phát: \"%.40s...\"\n", text);

    size_t mp3Len = 0;

    // 2. Tải toàn bộ MP3 vào RAM qua WiFiClient có timeout chặt chẽ
    for (int retry = 0; retry < 2 && mp3Len == 0 && !ttsCancel; retry++) {
        WiFiClient client;
        client.setTimeout(2500);

        if (!client.connect("translate.google.com", 80)) {
            Serial.printf("[TTS] Ket noi that bai (lan %d)\n", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        String req = "GET " + path + " HTTP/1.1\r\n"
                     "Host: translate.google.com\r\n"
                     "User-Agent: Mozilla/5.0\r\n"
                     "Connection: close\r\n\r\n";
        client.print(req);

        // Bỏ qua HTTP Headers
        bool headerDone = false;
        bool is200 = false;
        uint32_t tmo = millis() + 3000;
        char hdr[256];
        bool firstLine = true;

        while (client.connected() && millis() < tmo && !ttsCancel) {
            size_t n = client.readBytesUntil('\n', (uint8_t *)hdr, sizeof(hdr) - 1);
            if (n == 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
            hdr[n] = '\0';
            while (n > 0 && (hdr[n-1] == '\r' || hdr[n-1] == ' ')) hdr[--n] = '\0';

            if (firstLine) {
                firstLine = false;
                if (strstr(hdr, "200") != NULL) is200 = true;
            }
            if (n == 0) { headerDone = true; break; }
        }

        if (!headerDone || !is200 || ttsCancel) {
            client.stop();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Tải body MP3
        while (mp3Len < TTS_MP3_BUF_SIZE && !ttsCancel) {
            if (client.available()) {
                int room = TTS_MP3_BUF_SIZE - mp3Len;
                int got = client.read(mp3Buf + mp3Len, room > 1024 ? 1024 : room);
                if (got <= 0) break;
                mp3Len += got;
            } else if (!client.connected()) {
                break;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        client.stop();
    }

    if (mp3Len == 0 || ttsCancel) {
        free(mp3Buf);
        ttsBusy = false;
        return;
    }

    Serial.printf("[TTS] Tải xong %zu bytes MP3. Bắt đầu giải mã...\n", mp3Len);

    // 3. Giải mã từ RAM bằng AudioFileSourcePROGMEM (100% không bị nghẽn mạng trong lúc decode)
    AudioFileSourcePROGMEM *file = new AudioFileSourcePROGMEM(mp3Buf, mp3Len);
    AudioOutputToToneDriver *out = new AudioOutputToToneDriver();
    AudioGeneratorMP3 *mp3 = new AudioGeneratorMP3();

    out->begin();

    if (mp3->begin(file, out)) {
        while (mp3->isRunning() && !ttsCancel) {
            if (!mp3->loop()) {
                break;
            }
            // Nhả CPU cho IDLE0 task để không bao giờ bị watchdog TWDT abort
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    } else {
        Serial.println("[TTS] mp3->begin thất bại!");
    }

    out->stop();

    delete mp3;
    delete file;
    delete out;
    free(mp3Buf);

    ttsBusy = false;
}

bool tts_driver_is_busy(void) {
    return ttsBusy;
}

void tts_driver_stop(void) {
    ttsCancel = true;
    ttsBusy = false;
    tone_driver_stream_set_active(false);
}

void tts_driver_wait_playback_done(void) {
    uint32_t tmo = millis() + 30000;
    while (tone_driver_stream_available() > 0 && millis() < tmo) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Chờ 150ms để DMA buffer phát hết nốt âm thanh cuối cùng trước khi đóng stream
    vTaskDelay(pdMS_TO_TICKS(150));
    tone_driver_stream_set_active(false);
}

#endif