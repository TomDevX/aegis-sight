#include "button.h"
#include "config.h"

// Số bước tích phân để công nhận đổi trạng thái
#define BTN_INTEG_MAX   (BTN_DEBOUNCE_MS * 1000 / BTN_SAMPLE_US)

static volatile int32_t  s_integrator   = 0;
static volatile bool     s_down         = false;
static volatile uint32_t s_lastChangeMs = 0;
static volatile uint32_t s_pressSeq     = 0;   // tăng 1 sau mỗi lần nhấn THẬT
static uint32_t          s_pressSeqRead = 0;

static bool s_inited = false;

// ------------------------------------------------------------
// Lấy 1 mẫu và đưa vào bộ tích phân.
// Nhiễu vài ms chỉ làm bộ đếm nhích lên/xuống chút xíu, không đủ
// chạm ngưỡng -> trạng thái ra ngoài luôn sạch tuyệt đối.
// ------------------------------------------------------------
static inline void btn_sample(void) {
    bool raw = (digitalRead(BTN_TRIGGER) == LOW);   // Active LOW (INPUT_PULLUP)

    if (raw) {
        if (s_integrator < BTN_INTEG_MAX) s_integrator++;
    } else {
        if (s_integrator > 0) s_integrator--;
    }

    // Hysteresis: chỉ đổi trạng thái tại 2 đầu mút
    if (s_integrator >= BTN_INTEG_MAX && !s_down) {
        s_down = true;
        s_lastChangeMs = millis();
        s_pressSeq++;
    } else if (s_integrator <= 0 && s_down) {
        s_down = false;
        s_lastChangeMs = millis();
    }
}

static void button_task(void *pv) {
    const TickType_t period = pdMS_TO_TICKS(BTN_SAMPLE_US / 1000);
    TickType_t last = xTaskGetTickCount();
    while (true) {
        btn_sample();
        vTaskDelayUntil(&last, period > 0 ? period : 1);
    }
}

void button_init(void) {
    if (s_inited) return;

    pinMode(BTN_TRIGGER, INPUT_PULLUP);
    delay(5);

    // Nạp sẵn trạng thái ban đầu để không mất 20ms đầu tiên
    bool raw = (digitalRead(BTN_TRIGGER) == LOW);
    s_integrator   = raw ? BTN_INTEG_MAX : 0;
    s_down         = raw;
    s_lastChangeMs = millis();
    s_pressSeq     = 0;
    s_pressSeqRead = 0;

    s_inited = true;
    // Ưu tiên 6: cao hơn task audio (5) để việc lấy mẫu không bao giờ bị trễ
    xTaskCreatePinnedToCore(button_task, "btn_debounce", 2048, NULL, 5, NULL, 0);
    Serial.printf("[BTN] Debounce phần mềm %dms đã bật trên GPIO %d\n",
                  BTN_DEBOUNCE_MS, BTN_TRIGGER);
}

bool button_is_down(void) {
    return s_down;
}

bool button_take_press_event(void) {
    uint32_t seq = s_pressSeq;
    if (seq != s_pressSeqRead) {
        s_pressSeqRead = seq;      // gộp mọi sự kiện dồn lại thành 1
        return true;
    }
    return false;
}

void button_clear_events(void) {
    s_pressSeqRead = s_pressSeq;
}

uint32_t button_state_duration_ms(void) {
    return millis() - s_lastChangeMs;
}

bool button_wait_release(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (button_is_down()) {
        if (millis() - t0 > timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}
