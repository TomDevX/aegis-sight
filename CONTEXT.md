# AGENT_CONTEXT: Aegis Sight - AI Assistive Headband for Visually Impaired

## 1. PROJECT OVERVIEW & FORM FACTOR
- **Sản phẩm:** Aegis Sight (Băng cài tóc thông minh hỗ trợ người khiếm thị).
- **Thiết kế Cơ khí (Form Factor - Smart Headband):**
  - Khung cài tóc (Headband) in 3D rỗng ruột để luồn cáp nguồn/tín hiệu nối giữa 2 bên.
  - **Hộp Trái (Left Module):** Chứa Mạch nguồn Buck Mini360, Mic INMP441, Cảm biến gia tốc MPU6050, Nút nhấn Trigger (Hỏi AI / Hủy báo động).
  - **Hộp Phải (Right Module):** Chứa Bo mạch ESP32-S3-N16R8-CAM (hướng Camera OV2640 ra trước), Cảm biến siêu âm HC-SR04P (hướng ra trước), Mạch DAC PCM5102A & Loa I2S (hướng vào tai).

---

## 2. FEATURE REQUIREMENTS & LOGIC

### A. Tính năng AI chính (Trigger-based Ultra-Low Latency Pipeline)
- **Kiến trúc:** 1 Session WebSocket duy nhất kết nối tới Gemini Multimodal Live API (Bi-directional Stream).
- **Phân công FreeRTOS Dual-Core:**
  - **Core 0 (Networking & WebSocket):** Khởi tạo Socket, xử lý giao thức WS, gửi/nhận Binary Audio/Image Frame, quản lý RingBuffer trong PSRAM.
  - **Core 1 (Hardware & DSP):** Đọc Mic INMP441, Chụp ảnh OV2640, Đẩy Audio out PCM5102A, Đọc Siêu âm / MPU6050.
- **Workflow Tối Tốc Độ (Target Latency < 1.2s):**
  1. **User Triggers:** Bấm nút -> Core 0 mở kết nối WebSocket, Core 1 chụp $1$ Frame SVGA ($800 \times 600$, JPEG Quality 10).
  2. **Fast Streaming:** Core 0 gửi ngay Frame JPEG + Stream gói PCM Raw Binary ($16\text{kHz}$ $16\text{-bit}$) thu từ Mic INMP441 lên Socket.
  3. **Stream Playback (Cuốn chiếu):** Ngay khi nhận Chunk Audio PCM phản hồi đầu tiên từ Gemini, Core 1 đẩy thẳng ra Loa I2S DAC (không chờ nhận xong toàn bộ câu thoại mới phát).
  4. **Session Cleanup:** Kết thúc câu thoại -> Đóng WebSocket, giải phóng PSRAM Buffer, trả CPU về IDLE.

### B. Cảm biến Cảnh báo Vật cản (Ultrasonic Proximity Beep)
- **Cảm biến:** Cảm biến siêu âm HC-SR04P ($3.3\text{V}$) hướng chính diện (chếch $15^\circ$ xuống).
- **Logic "Vật cản càng gần -> Loa Bíp Bíp càng nhanh":**
  - Khoảng cách $D > 1.5\text{m}$: Yên lặng (Không bíp).
  - $1.0\text{m} < D \le 1.5\text{m}$: Bíp chậm (nhịp $600\text{ms}$).
  - $0.5\text{m} < D \le 1.0\text{m}$: Bíp vừa (nhịp $300\text{ms}$).
  - $D \le 0.5\text{m}$: Bíp dồn dập / Tiếng bíp kéo dài liên tục (Khoảng cách nguy hiểm!).
- *Lưu ý:* Âm thanh bíp bíp được tổng hợp qua DAC I2S dạng Panned Audio hoặc âm tone ngắn, chạy song song không làm gián đoạn luồng AI.

### C. Cảm biến Té ngã (Fall Detection -> Local SOS Alarm)
- **Cảm biến:** MPU6050 (giao tiếp I2C trên Hộp Trái).
- **Logic 3 Pha chuẩn xác:** 
  1. *Rơi tự do (Free-fall):* Gia tốc tổng $SV = \sqrt{a_x^2 + a_y^2 + a_z^2} < 0.5g$.
  2. *Va chạm (Impact):* $SV > 2.5g$ xuất hiện trong vòng $300\text{ms}$ tiếp theo.
  3. *Bất động (Inactivity):* Cơ thể nằm yên ($SV \approx 1g$) trong $2\text{s}$.
- **Hành động:** Khi thỏa mãn cả 3 pha -> Bíp nhẹ + rung 10s chờ hủy -> Nếu người dùng không bấm nút hủy (Trigger Button), **Loa I2S phát tiếng Còi hú SOS (Buzzer Alarm)** ở mức âm lượng tối đa $100\%$ để gọi trợ giúp xung quanh.

### D. Tự động điều chỉnh Âm lượng theo Môi trường (Auto-Volume Adjust)
- **Cảm biến:** Dùng chính Mic INMP441 thu âm tiếng ồn môi trường xung quanh khi người dùng KHÔNG nói chuyện.
- **Logic:**
  1. Đọc chuỗi mẫu PCM từ Mic -> Tính giá trị công suất âm thanh **RMS (Root Mean Square)**:
     $$\text{RMS} = \sqrt{\frac{1}{N} \sum_{i=1}^{N} x_i^2}$$
  2. Map tự động giá trị RMS môi trường (phòng kín yên tĩnh vs. ngoài đường xe cộ) sang dải Volume của thư viện `ESP32-audioI2S` (`audio.setVolume(1-21)`).
  3. Giúp âm thanh cảnh báo bíp bíp và giọng nói AI phát ra vừa đủ nghe, tự động to lên khi ra đường ồn và nhỏ lại khi ở trong nhà.

---

## 3. HARDWARE PINOUT MAPPING (ESP32-S3-N16R8-CAM)
> **CẢNH BÁO MẠCH:** OPI PSRAM chiếm dụng các chân GPIO 33, 34, 35, 36, 37. Các chân dưới đây KHÔNG ĐỤNG ĐỘ với khe cắm FPC Camera và PSRAM.

### A. Camera OV2640 (Khe FPC DVP Bus cố định trên Hộp Phải)
- PWDN: -1 | RESET: -1 | XCLK: GPIO 10 | SIOD (SDA): GPIO 4 | SIOC (SCL): GPIO 5
- Y9: GPIO 16 | Y8: GPIO 17 | Y7: GPIO 18 | Y6: GPIO 12 | Y5: GPIO 11 | Y4: GPIO 6 | Y3: GPIO 7 | Y2: GPIO 15
- VSYNC: GPIO 38 | HREF: GPIO 21 | PCLK: GPIO 13

### B. Microphone INMP441 (I2S RX - Channel 0 - Dây đi qua gờ Cài tóc sang Hộp Trái)
- SCK (BCLK): GPIO 41 | WS (LRCK): GPIO 42 | SD (Data In): GPIO 2 | L/R: GND

### C. Loa / I2S DAC PCM5102A (I2S TX - Channel 1 - Hộp Phải)
- BCLK: GPIO 1 | LRCK (WS): GPIO 3 | DIN (Data Out): GPIO 40

### D. Sensors & Peripherals
- **Trigger Button (Nút bấm AI / Hủy SOS - Hộp Trái):** GPIO 14 (Internal Pull-Up, Active LOW)
- **Ultrasonic (Siêu âm HC-SR04P - Hộp Phải):** Trig = GPIO 8, Echo = GPIO 9
- **MPU6050 (Té ngã - Hộp Trái) / I2C:** SDA = GPIO 47, SCL = GPIO 48

---

## 4. TECHNICAL STACK & LIBRARIES (`platformio.ini`)
- **Framework:** Arduino ESP32 Core (v3.x+)
- **Target Board:** `esp32-s3-devkitc-1` (Flash: 16MB QIO, PSRAM: 8MB OPI)
- **Libraries:** `esp32-camera`, `schreibfaul1/ESP32-audioI2S`, `bblanchon/ArduinoJson`, `links2004/WebSockets`, `Adafruit_MPU6050`.

---

## 5. SYSTEM AGENT RULES (MANDATORY FOR OPENCODE)

1. **FreeRTOS Dual-Core Architecture:**
   - **Core 0:** Đảm nhận duy trì kết nối Wi-Fi, WebSocket Stream dữ liệu 2 chiều với Gemini Live API và quản lý RingBuffer trên PSRAM.
   - **Core 1:** Liên tục đọc MPU6050 (Té ngã), đọc Siêu âm (Tính khoảng cách bíp bíp), đọc Mic (Tính RMS Auto Volume), phát âm thanh I2S.

2. **Zero-Delay & Non-blocking Code:**
   - Tuyệt đối KHÔNG dùng `delay()`. Mọi tác vụ phát bíp bíp, đọc cảm biến và quét nút bấm phải chạy dựa trên `millis()` hoặc FreeRTOS Software Timers (`vTaskDelay`).

3. **Memory Management:**
   - Cấp phát toàn bộ bộ đệm lớn (JPEG Frame, Audio PCM Buffer, WebSocket Tx/Rx Queue) trên **OPI PSRAM** (`ps_malloc`).

4. **Code Quality:**
   - Viết code C++ hoàn chỉnh, không cắt đoạn (`// insert code here`), comment giải thích ngắn gọn, rõ ràng.

---

## 6. ITERATIVE ROADMAP FOR AGENT
- **Chặng 1 (Hardware Validation):** Code test độc lập Cam OV2640, Mic INMP441, Loa I2S PCM5102A, Siêu âm, MPU6050.
- **Chặng 2 (Real-time Local Features):** Lập trình tác vụ Core 1: Đọc Siêu âm -> Phát bíp bíp theo khoảng cách; Đọc Mic RMS -> Auto Volume; Đọc MPU6050 -> Hú còi SOS.
- **Chặng 3 (Gemini Live Stream Pipeline):** Bấm Nút Trigger -> Mở WebSocket -> Stream JPEG + PCM Audio -> Nhận PCM Audio Stream cuốn chiếu -> Đẩy ra Loa I2S.
- **Chặng 4 (Integration & Latency Tuning):** Ghép toàn bộ mô-đun, tinh chỉnh độ trễ phản hồi Gemini Live $< 1.2\text{s}$.