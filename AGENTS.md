# Aegis Sight — Agent Guide

## Build & flash

```powershell
pio run -t upload          # compile + flash via USB
pio device monitor -b 115200  # serial console
```

Single environment: `esp32-s3-devkitc-1` (Arduino framework, 16MB Flash QIO, 8MB OPI PSRAM).

## Architecture

- **FreeRTOS dual-core**: Core 0 = Wi-Fi + HTTPS (Gemini SSE) → Google TTS (HTTP); Core 1 = button monitor + camera capture.
- **No `delay()`** — use `vTaskDelay()` or `millis()`-based timers. Exception: `delayMicroseconds(10)` for HC-SR04 trigger pulse, and `delay()` in `setup()` before FreeRTOS starts.
- **PSRAM mandatory** — all large buffers allocated with `ps_malloc()`. Camera config uses `CAMERA_FB_IN_PSRAM`.
- **I2S**: Channel 0 (RX, mic). **Speaker is NOT I2S** — Seeed Grove Speaker (VDD/GND/SIG/NC) driven by LEDC PWM (channel 2, 10-bit @ 78.125kHz) on SIG=GPIO40; ESP32-S3 has no DAC. Separate I2C bus for MPU6050 (SDA=47, SCL=48).
- **Legacy I2S API** (`i2s_driver_install` / `i2s_set_pin`) — not the newer `i2s_new_channel` API. Mic uses legacy API; speaker uses `ledcSetup/ledcAttachPin/ledcWrite` + `esp_timer_get_time()` paced loop.

## Boot flow

1. **Factory reset check** — if trigger button held for 5s during boot → clear NVS → enter config portal.
2. **Config portal** — if no saved credentials in NVS → start AP `AegisSight-Setup`, serve web form for Wi‑Fi SSID/PASS + Gemini API Key → save to NVS → reboot.
3. **Normal init** — secrets loaded from NVS → init camera, I2S, sensors → spawn FreeRTOS tasks.

## Feature flags (`include/config.h`)

Everything is modular via `#define` / `#ifdef`. Comment a flag to exclude that feature at compile time.

### Main pipeline flags (always on for normal build)
- `ENABLE_CAMERA_OV2640`, `ENABLE_SPEAKER_I2S`
- `ENABLE_AI_PIPELINE`, `ENABLE_TTS_CLOUD`

> **Camera reality check**: sensor is **RHYX M21-45 = GC2145** (2MP), which has **no
> hardware JPEG encoder** (`support_jpeg = false` in the esp32-camera driver). Trying
> `PIXFORMAT_JPEG` fails with `0x106`. Capture must use `PIXFORMAT_RGB565` / `YUV422`
> at low resolution (QVGA/VGA). The AI pipeline currently expects JPEG frames — needs a
> software JPEG encoder (e.g. `esp_jpeg`) or a JPEG-capable sensor before `ENABLE_AI_PIPELINE`
> can work end-to-end.

### Standalone test flags (Chặng 1 — one at a time)
- `ENABLE_MIC_TEST`, `ENABLE_SPEAKER_TEST`, `ENABLE_ULTRASONIC_TEST`, `ENABLE_MPU6050_TEST`

Each has its own `setup()`/`loop()` and **will conflict** with `main.cpp`. To run a test:
1. Comment out ALL main pipeline flags
2. Uncomment exactly ONE test flag
3. Flash and monitor

## Core files

| File | Role |
|---|---|
| `include/config.h` | Feature flags + all GPIO pin definitions + sensor thresholds |
| `include/secrets.h` | NVS key definitions for Wi‑Fi SSID/PASS & Gemini API Key |
| `include/config_portal.h` | First‑time setup web portal declaration |
| `include/tone_driver.h` | Shared tone API + AI audio stream ring buffer interface |
| `include/ai_pipeline.h` | AI pipeline start/stop + task declarations |
| `src/secrets.cpp` | Preferences (NVS) load/save/clear for credentials |
| `src/config_portal.cpp` | Captive portal: AP (AegisSight-Setup) + DNS + HTTP form |
| `src/tone_driver.cpp` | LEDC PWM init, sine generator, FreeRTOS queue, AI stream ring buffer playback |
| `src/ai_pipeline.cpp` | Core 0 HTTPS (Gemini SSE text) → normalize → Google TTS HTTP; Core 1 button → JPEG capture |
| `src/ultrasonic_proximity.cpp` | HC-SR04 ISR → distance → `tone_driver_play()` |
| `src/fall_detection.cpp` | MPU6050 3-phase state machine → SOS via PWM speaker |
| `src/auto_volume.cpp` | Mic I2S RX RMS → `tone_driver_set_volume()` (releases mic when AI pipeline active) |
| `src/main.cpp` | Boot flow → `setup()` inits hardware + spawns Core 0/1 tasks |
| `CONTEXT.md` | Full product spec in Vietnamese |

## Pinout highlights (all in `config.h`)

- **Speaker** (Grove PWM): SIG=GPIO21 (LEDC ch2, 10-bit @ 78.125kHz), VDD/GND cấp nguồn cho loa. GPIO40 = SD DATA0 (pull-up khe thẻ SD) — không dùng cho loa.
- **Mic** (I2S RX): SD=GPIO2, SCK=GPIO41, WS=GPIO42
- **Camera** (DVP, FPC cố định): sensor RHYX M21-45 (GC2145) — SIOD=4 SIOC=5 VSYNC=6 HREF=7 PCLK=13 XCLK=15, Y2=11 Y3=9 Y4=8 Y5=10 Y6=12 Y7=18 Y8=17 Y9=16
- **Ultrasonic**: Trig=GPIO8, Echo=GPIO9
- **MPU6050** (I2C): SDA=GPIO47, SCL=GPIO48
- **Trigger button**: GPIO14 (INPUT_PULLUP, Active LOW) — press to record, release to send AI query

## Key conventions

- **SOS alarm** is software-synthesized through PWM speaker (`tone_driver_play()`), not a physical GPIO buzzer.
- **All audio output** shares one LEDC PWM channel via the tone driver. AI audio streaming uses a PSRAM ring buffer; when `tone_driver_stream_set_active(true)`, the tone task reads from the stream buffer first. Output is paced at 16kHz with `esp_timer_get_time()` (ESP32-S3 has no DAC).
- **AI pipeline** mic acquisition: when pipeline starts, `auto_volume` releases mic I2S and the AI audio task takes over. On stop, auto_volume re-acquires it.
- **I2S port conflict** is avoided by `i2s_driver_uninstall()` / re-install — only one task owns the mic at a time.
- **Ultrasonic beep zones** (cm): `DANGER≤50cm` (rapid 80ms), `WARNING≤100cm` (300ms), `SLOW≤150cm` (600ms), `>150cm` (silent).
- **Fall detection**: 3-phase state machine (free-fall <0.5g → impact >2.5g → inactivity ~1g for 2s) → 10s cancel window → SOS alarm.
- **Auto-volume**: Maps ambient mic RMS to volume 1-21 via `tone_driver_set_volume()`.

## Config Portal (first‑time setup)

- On first boot (empty NVS) → ESP32 creates AP `AegisSight-Setup` (no password).
- Use phone: connect to `AegisSight-Setup` → captive portal opens automatically (or browse to `192.168.4.1`).
- Fill in: **Wi‑Fi SSID**, **Wi‑Fi Password**, **Gemini API Key**.
- Submit → saved to NVS (Preferences) → device reboots into normal mode.
- To reconfigure: hold trigger button for 5s during boot → factory reset → portal again.

## AI Pipeline (Chặng 3 — HTTP REST)

### Protocol
- **API**: `POST /v1beta/models/gemini-3.5-flash-lite:streamGenerateContent?alt=sse&key=API_KEY`
- **Transport**: HTTPS (`WiFiClientSecure`, `client.setInsecure()`) for Gemini; plain HTTP (`WiFiClient`) for Google TTS
- **Request**: `systemInstruction` (disable LaTeX/MD) + base64-encoded JPEG image
- **Response**: SSE stream (`data: {...}`) with `candidates[].content.parts[].text` → accumulate → normalize → split sentences → Google TTS per sentence

### Data flow (Press‑to‑Ask)
1. **Press button** → Core 1 captures 1 JPEG frame → signals `dataReady`. **No mic recording** — faster, lower power. *(GC2145 has no JPEG — needs `esp_jpeg` RGB565→JPEG conversion, see "Camera reality check" above.)*
2. **Core 0** (net task): proactive Wi‑Fi starts as soon as `pipelineBusy` is true (overlaps with JPEG capture) → builds JSON (system instruction + base64 JPEG) → POST to Gemini `streamGenerateContent` → reads SSE text chunks.
3. After `[DONE]` or `finishReason: "STOP"` → **close SSL** (free TLS RAM) → normalize text (clean LaTeX/MD, Vietnamese pronunciation rules) → split into sentences at `. ? ! \n` (`.` ignored between digits, e.g. `2.147`).
4. For each sentence: HTTP GET `translate.google.com:80` → download MP3 → decode via Helix MP3 (`libhelix-mp3`) → resample to 16kHz → write to `tone_driver` stream ring buffer → wait for playback complete.
5. **Core 1** (tone task): reads stream → `i2s_write()` to speaker (with volume scaling).
6. Cleanup → Wi‑Fi enters modem-sleep. Press button during playback to cancel.

## Secrets & NVS persistence

| Key | NVS name | Description |
|---|---|---|
| `SK_WIFI_SSID` | `ssid` | Wi‑Fi network name |
| `SK_WIFI_PASS` | `pass` | Wi‑Fi password |
| `SK_GEMINI_KEY` | `api_key` | Gemini API key |
| `SK_LAST_SSID` | `last_ssid` | Last connected SSID (fast reconnect) |
| `SK_WIFI_SSID2/3` | `ssid2/3` | Optional secondary networks |

Credentials only come from NVS (set via portal). No compile-time defaults.
`secrets.h` only defines NVS key names — never contains real credentials.

## Wi‑Fi & power management

- **Idle**: Wi‑Fi stays associated in **modem-sleep** (~15-30mA). Radio sleeps between DTIM beacons, wake ~10ms.
- **Active**: Full power during HTTP request (~100mA). `WiFi.setSleep(false)` at start, `WiFi.setSleep(true)` when done.
- **Latency**: ~10ms wake (modem-sleep) → ~0.5-1.5s (cached AP reconnect) → up to 6s (fallback scan all networks).
- Start of `ensure_wifi()`: try last-connected SSID first (2s timeout). If fails, try all saved networks sequentially (6s each).
- **Proactive connect**: `ai_net_task` calls `ensure_wifi()` while user is still recording — overlaps reconnect with recording, saving 0.5-1.5s per cycle.

## Roadmap status

- **Chặng 1** ✅ — HW validation tests for each peripheral
- **Chặng 2** ✅ — Core 1 real-time tasks running (ultrasonic, fall, auto-vol)
- **Chặng 3** ✅ — HTTP REST + Gemini pipeline (Core 0 HTTPS + Core 1 Hold‑to‑Talk record/playback)
- **Chặng 4** ✅ — Integration & latency tuning (<1.5s)

## Chặng 4 — Integration & Latency Tuning

### Changes applied

| Area | What | Benefit |
|---|---|---|
| **Proactive Wi‑Fi** | `ai_net_task` calls `ensure_wifi()` as soon as `pipelineBusy && !dataReady` (during JPEG capture) | Wi‑Fi connects **during** JPEG capture, saving ~500-1500ms per cycle |
| **Streaming JSON body** | `serializeJson(doc, client)` replaces `serializeJson(doc, String)` — body streams directly to HTTPS | No giant String (~500KB), writes in chunks, reduces CPU + memory latency |
| **AI stream volume** | Tone task applies `currentVolume` scaling to AI PCM samples | AI voice respects auto-volume adjustment |
| **Fall detection integration** | Resets to `FALL_IDLE` when `ai_pipeline_is_busy()` | User pressing button = not falling, no false SOS |
| **JPEG capture timing** | Captured on button press (not after release) | Image matches what user saw when deciding to ask |

### Latency budget (estimated)

| Stage | Without optimization | With Chặng 4 | Notes |
|---|---|---|---|
| Wi‑Fi reconnect | ~1000ms (after record) | **~0ms** (during record) | Overlapped with recording |
| Build request | ~400ms (String alloc) | **~100ms** (stream `serializeJson`) | No 500KB copy |
| HTTP send | ~300ms | **~300ms** | Same network cost |
| Gemini processing | ~500ms+ | **~500ms+** | Server-side, not optimizable |
| **Total** | **~2200ms** | **< 1500ms** | Target met |

## Gotchas
- `links2004/WebSockets` is removed — the pipeline uses `WiFiClientSecure` for HTTPS.
- `ArduinoJson` v7 is used for building the JSON request and parsing SSE events.
- Camera test uses **RGB565/QVGA** → served as **BMP** to the browser (GC2145 has no JPEG). AI pipeline would need RGB565→JPEG conversion (`esp_jpeg`) before Gemini upload.
- Camera uses `FRAMESIZE_QVGA` (320×240, RGB565) for the web-stream test.
- Task priorities: AI_net (2), AI_audio (4), tone (3). No mic/fall/ultrasonic tasks in simplified mode.
- Google TTS uses **HTTP** (port 80) with `client=gtx` — no TLS overhead, saves RAM.
- SSL connection to Gemini is closed **before** TTS starts to free TLS memory (~50KB).
- I2S uses **legacy API** (`i2s_driver_install`), NOT `i2s_new_channel`.
- `WiFiClientSecure::setInsecure()` is used for HTTPS (no cert validation).
- Config portal runs in `setup()` before FreeRTOS tasks; `delay()` is acceptable there.
