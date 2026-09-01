# Aegis Sight

**Kính Thông Minh Trợ Thị Tích Hợp Đa Trí Tuệ Nhân Tạo Dành Cho Người Khiếm Thị**

Aegis Sight là thiết bị đeo hỗ trợ người khiếm thị xây dựng trên nền tảng vi điều khiển **ESP32-S3 (16MB Flash, 8MB PSRAM)** tích hợp Camera, Micro I2S, Loa Khuếch đại MAX98357A, Cảm biến Siêu âm HC-SR04 và IMU MPU6050 — tương tác giọng nói thời gian thực với độ trễ cực thấp (<1.5s) thông qua kiến trúc **Đa Tầng AI Hoán Đổi Thông Minh (Google Gemini + Groq LPU)**.

---

## 🌟 Tính Năng Nổi Bật

- **🤖 AI Đa Tầng Thời Gian Thực (<1.5s)**:
  - **Tầng 1 (Chính)**: **Google Gemini Interactions API (`gemini-3.5-flash-lite`)** với SSE Streaming trực tiếp và lưu trữ Session ID ngữ cảnh Server-side.
  - **Tầng 2 (Dự phòng)**: **Groq Vision LLM (`qwen/qwen3.8-27b` / `qwen/qwen3.6-27b`)** với bộ nhớ Multi-turn lưu trữ trong 8MB PSRAM.
  - **STT Giọng Nói**: **Groq Whisper Large v3** (~0.3s) + Dự phòng **Deepgram Nova-3**.
  - **TTS Phát Loa**: Google Translate TTS với DNS Caching 0ms + Streaming MP3 Chunk 4KB.
  - **Nhạc Chờ**: Elevator Music giải mã sẵn trong PSRAM (Zero-CPU) phát trong lúc chờ AI phản hồi.
- **🎮 Cử Chỉ Nút Bấm Đa Năng**:
  - **Bấm Giữ ($\ge 250\text{ms}$)**: Hỏi câu hỏi mới (Tiếng "Tít" đơn, reset ngữ cảnh).
  - **Bấm Đúp (Nhấp 1 cái $\rightarrow$ Bấm giữ cái thứ 2)**: Nối tiếp hội thoại cũ (Tiếng "Tít-Tít" đôi, soi lại ảnh cũ và nhớ câu trả lời trước).
  - **Nhấp Nhanh 1 Cái (<250ms)**: Chụp ảnh mô tả nhanh quang cảnh/chữ viết phía trước (không cần nói).
- **🚶 Motion Gate (Cảm Biến Siêu Âm Thông Minh)**:
  - Cảm biến khoảng cách HC-SR04 **chỉ phát tiếng bíp khi người dùng thực sự bước đi** (nhận diện xung lực gót chân đập xuống đất: $\text{StdDev} \ge 0.12\text{G}$ và $\text{P2P} \ge 0.25\text{G}$).
  - Đứng yên, ngồi yên hoặc **xoay đầu nhìn quanh tại chỗ $\rightarrow$ Mute $100\%$**, trả lại không gian yên tĩnh.
- **🚨 Phát Hiện Té Ngã (Fall Detection 3 Pha)**:
  - MPU6050 nhận diện: Rơi tự do ($<0.5\text{g}$) $\rightarrow$ Va đập ($>2.5\text{g}$) $\rightarrow$ Bất động ($\approx 1\text{g}$).
  - Cửa sổ hủy 10s (bấm nút để hủy) $\rightarrow$ Phát còi báo động SOS cứu hộ ra loa.
- **🔊 Tự Động Điều Chỉnh Âm Lượng (Auto-Volume)**:
  - Micro đo độ ồn môi trường và tự động map mức âm lượng loa từ 1–21.
- **⚙️ Config Portal (Cấu Hình Lần Đầu)**:
  - Tự phát Wi-Fi AP `AegisSight-Setup` $\rightarrow$ Mở Captive Portal nhập SSID/Pass + API Key lưu NVS.

---

## 🔌 Sơ Đồ Chân Phần Cứng (Hardware Pinout)

| Module / Ngoại vi | Chân Phần Cứng | Chân ESP32-S3 GPIO | Ghi chú |
|---|---|---|---|
| **Camera GC2145 (DVP)** | SIOD / SIOC / PCLK / XCLK / VSYNC / HREF / Y2..Y9 | `4, 5, 13, 15, 6, 7, 11, 9, 8, 10, 12, 18, 17, 16` | FPC cố định trên bo mạch |
| **Loa MAX98357A (I2S TX)** | DIN / BCLK / LRC | `DIN=19, BCLK=20, LRC=21` | I2S Channel 1, cấp nguồn 5V từ Buck |
| **Micro INMP441 (I2S RX)** | SD / SCK / WS / L/R | `SD=2, SCK=41, WS=42, L/R=GND` | I2S Channel 0, Left Channel 16kHz |
| **Siêu Âm HC-SR04** | Trig / Echo | `Trig=46, Echo=3` | Nguồn 5V từ Buck, chân tự do an toàn |
| **MPU6050 (I2C)** | SDA / SCL | `SDA=47, SCL=39` | Bus I2C phần cứng |
| **Nút Nhấn Trigger** | Data / GND | `GPIO14 (INPUT_PULLUP)` | Active LOW |

---

## 🏗️ System Flowcharts (Kiến Trúc & Lưu Đồ Giải Thuật)

### 1. System Architecture Flowchart

```mermaid
---
config:
  layout: dagre
---
flowchart TB
 subgraph C1["Core 1: Hardware & Real-Time Sensors (40Hz)"]
        A1["Button gestures: Hold / Double-Click / Click"]
        A2["INMP441 Mic: Audio recording"]
        A3["GC2145 Camera: Capture RGB565 320x240"]
        A4["MPU6050 Motion Gate: Footstep gait filtering"]
        A5["HC-SR04 Ultrasonic: Obstacle distance measurement"]
        A6["MPU6050 Fall Detection: Fall detection"]
        A7["MAX98357A Speaker: Play chimes, waiting music & AI voice"]
  end
 subgraph C0["Core 0: Network Protocols & AI Pipeline"]
        B1["Proactive Wi-Fi connection while speaking"]
        B2["STT Processing: Send audio to 3rd-party STT API (~0.3s)"]
        B3["Multi-tier AI Orchestrator: Primary and fallback tiers"]
        B4["TTS Processing: Stream audio from 3rd-party TTS API"]
        B5["Audio Decoder"]
  end
 subgraph S_EDGE["HEAD-WORN DEVICE - ESP32-S3 DUAL-CORE"]
    direction TB
        C1
        C0
  end
 subgraph S_CLOUD["THIRD-PARTY CLOUD AI INFRASTRUCTURE"]
        C_STT["3rd-Party STT API (Primary / Fallback)"]
        C_LLM1["3rd-Party Multimodal LLM (Primary tier)"]
        C_LLM2["3rd-Party Vision LLM (Tier 2 - Multi-turn Fallback)"]
        C_TTS["3rd-Party TTS Voice API"]
  end
    A1 --> A2 & A3
    A2 --> B2
    A3 --> B3
    B1 --> B2
    B2 --> C_STT
    C_STT -- Transcribed question text --> B3
    B3 -- Tier 1: Session ID + Prompt + Image --> C_LLM1
    C_LLM1 -. Failover on disconnection/overload .-> C_LLM2
    C_LLM1 -- Real-time answer streaming --> B4
    C_LLM2 -- Real-time Token Stream --> B4
    B4 --> C_TTS
    C_TTS --> B5
    B5 --> A7
    A4 -- Enable proximity beeps --> A5
    A6 -- Trigger SOS alarm --> A7
```

---

### 2. Multimodal AI Pipeline Flowchart

```mermaid
flowchart TB
    Start(["Start: User interacts with button"]) --> CheckGesture{"Analyze button gesture"}
    CheckGesture -- "Hold >= 250ms" --> G1["Single Hold: Record new question\nSingle Beep chime\nReset previous context"]
    CheckGesture -- "Double-click: Click 1 -> Hold 2" --> G2["Double-Click: Record follow-up question\nDouble Beep chime\nKeep previous image & context"]
    CheckGesture -- "Single click (<250ms)" --> G3["Single Click: Describe photo immediately\nNo audio recording"]
    G1 --> NetInit["Core 0: Connect Wi-Fi & Capture JPEG"]
    G2 --> NetInit
    G3 --> NetInit
    NetInit --> WaitTone["Play offline waiting music in PSRAM"]
    WaitTone --> STT{"Is voice audio present?"}
    STT -- Yes --> STT_P["Send WAV to 3rd-party primary STT API"]
    STT -- No --> PromptGen["Generate Prompt: Request describing scene ahead"]
    STT_P --> STTOk{"STT successful?"}
    STTOk -- Success --> PromptGen
    STTOk -- Network Error --> STT_F["Switch to 3rd-party fallback STT API"]
    STT_F --> PromptGen
    PromptGen --> LLM_Tier1["TIER 1: 3rd-Party AI LLM"]
    LLM_Tier1 --> CheckLLM1{"Tier 1 responded?"}
    CheckLLM1 -- "Success: SSE Stream" --> StreamTTS["Receive words -> Split sentences -> Download & Play via 3rd-party TTS API"]
    CheckLLM1 -- "Network Error / Context Expired / Overloaded" --> LLM_Tier2["TIER 2: 3rd-Party AI LLM (Multi-turn PSRAM Memory)"]
    LLM_Tier2 --> CheckLLM2{"Tier 2 responded?"}
    CheckLLM2 -- Success --> StreamTTS
    CheckLLM2 -- Rate Limited --> SwapModel["Automatically switch to fallback model and retry"]
    SwapModel --> StreamTTS
    CheckLLM2 -- Both Rounds Failed --> ErrorBeep["Play connection error warning beep"]
    StreamTTS --> Done(["End: Play completion chime"])
```

---

### 3. Sensor & Safety Flowchart (Motion Gate & Fall Detection)

```mermaid
flowchart TB
    Start(["Start MPU6050 sampling cycle (40Hz / 25ms)"]) --> ReadSensors[/"Read MPU6050 accelerometer (ax, ay, az)"/]
    ReadSensors --> CalcSV["Calculate composite acceleration: SV = sqrt(ax² + ay² + az²)"]
    CalcSV --> Fall1{"Phase 1: Free-fall?<br>(SV &lt; 0.5G for 150-300ms)"}
    Fall1 -- YES --> Fall2{"Phase 2: Ground impact?<br>(SV &gt; 2.5G within next 300ms)"}
    Fall2 -- YES --> Fall3{"Phase 3: Rest inactivity?<br>(SV ≈ 1.0G sustained for 2s)"}
    Fall3 -- "YES: Confirmed Fall" --> FallWindow["Open 10s cancellation countdown window<br>Play reminder beeps"]
    FallWindow --> FallCancelBtn{"Did user press Trigger button?"}
    FallCancelBtn -- "YES: False alarm" --> FallReset["Cancel fall alert & Return to normal state"]
    FallCancelBtn -- "NO (10s expired): Unconscious" --> FallAlarm[/"PLAY SOS RESCUE ALARM VIA SPEAKER"/]
    Fall1 -- NO --> CalcMotion["Calculate StdDev & P2P in 600ms window"]
    Fall2 -- NO --> CalcMotion
    Fall3 -- "NO (Regained movement)" --> CalcMotion
    FallReset --> EndCycle(["Wait for next 25ms cycle"])
    FallAlarm --> EndCycle
    CalcMotion --> MotionCheck{"StdDev &gt;= 0.12G<br>AND P2P &gt;= 0.25G<br>sustained &gt;= 600ms?"}
    MotionCheck -- "NO: Still / Rotating head" --> GateOff["Disable ultrasonic beeps (Keep silent)"]
    GateOff --> EndCycle
    MotionCheck -- "YES: Walking" --> GateOn[/"Activate HC-SR04 distance measurement"/]
    GateOn --> DistCheck{"Obstacle distance (D)"}
    DistCheck -- "D &lt;= 18cm" --> Beep1[/"Rapid beep 180ms: Danger Zone"/]
    DistCheck -- "18cm &lt; D &lt;= 32cm" --> Beep2[/"Medium beep 320ms: Warning Zone"/]
    DistCheck -- "32cm &lt; D &lt;= 45cm" --> Beep3[/"Slow beep 550ms: Safe Zone"/]
    DistCheck -- D &gt; 45cm --> Beep4["No beeping"]
    Beep1 --> EndCycle
    Beep2 --> EndCycle
    Beep3 --> EndCycle
    Beep4 --> EndCycle

    %% LEGEND BOX
    subgraph LEGEND ["📝 LEGEND"]
        direction TB
        N_SV["• SV (Signal Vector Magnitude): Composite 3-axis acceleration vector magnitude (Orientation-invariant)<br>• G: Acceleration unit"]
        N_MOTION["• StdDev (Standard Deviation): Standard deviation measuring footstep oscillation dispersion<br>• P2P (Peak-to-Peak): Maximum amplitude difference between peak and trough acceleration"]
    end

    %% Dotted annotations
    CalcSV -.- N_SV
    CalcMotion -.- N_MOTION

    classDef legendStyle fill:#0f172a,stroke:#38bdf8,stroke-width:1px,stroke-dasharray: 4 4,color:#e2e8f0,text-align:left;
    class LEGEND,N_SV,N_MOTION legendStyle;
```

---

## ⚡ Hướng Dẫn Biên Dịch & Nạp Firmware

```bash
# Biên dịch và nạp firmware qua cổng USB
pio run -t upload

# Mở Serial Monitor với tốc độ 2.000.000 baud (2Mbps)
pio device monitor -b 2000000
```

---

## 📁 Cấu Trúc Thư Mục

```
include/
├── config.h              Định nghĩa chân GPIO, ngưỡng cảm biến & cờ tính năng
├── secrets.h             Định nghĩa các NVS Key cho Wi-Fi và API Key
├── tone_driver.h         Driver phát loa I2S, nhạc chờ và Ring Buffer AI
├── tts_driver.h          Driver tải & giải mã MP3 Google TTS
├── motion_gate.h         Bộ lọc nhận diện bước chân MPU6050
└── ai_pipeline.h         Khai báo pipeline đa tầng AI & FreeRTOS tasks

src/
├── main.cpp              Khởi động hệ thống, kiểm tra NVS & spawn FreeRTOS tasks
├── ai_pipeline.cpp       Toàn bộ pipeline AI: Thu âm, Gemini SSE, Groq Vision, Whisper STT
├── motion_gate.cpp       Thuật toán lọc xung lực bước chân (StdDev + Peak-to-Peak)
├── tone_driver.cpp       Phát nhạc chờ PSRAM, tiếng chuông & I2S Stream Loa
├── tts_driver.cpp        HTTP Google TTS với DNS Cache & Helix Decoder
├── ultrasonic_proximity.cpp Cảm biến HC-SR04 tích hợp Motion Gate (40Hz)
├── fall_detection.cpp    State machine 3 pha phát hiện ngã
├── mpu_manager.cpp       Quản lý bus I2C & lấy mẫu MPU6050
├── secrets.cpp           NVS Preferences lưu trữ credentials
└── config_portal.cpp     Web captive portal cấu hình Wi-Fi lần đầu
```
