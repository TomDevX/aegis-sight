# Aegis Sight

AI assistive headband for visually impaired. Built on **ESP32-S3** with camera, mic, speaker, ultrasonic, and IMU — talks to **Gemini** via HTTP REST for voice Q&A.

---

## Features

- **🤖 AI Hold-to-Talk** — press button, ask, release → Gemini streams audio answer to speaker
- **📡 Obstacle Beep** — HC-SR04 beeps faster as obstacles get closer (<50cm = rapid)
- **🚨 Fall Detection** — MPU6050 3-phase (free-fall → impact → inactivity) → SOS alarm
- **🔊 Auto Volume** — mic measures ambient noise, adjusts speaker volume 1-21
- **⚙️ Config Portal** — first boot: AP `AegisSight-Setup` → web form for Wi-Fi + API key

---

## Hardware

| Module | Peripheral | Pins |
|---|---|---|
| Right | Camera OV2640 | DVP bus (fixed on S3-CAM board) |
| Right | Speaker (I2S TX) | BCLK=1, LRCK=3, DIN=40 |
| Right | Ultrasonic HC-SR04 | Trig=8, Echo=9 |
| Left | Mic INMP441 (I2S RX) | BCLK=41, WS=42, SD=2 |
| Left | MPU6050 (I2C) | SDA=47, SCL=48 |
| Left | Trigger Button | GPIO14 (INPUT_PULLUP, Active LOW) |

Power: Li-ion in pocket → buck converter in left module → 5V across headband to right module.

---

## How It Works — Dual-Core FreeRTOS

| Core | Responsibility |
|---|---|
| **Core 0** | Wi-Fi, HTTP POST to Gemini, parse SSE stream |
| **Core 1** | All hardware (ultrasonic, MPU6050, mic, speaker, AI record/playback) |

All audio output shares one I2S TX channel via the tone driver. No `delay()` in tasks (except `delayMicroseconds(10)` for HC-SR04 trigger).

```
Button press (GPIO14)
  │
  ▼
Core 1: Capture JPEG ──→ Start mic recording (up to 8s)
  │
  └── Button release / timeout ──→ dataReady = true
                                        │
                                        ▼
                              Core 0: ensure_wifi() (proactive, started during recording)
                                        │
                                        ▼
                              Build JSON (stream serializeJson) ──→ POST Gemini HTTPS
                                        │
                                        ▼
                              SSE stream ──→ base64 PCM ──→ ring buffer ──→ Core 1 I2S speaker
                                        │
                                        ▼
                              [DONE] → modem-sleep
```

---

## Key Workflows

### AI Hold-to-Talk
1. **Press** → JPEG captured immediately, mic starts recording into PSRAM buffer
2. **Release / 8s timeout** → mic stops, `dataReady` flagged
3. **Core 0** (already connected proactively) → builds JSON (JPEG + WAV), streams `serializeJson` to HTTPS
4. SSE response → base64 decode → ring buffer → tone task plays with volume scaling
5. `finishReason: "STOP"` → cleanup, Wi-Fi enters modem-sleep

### Obstacle Beep
| Distance | Beep interval |
|---|---|
| > 150cm | Silence |
| 100–150cm | 600ms |
| 50–100cm | 300ms |
| < 50cm | 80ms (rapid) |

### Fall Detection (3-Phase)
1. **Free-fall**: SV < 0.5g
2. **Impact**: SV > 2.5g within 300ms
3. **Inactivity**: SV ≈ 1g for 2s

→ Beep 10s cancel window (press button to cancel) → SOS alarm via speaker

Resets to IDLE when AI pipeline is busy (user pressing button = not falling).

### Auto Volume
- Reads mic RMS every 500ms (releases mic to AI pipeline when busy)
- Maps RMS → volume 1-21 via `tone_driver_set_volume()`

### Config Portal (First-Time Setup)
1. On boot, if NVS has no SSID + API key → AP `AegisSight-Setup`
2. Connect phone → captive portal opens (or `192.168.4.1`)
3. Enter Wi-Fi SSID/PASS + Gemini API Key → save → reboot
4. Factory reset: hold trigger button 5s on boot → clears NVS → portal again

---

## Project Structure

```
include/
├── config.h              Feature flags, pin definitions, thresholds
├── secrets.h             NVS key names (no real credentials)
├── config_portal.h       Portal AP declarations
├── tone_driver.h         I2S tone + AI stream ring buffer API
└── ai_pipeline.h         Pipeline start/stop/busy API

src/
├── main.cpp              Boot flow: factory reset → portal → init → spawn
├── ai_pipeline.cpp       Core 0 HTTP (Wi-Fi + REST + SSE) + Core 1 Hold-to-Talk
├── tone_driver.cpp       I2S TX, sine generator, queue, stream playback + volume
├── secrets.cpp           Preferences (NVS) load/save/clear
├── config_portal.cpp     Captive portal (AP + DNS + web form → NVS → restart)
├── ultrasonic_proximity.cpp  HC-SR04 ISR → distance → beep
├── fall_detection.cpp        MPU6050 3-phase state machine → SOS
└── auto_volume.cpp           Mic RMS → volume mapping
```

---

## Feature Flags

Define/undefine in `include/config.h` to include/exclude features at compile time.

| Flag | Default | Description |
|---|---|---|
| `ENABLE_CAMERA_OV2640` | on | Camera for AI pipeline |
| `ENABLE_MICROPHONE_INMP441` | on | Mic for recording + auto-volume |
| `ENABLE_SPEAKER_I2S` | on | I2S speaker output |
| `ENABLE_ULTRASONIC_HC_SR04` | on | Obstacle proximity beep |
| `ENABLE_MPU6050_FALL_DETECTION` | on | Fall detection + SOS |
| `ENABLE_AI_PIPELINE` | on | Hold-to-Talk + Gemini HTTP |
| `ENABLE_AUTO_VOLUME` | on | Ambient noise → volume adjust |

Test flags (one at a time, comment all above): `ENABLE_MIC_TEST`, `ENABLE_SPEAKER_TEST`, `ENABLE_ULTRASONIC_TEST`, `ENABLE_MPU6050_TEST`.

---

## Build & Flash

```powershell
pio run -t upload                  # compile + flash via USB
pio device monitor -b 115200       # serial console
```

Board: `esp32-s3-devkitc-1` (Arduino framework, 16MB Flash QIO, 8MB OPI PSRAM).

---

## Key Design Decisions

- **PSRAM mandatory** — all large buffers (`recAudio` 256KB, `jpegBuf` 64KB, `decBuf` 64KB) use `ps_malloc()`
- **No `String` allocations** in hot paths — SSE lines parsed into stack char arrays, JSON body streamed with `serializeJson(doc, client)`
- **Wi-Fi modem-sleep** between AI calls (~15–30mA idle); full power only during HTTP (~100mA)
- **Credential caching** — NVS read once into globals, avoiding 7+ reads per cycle
- **Proactive Wi-Fi** — `ensure_wifi()` called while user is still recording, overlapping reconnect with record time
- **Legacy I2S API** (`i2s_driver_install`) — not `i2s_new_channel`
- **I2S port conflict avoided** by `i2s_driver_uninstall()` / re-install between auto_volume and AI pipeline
- **Zero `delay()`** in FreeRTOS tasks (exception: `delayMicroseconds(10)` for HC-SR04, `delay()` OK in `setup()`)

---

## Roadmap

- **Phase 1** ✅ — HW validation tests for each peripheral
- **Phase 2** ✅ — Core 1 real-time tasks (ultrasonic, fall, auto-volume)
- **Phase 3** ✅ — HTTP REST + Gemini pipeline (Core 0 HTTPS + Core 1 Hold-to-Talk)
- **Phase 4** ✅ — Integration & latency tuning (< 1.5s)
