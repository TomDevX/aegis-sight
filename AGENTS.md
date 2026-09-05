# Aegis Sight — Agent Guide

## Build & flash

```powershell
pio run -t upload          # compile + flash via USB
pio device monitor -b 2000000  # serial console at 2Mbps
```

Single environment: `esp32-s3-devkitc-1` (Arduino framework, 16MB Flash QIO, 8MB OPI PSRAM).

## Architecture

- **FreeRTOS dual-core**:
  - **Core 0**: Wi-Fi + HTTPS (Gemini SSE / Groq Vision) → Google TTS (HTTP 80 chunk 4KB) + Deepgram/Groq STT.
  - **Core 1**: Button gesture monitor + Camera capture + Mic I2S RX + HC-SR04 ultrasonic (40Hz Motion Gate) + MPU6050 fall detection + MAX98357A I2S audio tone/voice streaming.
- **No `delay()`** in FreeRTOS tasks — use `vTaskDelay()` or `millis()`-based timers. Exception: `delayMicroseconds(10)` for HC-SR04 trigger pulse, and `delay()` in `setup()` before FreeRTOS starts.
- **PSRAM mandatory** — all large buffers allocated with `ps_malloc()`. Camera config uses `CAMERA_FB_IN_PSRAM`.
- **I2S**: 
  - `I2S_NUM_0` (RX, mic INMP441): SD=GPIO2, SCK=GPIO41, WS=GPIO42.
  - `I2S_NUM_1` (TX, speaker MAX98357A): DIN=GPIO19, BCLK=GPIO20, LRC=GPIO21.
  - Hardware I2C bus for MPU6050 (SDA=GPIO47, SCL=GPIO39).
- **Legacy I2S API** (`i2s_driver_install` / `i2s_set_pin`) — not the newer `i2s_new_channel` API.

## Boot flow

1. **Factory reset check** — if trigger button held for 5s during boot → clear NVS → enter config portal.
2. **Config portal** — if no saved credentials in NVS → start AP `AegisSight-Setup`, serve web form for Wi‑Fi SSID/PASS + Gemini API Key → save to NVS → reboot.
3. **Normal init** — secrets loaded from NVS → init camera, I2S, sensors → pre-allocate PSRAM buffers → spawn FreeRTOS tasks.

## Pinout (all in `include/config.h`)

- **Camera**: GC2145 DVP bus cố định — SIOD=4, SIOC=5, VSYNC=6, HREF=7, PCLK=13, XCLK=15, Y2=11, Y3=9, Y4=8, Y5=10, Y6=12, Y7=18, Y8=17, Y9=16.
- **Speaker (I2S TX)**: MAX98357A — DIN=GPIO19, BCLK=GPIO20, LRC=GPIO21.
- **Mic (I2S RX)**: INMP441 — SD=GPIO2, SCK=GPIO41, WS=GPIO42, L/R=GND.
- **Ultrasonic**: HC-SR04 — Trig=GPIO46, Echo=GPIO3.
- **MPU6050**: I2C — SDA=GPIO47, SCL=GPIO39.
- **Trigger button**: GPIO14 (INPUT_PULLUP, Active LOW).

## Button Gestures (Nút Nhấn Thông Minh)

1. **Bấm Giữ ($\ge 250\text{ms}$)**: Hỏi câu hỏi mới (Tiếng "Tít" đơn, reset trí nhớ cũ).
2. **Bấm Đúp (Nhấp 1 cái $\rightarrow$ Bấm giữ cái thứ 2)**: Nối tiếp hội thoại cũ (Tiếng "Tít-Tít" đôi, giữ nguyên ảnh cũ và lịch sử hỏi đáp).
3. **Nhấp Nhanh 1 Cái (<250ms)**: Chụp ảnh mô tả ngay (không thu âm giọng nói).

## AI Pipeline (Đa Tầng Hoán Đổi Thông Minh)

1. **STT (Chuyển giọng nói thành văn bản)**:
   - **Tầng 1 (Chính)**: `whisper-large-v3` trên Groq LPU (~0.3s) nhận diện tiếng Việt đầy đủ thanh điệu.
   - **Tầng 2 (Dự phòng)**: `nova-3` trên Deepgram.
2. **LLM (Trí tuệ nhân tạo thị giác & đàm thoại)**:
   - **Tầng 1 (Chính)**: `gemini-3.5-flash-lite` qua Google Interactions API với SSE Streaming (`stream: true`) và Server-side Session ID (`previous_interaction_id`).
   - **Tầng 2 (Dự phòng)**: `qwen/qwen3.8-27b` / `qwen/qwen3.6-27b` trên Groq Vision với Multi-turn lưu trong 8MB PSRAM (kèm hàm `escape_json_str`).
3. **TTS (Đọc loa Google)**:
   - Google Translate TTS (HTTP 80) với bộ nhớ đệm DNS IP Cache (0ms trễ DNS) + Tải chunk MP3 4KB + Helix MP3 Decoder.
4. **Nhạc Chờ**:
   - Nhạc chờ Offline Elevator Music giải mã sẵn trong PSRAM (Zero-CPU) phát trong lúc chờ AI phản hồi.

## Motion Gate & An Toàn

- **Cảm biến siêu âm (HC-SR04)**: Luôn hoạt động độc lập (không phụ thuộc MPU). Tầm quét mở rộng đến $85\text{cm}$ để báo sớm từ xa với 3 vùng cảnh báo (giữ nguyên tần suất bíp từng cấp độ): Safe ($50-85\text{cm}$, thong thả $550\text{ms}$), Warn ($25-50\text{cm}$, vừa $320\text{ms}$), Danger ($\le 25\text{cm}$, dồn dập $180\text{ms}$). Nếu người dùng đứng lại trước vật cản (khoảng cách chỉ dao động trong phạm vi $\le 5\text{cm}$ quá $1.2\text{s}$), kính sẽ tự động dừng tiếng bíp để tránh phiền tai. Khi người dùng di chuyển lại gần hơn ($\ge 3.5\text{cm}$) hoặc bước vào vùng nguy hiểm hơn, kính sẽ tiếp tục phát chuông cảnh báo ngay lập tức.
- **Fall Detection**: State machine 3 pha (Free-fall <0.5g → Impact >2.5g → Inactivity 1g for 2s) → Cửa sổ hủy 10s → SOS alarm. Lấy mẫu MPU6050 duy trì ổn định $40\text{Hz}$ (delay 25ms cố định).
- **Auto Volume**: Đo RMS độ ồn môi trường để tự động chỉnh mức âm lượng loa 1–21.

