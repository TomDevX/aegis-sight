#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "config.h"
#include "img_converters.h"

#ifdef ENABLE_CAMERA

const char* ssid = "Homepro";
const char* password = "Sunny20061@3";

httpd_handle_t stream_httpd = NULL;

static esp_err_t jpg_stream_httpd_handler(httpd_req_t *req) {
    int64_t t_start = esp_timer_get_time();

    // 1. Chụp frame RGB565 thô từ PSRAM
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;

    // 2. Nén JPEG với Quality = 20
    bool converted = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 20, &jpg_buf, &jpg_len);
    esp_camera_fb_return(fb); 

    if (!converted) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 3. Phản hồi luồng Byte JPEG về client/AI
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t res = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);

    int64_t t_end = esp_timer_get_time();
    Serial.printf("[SPEED] Size: %d Bytes | Time: %d ms\n", 
                  (int)jpg_len, (int)((t_end - t_start) / 1000));

    free(jpg_buf);
    return res;
}

void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_uri_t capture_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = jpg_stream_httpd_handler,
        .user_ctx  = NULL
    };

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &capture_uri);
    }
}

static bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_Y2;
    config.pin_d1       = CAM_Y3;
    config.pin_d2       = CAM_Y4;
    config.pin_d3       = CAM_Y5;
    config.pin_d4       = CAM_Y6;
    config.pin_d5       = CAM_Y7;
    config.pin_d6       = CAM_Y8;
    config.pin_d7       = CAM_Y9;
    config.pin_xclk     = CAM_XCLK;
    config.pin_pclk     = CAM_PCLK;
    config.pin_vsync    = CAM_VSYNC;
    config.pin_href     = CAM_HREF;
    config.pin_sccb_sda = CAM_SIOD;
    config.pin_sccb_scl = CAM_SIOC;
    config.pin_pwdn     = CAM_PWDN;
    config.pin_reset    = CAM_RESET;
    
    config.xclk_freq_hz = 15000000;       // Xung nhịp an toàn cho SVGA (15MHz)
    config.pixel_format = PIXFORMAT_RGB565; 
    config.frame_size   = FRAMESIZE_SVGA; // 800x600 mở rộng góc nhìn
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    // 1. Khởi tạo camera trước
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    // 2. Lấy con trỏ cảm biến SAU KHI đã khởi tạo thành công để cấu hình phơi sáng
    // mode: stable
    if (s) {
        s->set_exposure_ctrl(s, 0); // Tắt auto exposure
        s->set_aec_value(s, 300);   // Cố định mức phơi sáng
        s->set_gain_ctrl(s, 0);     // Tắt auto gain
        s->set_agc_gain(s, 0);      // Cố định gain thấp
    }

    // mode: environment adaptable. Giờ tôi 
    // if (s) {
    //     s->set_exposure_ctrl(s, 1);   // Bật Auto Exposure (AEC)
    //     s->set_gain_ctrl(s, 1);       // Bật Auto Gain (AGC)
    //     s->set_whitebal(s, 1);             // Bật Auto White Balance (cân bằng trắng tự động để màu sắc chuẩn xác)
    //     s->set_aec2(s, 1);            // Bật thuật toán AEC DSP giúp chống lóa sáng mạnh
    //     s->set_brightness(s, 1);      // Tăng nhẹ độ sáng (tuỳ chọn)
    //     s->set_contrast(s, 1);        // Tăng nhẹ tương phản để viền chữ nổi bật hơn cho AI
    // }

    return true;
}

void setup() {
    pinMode(SPK_PWM_PIN, OUTPUT);
    digitalWrite(SPK_PWM_PIN, LOW);

    Serial.begin(SERIAL_BAUD);
    delay(500);

    if (!psramFound()) {
        Serial.println("[CAM] Khong tim thay PSRAM!");
        return;
    }

    if (!initCamera()) {
        Serial.println("[CAM] Init Fail!");
        return;
    }

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
    }

    startCameraServer();
    Serial.print("Ready: http://");
    Serial.println(WiFi.localIP());
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

#endif