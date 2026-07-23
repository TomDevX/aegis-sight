# Aegis Sight — Agent Guide

## Build & flash

```powershell
pio run -t upload          # compile + flash via USB
pio device monitor -b 115200  # serial console
```

Single environment: `esp32-s3-devkitc-1` (Arduino framework, 16MB Flash QIO, 8MB OPI PSRAM).

## Architecture

- **FreeRTOS dual-core**: Core 0 = Wi-Fi/WebSocket (future); Core 1 = all hardware (ultrasonic, MPU6050, mic, speaker).
- **No `delay()`** — use `vTaskDelay()` or `millis()`-based timers. Exception: `delayMicroseconds(10)` for HC-SR04 trigger pulse.
- **PSRAM mandatory** — all large buffers allocated with `ps_malloc()`. Camera config uses `CAMERA_FB_IN_PSRAM`.
- **I2S**: Channel 0 (RX, mic), Channel 1 (TX, speaker). Separate I2C bus for MPU6050 (SDA=47, SCL=48).

## Feature flags (`include/config.h`)

Everything is modular via `#define` / `#ifdef`. Comment a flag to exclude that feature at compile time.

### Main pipeline flags (always on for normal build)
- `ENABLE_CAMERA_OV2640`, `ENABLE_MICROPHONE_INMP441`, `ENABLE_SPEAKER_I2S`
- `ENABLE_ULTRASONIC_HC_SR04`, `ENABLE_MPU6050_FALL_DETECTION`
- `ENABLE_AUTO_VOLUME`

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
| `include/tone_driver.h` | Shared tone API — `play()`, `stop()`, `set_volume()` |
| `src/tone_driver.cpp` | I2S TX init, sine generator, FreeRTOS queue |
| `src/ultrasonic_proximity.cpp` | HC-SR04 ISR → distance → `tone_driver_play()` |
| `src/fall_detection.cpp` | MPU6050 3-phase state machine → SOS via I2S speaker |
| `src/auto_volume.cpp` | Mic I2S RX RMS → `tone_driver_set_volume()` |
| `src/main.cpp` | `setup()` inits hardware + spawns Core 1 tasks |
| `CONTEXT.md` | Full product spec in Vietnamese |

## Pinout highlights (all in `config.h`)

- **Speaker** (I2S TX): BCLK=GPIO1, LRCK=GPIO3, DIN=GPIO40
- **Mic** (I2S RX): SD=GPIO2, SCK=GPIO41, WS=GPIO42
- **Camera** (DVP): pins fixed on ESP32-S3-CAM FPC connector
- **Ultrasonic**: Trig=GPIO8, Echo=GPIO9
- **MPU6050** (I2C): SDA=GPIO47, SCL=GPIO48
- **Trigger button**: GPIO14 (INPUT_PULLUP, Active LOW)

## Key conventions

- **SOS alarm** is software-synthesized through the I2S speaker (`tone_driver_play()`), not a physical GPIO buzzer. No separate buzzer pin exists.
- **All audio output** goes through the tone driver queue — proximity beep, fall alert SOS, and future AI speech all share one I2S TX channel.
- **Ultrasonic beep zones** (cm): `DANGER≤50cm` (rapid 80ms), `WARNING≤100cm` (300ms), `SLOW≤150cm` (600ms), `>150cm` (silent).
- **Fall detection**: 3-phase state machine (free-fall <0.5g → impact >2.5g → inactivity ~1g for 2s) → 10s cancel window via button → SOS alarm.
- **Auto-volume**: Maps ambient mic RMS (16-bit PCM) to volume 1-21 via `tone_driver_set_volume()`.

## Roadmap status

- **Chặng 1** ✅ — HW validation tests for each peripheral
- **Chặng 2** ✅ — Core 1 real-time tasks running (ultrasonic, fall, auto-vol)
- **Chặng 3** ❌ — WebSocket + Gemini Live stream pipeline
- **Chặng 4** ❌ — Integration & latency tuning (<1.2s)

## Gotchas

- `ESP32-audioI2S` library is declared in `platformio.ini` but **not yet used** — future Chặng 3 will use it for AI audio streaming.
- `links2004/WebSockets` and `ArduinoJson` are declared but **not yet used**.
- Camera uses `FRAMESIZE_SVGA` (800×600, JPEG Q10) for the AI pipeline.
- Task priorities: fall_det (4), tone (3), ultrasonic (2), auto_vol (1).
- The trigger button initialization depends on `ENABLE_CAMERA_OV2640` flag.
