# BÁO CÁO LƯU ĐỒ GIẢI THUẬT HỆ THỐNG AEGIS SIGHT
**Dự Án: Kính Thông Minh Trợ Thị Đa Trí Tuệ Nhân Tạo Dành Cho Người Khiếm Thị**

---

## 🏛️ 1. LƯU ĐỒ KIẾN TRÚC TOÀN HỆ THỐNG (SYSTEM ARCHITECTURE FLOWCHART)

Sơ đồ thể hiện sự phân bổ xử lý giữa phần cứng nhúng **Dual-Core FreeRTOS (ESP32-S3)** và **Hạ Tầng Dịch Vụ AI Đám Mây Bên Thứ 3**:

```mermaid
---
config:
  layout: dagre
---
flowchart TB
 subgraph C1["Core 1: Xử Lý Phần Cứng & Cảm Biến Thời Gian Thực 40Hz"]
        A1["Cử chỉ nút bấm: Bấm giữ / Bấm đúp / Nhấp nhanh"]
        A2["Micro INMP441: Thu âm"]
        A3["Camera GC2145: Chụp ảnh RGB565 320x240"]
        A4["MPU6050 Motion Gate: Lọc xung lực bước chân"]
        A5["Siêu âm HC-SR04: Đo khoảng cách vật cản"]
        A6["MPU6050 Fall Detection: Nhận diện té ngã"]
        A7["Loa MAX98357A: Phát tiếng chuông, nhạc chờ và Giọng đọc AI"]
  end
 subgraph C0["Core 0: Giao Thức Mạng & Pipeline Trí Tuệ Nhân Tạo"]
        B1["Kết nối wifi trước trong lúc nói"]
        B2["Xử lý STT: Gửi âm thanh lên API STT bên thứ 3 (~0.3s)"]
        B3["Bộ Điều Phối AI Đa Tầng: Tầng chính và dự phòng"]
        B4["Xử lý TTS: Tải luồng âm thanh từ API TTS bên thứ 3"]
        B5["Bộ giải mã âm thanh"]
  end
 subgraph S_EDGE["THIẾT BỊ ĐEO ĐẦU - ESP32-S3 DUAL-CORE"]
    direction TB
        C1
        C0
  end
 subgraph S_CLOUD["HẠ TẦNG DỊCH VỤ AI ĐÁM MÂY BÊN THỨ 3"]
        C_STT["API STT Bên Thứ 3 (Tầng Chính / Tầng Dự Phòng)"]
        C_LLM1["AI LLM Đa Phương Thức Bên Thứ 3 (Tầng chính)"]
        C_LLM2["AI LLM Thị Giác Bên Thứ 3 (Tầng 2 - Multi-turn Fallback)"]
        C_TTS["API TTS Đọc Giọng Nói Bên Thứ 3"]
  end
    A1 --> A2 & A3
    A2 --> B2
    A3 --> B3
    B1 --> B2
    B2 --> C_STT
    C_STT -- Văn bản câu hỏi nhận diện --> B3
    B3 -- Tầng 1: Session ID + Prompt + Ảnh --> C_LLM1
    C_LLM1 -. Đảo tầng khi mất kết nối/quá tải .-> C_LLM2
    C_LLM1 -- Streaming câu trả lời thời gian thực --> B4
    C_LLM2 -- Token Stream thời gian thực --> B4
    B4 --> C_TTS
    C_TTS --> B5
    B5 --> A7
    A4 -- Cho phép cảnh báo bíp --> A5
    A6 -- Kích hoạt còi báo động --> A7
```

---

## 🧠 2. LƯU ĐỒ GIẢI THUẬT AI PIPELINE ĐA TẦNG (MULTIMODAL AI PIPELINE)

Sơ đồ thể hiện chu trình nhận diện cử chỉ, chuyển đổi giọng nói thành văn bản qua API STT bên thứ 3, cây quyết định chuyển tầng AI (Failover Logic) và phát âm thanh thời gian thực (TTS Streaming):

```mermaid
flowchart TB
    Start(["Bắt đầu: Người dùng tương tác nút bấm"]) --> CheckGesture{"Phân tích cử chỉ nút bấm"}
    CheckGesture -- "Bấm giữ >= 250ms" --> G1["Bấm giữ đơn: Thu âm câu hỏi mới\nPhát tiếng Tít đơn\nReset trí nhớ cũ"]
    CheckGesture -- "Bấm đúp: Nhấp 1 cái -> Bấm giữ cái 2" --> G2["Bấm đúp: Thu âm câu hỏi nối tiếp\nPhát tiếng Tít-Tít đôi\nGiữ nguyên ảnh và ngữ cảnh cũ"]
    CheckGesture -- Nhấp nhanh 1 cái (&lt;250ms) --> G3["Nhấp nhanh: Chụp ảnh mô tả ngay\nKhông thu âm giọng nói"]
    G1 --> NetInit["Core 0: Bắt tay Wi-Fi và Chụp ảnh JPEG"]
    G2 --> NetInit
    G3 --> NetInit
    NetInit --> WaitTone["Phát nhạc chờ Offline trong PSRAM"]
    WaitTone --> STT{"Có dữ liệu âm thanh giọng nói?"}
    STT -- Có --> STT_P["Gửi WAV sang API STT Bên Thứ 3 tầng chính"]
    STT -- Không --> PromptGen["Tạo Prompt: Yêu cầu mô tả quang cảnh phía trước"]
    STT_P --> STTOk{"STT thành công?"}
    STTOk -- Thành công --> PromptGen
    STTOk -- Lỗi mạng --> STT_F["Chuyển sang API STT Bên Thứ 3 Tầng Dự Phòng"]
    STT_F --> PromptGen
    PromptGen --> LLM_Tier1["TẦNG 1: AI LLM Bên Thứ 3"]
    LLM_Tier1 --> CheckLLM1{"Tầng 1 phản hồi?"}
    CheckLLM1 -- Thành công: SSE Stream --> StreamTTS["Nhận từng từ -> Ngắt câu -> Tải và Phát qua API TTS bên thứ 3"]
    CheckLLM1 -- Lỗi mạng / Hết Context / Quá tải --> LLM_Tier2["TẦNG 2: AI LLM Bên Thứ 3 (Multi-turn PSRAM Memory)"]
    LLM_Tier2 --> CheckLLM2{"Tầng 2 phản hồi?"}
    CheckLLM2 -- Thành công --> StreamTTS
    CheckLLM2 -- Bị giới hạn tần suất --> SwapModel["Tự động đổi Model dự phòng và thử lại"]
    SwapModel --> StreamTTS
    CheckLLM2 -- Thất bại cả 2 vòng --> ErrorBeep["Phát tiếng bíp cảnh báo lỗi kết nối"]
    StreamTTS --> Done(["Kết thúc: Phát chuông hoàn thành"])
```

---

## 🚶 3. LƯU ĐỒ CẢM BIẾN

```mermaid
flowchart TB
    Start(["Khởi động chu kỳ lấy mẫu MPU6050 (40Hz / 25ms)"]) --> ReadSensors[/"Đọc cảm biến gia tốc MPU6050 (ax, ay, az)"/]
    ReadSensors --> CalcSV["Tính gia tốc tổng hợp: SV = sqrt(ax² + ay² + az²)"]
    CalcSV --> Fall1{"Pha 1: Rơi tự do?<br>(SV &lt; 0.5G trong 150-300ms)"}
    Fall1 -- ĐÚNG --> Fall2{"Pha 2: Va đập mặt đất?<br>(SV &gt; 2.5G trong 300ms kế tiếp)"}
    Fall2 -- ĐÚNG --> Fall3{"Pha 3: Nằm bất động?<br>(SV ≈ 1.0G duy trì 2 giây)"}
    Fall3 -- ĐÚNG: Xác nhận té ngã --> FallWindow["Mở cửa sổ đếm ngược hủy 10 giây<br>Phát tiếng bíp nhắc nhở"]
    FallWindow --> FallCancelBtn{"Người dùng có bấm nút Trigger?"}
    FallCancelBtn -- CÓ: Báo động giả --> FallReset["Hủy cảnh báo ngã & Trở về trạng thái thường"]
    FallCancelBtn -- KHÔNG (Hết 10s): Bất tỉnh --> FallAlarm[/"PHÁT CÒI CỨU HỘ SOS QUA LOA"/]
    Fall1 -- SAI --> CalcMotion["Tính StdDev & P2P trong cửa sổ 600ms"]
    Fall2 -- SAI --> CalcMotion
    Fall3 -- SAI (Có cử động lại) --> CalcMotion
    FallReset --> EndCycle(["Chờ chu kỳ 25ms tiếp theo"])
    FallAlarm --> EndCycle
    CalcMotion --> MotionCheck{"StdDev &gt;= 0.12G<br>VÀ P2P &gt;= 0.25G<br>liên tục &gt;= 600ms?"}
    MotionCheck -- SAI: Đứng yên / Quay đầu --> GateOff["Tắt cảnh báo siêu âm (Giữ yên tĩnh)"]
    GateOff --> EndCycle
    MotionCheck -- ĐÚNG: Đang bước đi --> GateOn[/"Kích hoạt đo khoảng cách HC-SR04"/]
    GateOn --> DistCheck{"Khoảng cách vật cản (D)"}
    DistCheck -- "D &lt;= 18cm" --> Beep1[/"Bíp dồn dập 180ms: Vùng Nguy Hiểm"/]
    DistCheck -- "18cm &lt; D &lt;= 32cm" --> Beep2[/"Bíp vừa 320ms: Vùng Cảnh Báo"/]
    DistCheck -- "32cm &lt; D &lt;= 45cm" --> Beep3[/"Bíp chậm 550ms: Vùng An Toàn"/]
    DistCheck -- D &gt; 45cm --> Beep4["Không phát tiếng bíp"]
    Beep1 --> EndCycle
    Beep2 --> EndCycle
    Beep3 --> EndCycle
    Beep4 --> EndCycle

    %% KHUNG CHÚ GIẢI THUẬT NGỮ
    subgraph LEGEND ["📝 CHÚ GIẢI"]
        direction TB
        N_SV["• SV (Signal Vector Magnitude): Độ lớn vector gia tốc tổng hợp 3 trục (Bất biến hướng đeo)<br>• G: Đơn vị gia tốc"]
        N_MOTION["• StdDev (Standard Deviation): Độ lệch chuẩn đo mức phân tán dao động bước chân<br>• P2P (Peak-to-Peak): Biên độ chênh lệch cực đại giữa đỉnh và đáy gia tốc"]
    end

    %% Liên kết chú thích nét đứt theo quy chuẩn
    CalcSV -.- N_SV
    CalcMotion -.- N_MOTION

    classDef legendStyle fill:#0f172a,stroke:#38bdf8,stroke-width:1px,stroke-dasharray: 4 4,color:#e2e8f0,text-align:left;
    class LEGEND,N_SV,N_MOTION,N_HARDWARE legendStyle;
```
