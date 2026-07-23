# Aegis Sight — Agent Guide

## Build & flash

```powershell
pio run -t upload          # compile + flash via USB
pio device monitor -b 115200  # serial console
```

Single environment: `esp32-s3-devkitc-1` (Arduino framework, 16MB Flash QIO, 8MB OPI PSRAM).

## Architecture

- **FreeRTOS dual-core**: Core 0 = Wi-Fi + HTTP REST (Gemini `generateContent`); Core 1 = all hardware (ultrasonic, MPU6050, mic, speaker, AI audio).
- **No `delay()`** — use `vTaskDelay()` or `millis()`-based timers. Exception: `delayMicroseconds(10)` for HC-SR04 trigger pulse, and `delay()` in `setup()` before FreeRTOS starts.
- **PSRAM mandatory** — all large buffers allocated with `ps_malloc()`. Camera config uses `CAMERA_FB_IN_PSRAM`.
- **I2S**: Channel 0 (RX, mic), Channel 1 (TX, speaker). Separate I2C bus for MPU6050 (SDA=47, SCL=48).
- **Legacy I2S API** (`i2s_driver_install` / `i2s_set_pin`) — not the newer `i2s_new_channel` API.

## Boot flow

1. **Factory reset check** — if trigger button held for 5s during boot → clear NVS → enter config portal.
2. **Config portal** — if no saved credentials in NVS → start AP `AegisSight-Setup`, serve web form for Wi‑Fi SSID/PASS + Gemini API Key → save to NVS → reboot.
3. **Normal init** — secrets loaded from NVS → init camera, I2S, sensors → spawn FreeRTOS tasks.

## Feature flags (`include/config.h`)

Everything is modular via `#define` / `#ifdef`. Comment a flag to exclude that feature at compile time.

### Main pipeline flags (always on for normal build)
- `ENABLE_CAMERA_OV2640`, `ENABLE_MICROPHONE_INMP441`, `ENABLE_SPEAKER_I2S`
- `ENABLE_ULTRASONIC_HC_SR04`, `ENABLE_MPU6050_FALL_DETECTION`
- `ENABLE_AUTO_VOLUME`, `ENABLE_AI_PIPELINE`

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
| `src/tone_driver.cpp` | I2S TX init, sine generator, FreeRTOS queue, AI stream ring buffer playback |
| `src/ai_pipeline.cpp` | Core 0 HTTP task (Wi‑Fi + REST POST + SSE parse) + Core 1 Hold‑to‑Talk mic + JPEG capture |
| `src/ultrasonic_proximity.cpp` | HC-SR04 ISR → distance → `tone_driver_play()` |
| `src/fall_detection.cpp` | MPU6050 3-phase state machine → SOS via I2S speaker |
| `src/auto_volume.cpp` | Mic I2S RX RMS → `tone_driver_set_volume()` (releases mic when AI pipeline active) |
| `src/main.cpp` | Boot flow → `setup()` inits hardware + spawns Core 0/1 tasks |
| `CONTEXT.md` | Full product spec in Vietnamese |

## Pinout highlights (all in `config.h`)

- **Speaker** (I2S TX): BCLK=GPIO1, LRCK=GPIO3, DIN=GPIO40
- **Mic** (I2S RX): SD=GPIO2, SCK=GPIO41, WS=GPIO42
- **Camera** (DVP): pins fixed on ESP32-S3-CAM FPC connector
- **Ultrasonic**: Trig=GPIO8, Echo=GPIO9
- **MPU6050** (I2C): SDA=GPIO47, SCL=GPIO48
- **Trigger button**: GPIO14 (INPUT_PULLUP, Active LOW) — press to record, release to send AI query

## Key conventions

- **SOS alarm** is software-synthesized through I2S speaker (`tone_driver_play()`), not a physical GPIO buzzer.
- **All audio output** shares one I2S TX channel via the tone driver. AI audio streaming uses a PSRAM ring buffer; when `tone_driver_stream_set_active(true)`, the tone task reads from the stream buffer first.
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
- **API**: `POST /v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse&key=API_KEY`
- **Transport**: HTTPS (`WiFiClientSecure`, `client.setInsecure()`)
- **Request**: JSON body with `parts` containing base64-encoded JPEG + WAV audio
- **Response**: SSE stream (`data: {...}`) with `candidates[].content.parts[].inlineData.data` = base64 PCM

### Data flow (Hold‑to‑Talk)
1. **Press & hold button** → Core 1 captures 1 JPEG frame, starts recording mic INMP441 into PSRAM buffer.
2. **Release button** / **8s timeout** → Core 1 stops mic, signals `dataReady`.
3. **Core 0** (net task): if Wi‑Fi was in modem-sleep → instant wake (~10ms); else already connected (proactive Wi‑Fi started during recording) → builds JSON payload (base64 JPEG + WAV) → POST to Gemini → reads SSE stream.
4. For each SSE `inlineData` chunk: decodes base64 PCM → writes to `tone_driver` stream ring buffer.
5. **Core 1** (tone task): reads stream → `i2s_write()` to speaker (with volume scaling).
6. On `finishReason: "STOP"` or `[DONE]` → cleanup → Wi‑Fi enters modem-sleep (associated, low power). Press button again to cancel playback.

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
| **Proactive Wi‑Fi** | `ai_net_task` calls `ensure_wifi()` as soon as `pipelineBusy && !dataReady` (user still recording) | Wi‑Fi connects **during** recording, saving ~500-1500ms per cycle |
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
- Camera uses `FRAMESIZE_SVGA` (800×600, JPEG Q10) for the AI pipeline.
- Task priorities: AI_audio (5), fall_det (4), tone (3), AI_net (2), ultrasonic (2), auto_vol (1).
- I2S uses **legacy API** (`i2s_driver_install`), NOT `i2s_new_channel`.
- `WiFiClientSecure::setInsecure()` is used for HTTPS (no cert validation).
- Config portal runs in `setup()` before FreeRTOS tasks; `delay()` is acceptable there.
