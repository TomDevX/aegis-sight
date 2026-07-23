# AGENT_CONTEXT: Aegis Sight - AI Assistive Headband for Visually Impaired

## 1. PROJECT OVERVIEW & FORM FACTOR
- **Sản phẩm:** Aegis Sight (Băng cài tóc thông minh hỗ trợ người khiếm thị).
- **Thiết kế Cơ khí & Phân bổ Trọng lượng (Smart Headband Form Factor):**
  - Khung cài tóc (Headband) in 3D rỗng ruột để luồn cáp nguồn/tín hiệu nối giữa 2 bên.
  - **Hộp Trái (Left Module - ~22g):** Chứa Mạch nguồn Buck Mini360, Mic INMP441, Cảm biến gia tốc MPU6050, Giắc cắm cáp nguồn rủ xuống túi quần (Pin Li-ion / Powerbank đặt trong túi).
  - **Hộp Phải (Right Module - ~28g):** Chứa Bo mạch ESP32-S3-N16R8-CAM (hướng Camera OV2640 ra trước), Cảm biến siêu âm HC-SR04 (hướng ra trước, nghiêng chếch 15°), Loa Seeed Grove I2S Speaker (hướng về phía sau tai).

---

## 2. FEATURE REQUIREMENTS & LOGIC

### A. Tính năng AI chính (Trigger-based Ultra-Low Latency Pipeline)
- **Kiến trúc:** 1 Session WebSocket duy nhất kết nối tới Gemini Multimodal Live API (Bi-directional Stream).
- **Phân công FreeRTOS Dual-Core:**
  - **Core 0 (Networking & WebSocket):** Khởi tạo Socket, xử lý giao thức WS, gửi/nhận Binary Audio/Image Frame, quản lý RingBuffer trong PSRAM.
  - **Core 1 (Hardware & DSP):** Đọc Mic INMP441, Chụp ảnh OV2640, Đẩy Audio out Loa Seeed Grove I2S Speaker, Đọc Siêu âm / MPU6050.
- **Workflow Tối Tốc Độ (Target Latency < 1.2s):**
  1. **User Triggers:** Mở kết nối WebSocket, Core 1 chụp $1$ Frame SVGA ($800 \times 600$, JPEG Quality 10).
  2. **Fast Streaming:** Core 0 gửi ngay Frame JPEG + Stream gói PCM Raw Binary ($16\text{kHz}$ $16\text{-bit}$) thu từ Mic INMP441 lên Socket.
  3. **Stream Playback (Cuốn chiếu):** Ngay khi nhận Chunk Audio PCM phản hồi đầu tiên từ Gemini, Core 1 đẩy thẳng ra Loa Seeed Grove I2S Speaker (không chờ nhận xong toàn bộ câu thoại mới phát).
  4. **Session Cleanup:** Kết thúc câu thoại -> Đóng WebSocket, giải phóng PSRAM Buffer, trả CPU về IDLE.

### B. Cảm biến Cảnh báo Vật cản (Ultrasonic Proximity Beep)
- **Cảm biến:** Cảm biến siêu âm HC-SR04 ($3.3\text{V}$) hướng chính diện (chếch $15^\circ$ xuống).
- **Logic "Vật cản càng gần -> Loa Bíp Bíp càng nhanh":**
  - Khoảng cách $D > 1.5\text{m}$: Yên lặng (Không bíp).
  - $1.0\text{m} < D \le 1.5\text{m}$: Bíp chậm (nhịp $600\text{ms}$).
  - $0.5\text{m} < D \le 1.0\text{m}$: Bíp vừa (nhịp $300\text{ms}$).
  - $D \le 0.5\text{m}$: Bíp dồn dập / Tiếng bíp kéo dài liên tục (Khoảng cách nguy hiểm!).
- *Lưu ý:* Âm thanh bíp bíp được tổng hợp qua Loa Seeed Grove I2S Speaker dạng tone ngắn, chạy song song không làm gián đoạn luồng AI.

### C. Cảm biến Té ngã (Fall Detection -> Local SOS Alarm qua Loa I2S)
- **Cảm biến:** MPU6050 (giao tiếp I2C trên Hộp Trái).
- **Logic 3 Pha chuẩn xác:** 
  1. *Rơi tự do (Free-fall):* Gia tốc tổng $SV = \sqrt{a_x^2 + a_y^2 + a_z^2} < 0.5g$.
  2. *Va chạm (Impact):* $SV > 2.5g$ xuất hiện trong vòng $300\text{ms}$ tiếp theo.
  3. *Bất động (Inactivity):* Cơ thể nằm yên ($SV \approx 1g$) trong $2\text{s}$.
- **Hành động:** Khi thỏa mãn cả 3 pha -> Bíp nhẹ 10s chờ phản hồi -> Nếu không có tương tác hủy, **Loa Seeed Grove I2S Speaker phát trực tiếp tiếng Còi hú SOS (Software Synthesized Alarm)** ở mức âm lượng tối đa $100\%$ qua luồng I2S (Không dùng còi chíp vật lý / Không dùng chân GPIO rời).

### D. Tự động điều chỉnh Âm lượng theo Môi trường (Auto-Volume Adjust)
- **Cảm biến:** Dùng chính Mic INMP441 thu âm tiếng ồn môi trường xung quanh khi người dùng KHÔNG nói chuyện.
- **Logic:**
  1. Đọc chuỗi mẫu PCM từ Mic -> Tính giá trị công suất âm thanh **RMS (Root Mean Square)**:
     $$\text{RMS} = \sqrt{\frac{1}{N} \sum_{i=1}^{N} x_i^2}$$
  2. Map tự động giá trị RMS môi trường (phòng kín yên tĩnh vs. ngoài đường xe cộ) sang dải Volume của thư viện `ESP32-audioI2S` (`audio.setVolume(1-21)`).
  3. Giúp giọng nói AI, tiếng bíp vật cản và tiếng báo động phát ra vừa đủ nghe, tự động to lên khi ra đường ồn và nhỏ lại khi ở trong nhà.

---

## 3. HARDWARE PINOUT MAPPING (ESP32-S3-N16R8-CAM - Complete Hardware Set)

### A. Camera OV2640 (Hộp Phải - FPC DVP Bus)
- Chân cố định trên bo mạch ESP32-S3-CAM.

### B. Loa Seeed Grove I2S Speaker (Hộp Phải - I2S TX Channel 1)
- **VCC:** 5V / 3.3V | **GND:** GND
- **BCLK (SCK):** GPIO 1
- **LRCK (WS):** GPIO 3
- **DIN (Data In):** GPIO 40

### C. Cảm biến Siêu âm HC-SR04 (Hộp Phải)
- **VCC:** 3.3V | **GND:** GND
- **Trig:** GPIO 8
- **Echo:** GPIO 9

### D. Microphone INMP441 (Hộp Trái - I2S RX Channel 0 - Luồn dây qua Vòm Cài)
- **VCC:** 3.3V | **GND:** GND | **L/R:** GND
- **SD (Data Out):** GPIO 2
- **SCK (BCLK):** GPIO 41
- **WS (LRCK):** GPIO 42

### E. Cảm biến Té ngã MPU6050 (Hộp Trái - I2C Bus - Luồn dây qua Vòm Cài)
- **VCC:** 3.3V | **GND:** GND
- **SDA:** GPIO 47
- **SCL:** GPIO 48

### F. Cấu trúc Nguồn
- Pin Li-ion / Powerbank (Túi quần) -> Dây cáp rủ -> Giắc cắm Hộp Trái -> Mạch Buck Mini360 (Hạ áp xuống 5V/3.3V) -> Cấp nguồn cho Mic/MPU6050 & Luồn dây 5V/GND qua Vòm cài sang Hộp Phải nuôi ESP32-S3 + Siêu âm + Loa Grove.

---

## 4. TECHNICAL STACK & LIBRARIES (`platformio.ini`)
- **Framework:** Arduino ESP32 Core (v3.x+)
- **Target Board:** `esp32-s3-devkitc-1` (Flash: 16MB QIO, PSRAM: 8MB OPI)
- **Libraries:** `esp32-camera`, `schreibfaul1/ESP32-audioI2S`, `bblanchon/ArduinoJson`, `links2004/WebSockets`, `Adafruit_MPU6050`.

---

## 5. SYSTEM AGENT RULES (MANDATORY FOR OPENCODE)

1. **FreeRTOS Dual-Core Architecture:**
   - **Core 0:** Quản lý kết nối Wi-Fi, WebSocket Stream dữ liệu 2 chiều với Gemini Live API và RingBuffer trên PSRAM.
   - **Core 1:** Đọc Siêu âm (Tính khoảng cách bíp bíp), đọc MPU6050 (Té ngã), đọc Mic INMP441 (Tính RMS Auto Volume), phát âm thanh I2S ra Loa Seeed Grove I2S Speaker.

2. **Zero-Delay & Non-blocking Code:**
   - Tuyệt đối KHÔNG dùng `delay()`. Mọi tác vụ phát âm thanh bíp bíp, đọc cảm biến phải chạy dựa trên `millis()` hoặc FreeRTOS Software Timers (`vTaskDelay`).

3. **Memory Management:**
   - Cấp phát toàn bộ bộ đệm lớn (JPEG Frame, Audio PCM Buffer, WebSocket Tx/Rx Queue) trên **OPI PSRAM** (`ps_malloc`).

4. **Code Quality:**
   - Viết code C++ hoàn chỉnh, không cắt đoạn, comment giải thích ngắn gọn.

---

## 6. ITERATIVE ROADMAP FOR AGENT
- **Chặng 1 (Hardware Validation):** Code test độc lập Cam OV2640, Mic INMP441, Loa Seeed Grove I2S Speaker, Siêu âm HC-SR04, MPU6050.
- **Chặng 2 (Real-time Local Features):** Lập trình tác vụ Core 1: Đọc Siêu âm -> Bíp bíp dồn dập; Đọc Mic RMS -> Auto Volume; Đọc MPU6050 -> Tổng hợp sóng âm hú còi SOS ra Loa Seeed Grove I2S Speaker.
- **Chặng 3 (Gemini Live Stream Pipeline):** Mở WebSocket -> Stream JPEG + PCM Audio -> Nhận PCM Audio Stream cuốn chiếu -> Đẩy ra Loa Seeed Grove I2S Speaker.
- **Chặng 4 (Integration & Latency Tuning):** Ghép toàn bộ mô-đun, tinh chỉnh độ trễ phản hồi Gemini Live $< 1.2\text{s}$.