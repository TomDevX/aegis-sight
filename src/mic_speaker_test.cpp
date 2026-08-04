#include <Arduino.h>
#include "config.h"
#include "driver/i2s.h"
#include "driver/ledc.h"
#include "esp_timer.h"

#ifdef ENABLE_MIC_SPEAKER_TEST

#define MIC_SAMPLE_RATE      16000
#define BUFFER_SIZE          1024

// Cấu hình PWM cho Grove Speaker
#define PWM_CHANNEL          2
#define PWM_RES_BITS         8        // 8-bit resolution (0..255)
#define PWM_RES_MAX          255
#define PWM_CARRIER_HZ       64000
#define PWM_DC_MID           127      // Điểm cân bằng Zero-point (127)

// BIẾN CẤU HÌNH ÂM LƯỢNG (GAIN)
// Tăng Gain để giọng nói to hơn. Nếu bị vỡ tiếng thì giảm xuống (ví dụ 2.0f)
#define AUDIO_GAIN           2.5f     

// Ring Buffer cho Audio Output
static uint8_t audio_ring_buf[BUFFER_SIZE];
static volatile uint16_t read_ptr = 0;
static volatile uint16_t write_ptr = 0;

static int32_t *rxBuf = NULL;
static esp_timer_handle_t timer_handle;

// Bộ lọc DC Blocking
static float dc_offset = 0.0f;

// Bộ lọc Low-pass mềm trong ISR
static float lpf_state = 127.0f;

// --- ISR: TIMER NGẮT CỨNG CHẠY 16.000 LẦN / GIÂY (62.5 micro-giây/lần) ---
static void IRAM_ATTR onPwmTimer(void* arg) {
    if (read_ptr != write_ptr) {
        uint8_t raw_val = audio_ring_buf[read_ptr];
        
        // Bộ lọc Low-Pass 1-pole đơn giản để gọt phẳng sóng PWM trước khi ra loa
        lpf_state = 0.6f * lpf_state + 0.4f * (float)raw_val;
        
        ledcWrite(PWM_CHANNEL, (uint32_t)lpf_state);
        read_ptr = (read_ptr + 1) % BUFFER_SIZE;
    } else {
        // Duy trì áp mức giữa (DC Mid) khi thiếu dữ liệu để không bị nổ/tạch
        ledcWrite(PWM_CHANNEL, PWM_DC_MID);
    }
}

static bool initMicI2S(void) {
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
        .use_apll = false,
    };

    i2s_pin_config_t pin_cfg = {
        .bck_io_num = MIC_BCLK,
        .ws_io_num = MIC_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_DATA_IN,
    };

    esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &i2s_cfg, 0, NULL);
    if (err != ESP_OK) return false;
    
    err = i2s_set_pin(I2S_MIC_PORT, &pin_cfg);
    if (err != ESP_OK) {
        i2s_driver_uninstall(I2S_MIC_PORT);
        return false;
    }
    return true;
}

static bool initPwmSpeaker(void) {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = (ledc_timer_t)LEDC_TIMER_1;
    timer.duty_resolution = (ledc_timer_bit_t)PWM_RES_BITS;
    timer.freq_hz = PWM_CARRIER_HZ;
    timer.clk_cfg = LEDC_USE_APB_CLK;
    
    if (ledc_timer_config(&timer) != ESP_OK) return false;

    ledcAttachPin(SPK_PWM_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, PWM_DC_MID);

    // Hardware Timer ngắt chính xác 62 micro-giây (~16000Hz)
    const esp_timer_create_args_t timer_args = {
        .callback = &onPwmTimer,
        .arg = NULL,
        .name = "pwm_audio_timer"
    };
    esp_timer_create(&timer_args, &timer_handle);
    esp_timer_start_periodic(timer_handle, 62); 

    return true;
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("=== MIC INMP441 -> GROVE SPEAKER (GAIN & LPF ENHANCED) ===");

    rxBuf = (int32_t *)ps_malloc(128 * sizeof(int32_t));
    if (!rxBuf) {
        Serial.println("[MIC_SPK] FATAL: ps_malloc failed!");
        return;
    }

    if (!initMicI2S() || !initPwmSpeaker()) {
        Serial.println("[MIC_SPK] Hardware Init Failed!");
        return;
    }
    Serial.println("[MIC_SPK] Ready! Hãy nói vào Mic...");
}

void loop() {
    size_t bytes_read = 0;
    
    // Đọc dữ liệu từ Mic I2S
    esp_err_t res = i2s_read(I2S_MIC_PORT, rxBuf, 128 * sizeof(int32_t),
                             &bytes_read, portMAX_DELAY);
    if (res != ESP_OK || bytes_read == 0) return;

    uint32_t samples = bytes_read / sizeof(int32_t);

    for (uint32_t i = 0; i < samples; i++) {
        // 1. Lấy dữ liệu 24-bit MSB từ INMP441
        int32_t raw24 = rxBuf[i] >> 8; 
        float sample = (float)raw24;

        // 2. Bộ lọc loại bỏ DC offset
        dc_offset = 0.99f * dc_offset + 0.01f * sample;
        float clean = sample - dc_offset;

        // 3. Tăng Gain để giọng nói to hơn (chia cho 32768.0f thay vì 65536.0f và nhân AUDIO_GAIN)
        float scaled = (clean / 32768.0f) * AUDIO_GAIN;

        // Clamp dải âm thanh tránh bị vỡ/méo biên độ (Clipping)
        if (scaled > 127.0f) scaled = 127.0f;
        if (scaled < -127.0f) scaled = -127.0f;

        int32_t val = PWM_DC_MID + (int32_t)scaled;

        if (val < 0) val = 0;
        if (val > PWM_RES_MAX) val = PWM_RES_MAX;

        // Nạp vào Ring Buffer để Timer đẩy ra loa
        uint16_t next_write = (write_ptr + 1) % BUFFER_SIZE;
        if (next_write != read_ptr) { 
            audio_ring_buf[write_ptr] = (uint8_t)val;
            write_ptr = next_write;
        }
    }
}

#endif