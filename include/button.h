#ifndef BUTTON_H
#define BUTTON_H

#include <Arduino.h>

// ============================================================
// BUTTON DEBOUNCE (phần mềm) — bù cho nút không có tụ debounce
//
// Nguyên lý: một esp_timer lấy mẫu chân nút mỗi 2ms và đưa vào
// "bộ tích phân" (integrator). Trạng thái chỉ đổi khi tín hiệu
// giữ ổn định đủ ~20ms. Nhiễu rung phím vài ms KHÔNG bao giờ
// làm đổi trạng thái -> vòng lặp "đang giữ nút" không bị đứt.
//
// TẤT CẢ code khác phải dùng button_is_down() thay cho
// digitalRead(BTN_TRIGGER) — đọc trực tiếp là nguồn gốc của lỗi
// "bấm giữ không ăn".
// ============================================================

// Thời gian tín hiệu phải ổn định trước khi được công nhận (ms)
#define BTN_DEBOUNCE_MS      20
// Chu kỳ lấy mẫu (us)
#define BTN_SAMPLE_US        2000

void     button_init(void);

// Trạng thái đã lọc nhiễu: true = đang bị đè
bool     button_is_down(void);

// Lấy 1 sự kiện "vừa nhấn xuống" (falling edge sạch). Trả về true
// đúng 1 lần cho mỗi lần nhấn thật. Nếu có nhiều sự kiện dồn lại
// thì chỉ tính là 1.
bool     button_take_press_event(void);

// Xoá sự kiện đang chờ (gọi sau khi xử lý xong 1 thao tác để
// nhiễu lúc nhả nút không kích hoạt lần chạy mới)
void     button_clear_events(void);

// Số ms trạng thái hiện tại đã được giữ ổn định
uint32_t button_state_duration_ms(void);

// Chờ nút được nhả ổn định, tối đa timeout_ms. true = đã nhả
bool     button_wait_release(uint32_t timeout_ms);

#endif // BUTTON_H
