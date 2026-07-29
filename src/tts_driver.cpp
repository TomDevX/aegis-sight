#include "tts_driver.h"
#include "tone_driver.h"

#ifdef ENABLE_TTS_CLOUD

#include <WiFi.h>
#include "libhelix-mp3/mp3dec.h"
#include <string.h>

static volatile bool ttsBusy = false;
static volatile bool ttsCancel = false;

static void url_encode(const char *src, size_t len, char *dst, size_t dstLen) {
    size_t p = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len && p < dstLen - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[p++] = (char)c;
        } else if (c == ' ') {
            dst[p++] = '+';
        } else {
            if (p + 3 > dstLen - 1) break;
            dst[p++] = '%';
            dst[p++] = hex[c >> 4];
            dst[p++] = hex[c & 0x0F];
        }
    }
    dst[p] = '\0';
}

static size_t resample_pcm(const int16_t *in, size_t inLen, int16_t *out,
                           size_t outLen, uint32_t inRate, uint32_t outRate) {
    if (inRate == outRate || inLen < 2) {
        size_t copy = (inLen < outLen) ? inLen : outLen;
        memcpy(out, in, copy * sizeof(int16_t));
        return copy;
    }
    uint32_t pos = 0;
    uint32_t step = ((uint32_t)inRate << 16) / outRate;
    size_t outPos = 0;
    while (outPos < outLen) {
        uint32_t idx = pos >> 16;
        if (idx + 1 >= inLen) break;
        uint32_t frac = pos & 0xFFFF;
        int32_t s = (int32_t)in[idx] + (((int32_t)(in[idx + 1] - in[idx]) * (int32_t)frac) >> 16);
        out[outPos++] = (int16_t)s;
        pos += step;
    }
    return outPos;
}

bool tts_driver_init(void) {
    return true;
}

void tts_driver_speak(const char *text, size_t len) {
    if (!text || len == 0) return;
    ttsBusy = true;
    ttsCancel = false;

    uint8_t *mp3Buf = (uint8_t *)ps_malloc(TTS_MP3_BUF_SIZE);
    if (!mp3Buf) { ttsBusy = false; return; }

    size_t urlBufLen = len * 3 + 256;
    char *urlEnc = (char *)malloc(urlBufLen);
    if (!urlEnc) { free(mp3Buf); ttsBusy = false; return; }
    url_encode(text, len, urlEnc, urlBufLen);

    char path[768];
    snprintf(path, sizeof(path),
        "/translate_tts?ie=UTF-8&tl=vi&sl=vi&client=gtx&q=%s", urlEnc);
    free(urlEnc);

    WiFiClient client;
    client.setTimeout(5000);

    if (!client.connect("translate.google.com", 80)) {
        Serial.println("[TTS] Connection failed");
        free(mp3Buf); ttsBusy = false; return;
    }

    String req = "GET " + String(path) + " HTTP/1.1\r\n"
                 "Host: translate.google.com\r\n"
                 "User-Agent: Mozilla/5.0\r\n"
                 "Connection: close\r\n\r\n";
    client.print(req);

    bool headerDone = false;
    uint32_t tmo = millis() + 5000;
    char hdr[256];
    while (client.connected() && millis() < tmo) {
        size_t n = client.readBytesUntil('\n', (uint8_t *)hdr, sizeof(hdr) - 1);
        if (n == 0) continue;
        hdr[n] = '\0';
        while (n > 0 && (hdr[n - 1] == '\r' || hdr[n - 1] == ' ')) hdr[--n] = '\0';
        if (n == 0) { headerDone = true; break; }
    }
    if (!headerDone) { client.stop(); free(mp3Buf); ttsBusy = false; return; }

    size_t mp3Len = 0;
    while (client.connected() && mp3Len < TTS_MP3_BUF_SIZE) {
        int room = TTS_MP3_BUF_SIZE - mp3Len;
        int got = client.read(mp3Buf + mp3Len, room > 512 ? 512 : room);
        if (got <= 0) break;
        mp3Len += got;
    }
    client.stop();

    if (mp3Len == 0) { free(mp3Buf); ttsBusy = false; return; }
    Serial.printf("[TTS] Downloaded %zu bytes MP3\n", mp3Len);

    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) { free(mp3Buf); ttsBusy = false; return; }

    tone_driver_stream_set_active(true);

    unsigned char *inbuf = mp3Buf;
    int bytesLeft = (int)mp3Len;
    short outBuf[1152 * 2];
    int sampleRate = 24000;

    while (bytesLeft > 0 && !ttsCancel) {
        int result = MP3Decode(decoder, &inbuf, &bytesLeft, outBuf, 0);
        if (result == ERR_MP3_NONE) {
            MP3FrameInfo info;
            MP3GetLastFrameInfo(decoder, &info);
            if (info.nChans > 0 && info.samprate > 0) sampleRate = info.samprate;
            int samples = info.outputSamps / info.nChans;

            int16_t monoBuf[1152];
            if (info.nChans == 2) {
                for (int i = 0; i < samples; i++)
                    monoBuf[i] = (int16_t)(((int32_t)outBuf[i * 2] + (int32_t)outBuf[i * 2 + 1]) >> 1);
            } else {
                memcpy(monoBuf, outBuf, samples * sizeof(int16_t));
            }

            int16_t playBuf[1152];
            size_t playLen;
            if (sampleRate != 16000) {
                playLen = resample_pcm(monoBuf, samples, playBuf, 1152, sampleRate, 16000);
            } else {
                memcpy(playBuf, monoBuf, samples * sizeof(int16_t));
                playLen = samples;
            }

            size_t written = 0;
            while (written < playLen && !ttsCancel) {
                size_t chunk = playLen - written;
                if (chunk > 256) chunk = 256;
                while (!tone_driver_stream_write(playBuf + written, chunk) && !ttsCancel) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
                written += chunk;
            }
        } else if (result == ERR_MP3_INDATA_UNDERFLOW || result == ERR_MP3_MAINDATA_UNDERFLOW) {
            break;
        } else if (result == ERR_MP3_FREE_BITRATE_SYNC) {
            continue;
        } else {
            int sync = MP3FindSyncWord(inbuf, bytesLeft);
            if (sync >= 0) { inbuf += sync; bytesLeft -= sync; continue; }
            break;
        }
    }

    MP3FreeDecoder(decoder);
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
    tone_driver_stream_set_active(false);
}

#endif
