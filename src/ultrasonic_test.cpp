#ifdef ENABLE_ULTRASONIC_TEST

#include <Arduino.h>
#include "config.h"

// ============================================================
// Ultrasonic HC-SR04 Test - Đo khoảng cách liên tục
// Gửi trigger pulse 10us, đo echo, tính khoảng cách cm
// Hiển thị khoảng cách + thời gian bay âm qua Serial
// Kích hoạt: uncomment #define ENABLE_ULTRASONIC_TEST trong config.h
//             và comment TẤT CẢ main flags để tránh conflict
// ============================================================

#define US_TIMEOUT_US     30000
#define US_REPORT_MS      200
#define US_SPEED_CM_US    0.0343f

static volatile uint32_t echoStartUs = 0;
static volatile uint32_t echoDurationUs = 0;
static volatile bool echoReady = false;

static void IRAM_ATTR echo_isr(void) {
    if (digitalRead(ULTRASONIC_ECHO)) {
        echoStartUs = micros();
    } else {
        echoDurationUs = micros() - echoStartUs;
        echoReady = true;
    }
}

static bool initUltrasonic(void) {
    pinMode(ULTRASONIC_TRIG, OUTPUT);
    pinMode(ULTRASONIC_ECHO, INPUT);
    digitalWrite(ULTRASONIC_TRIG, LOW);

    attachInterrupt(digitalPinToInterrupt(ULTRASONIC_ECHO), echo_isr, CHANGE);

    return true;
}

static void measureDistance(void) {
    echoReady = false;
    echoDurationUs = 0;

    digitalWrite(ULTRASONIC_TRIG, LOW);
    vTaskDelay(pdMS_TO_TICKS(2));
    digitalWrite(ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(ULTRASONIC_TRIG, LOW);

    unsigned long timeout = micros() + US_TIMEOUT_US;
    while (!echoReady && micros() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!echoReady || echoDurationUs == 0) {
        Serial.println("[US_TEST] No echo / timeout (no object in range)");
        return;
    }

    float distanceCm = echoDurationUs * US_SPEED_CM_US / 2.0f;
    float distanceM  = distanceCm / 100.0f;

    Serial.printf("[US_TEST] Echo: %lu us | Distance: %.1f cm (%.2f m)\n",
                  echoDurationUs, distanceCm, distanceM);

    if (distanceCm <= DISTANCE_DANGER) {
        Serial.println("[US_TEST] >>> DANGER ZONE");
    } else if (distanceCm <= DISTANCE_WARNING) {
        Serial.println("[US_TEST] >>> WARNING ZONE");
    } else if (distanceCm <= DISTANCE_SLOW) {
        Serial.println("[US_TEST] >>> SLOW BEEP ZONE");
    } else {
        Serial.println("[US_TEST] >>> SAFE (silent)");
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n========================================");
    Serial.println("  ULTRASONIC TEST - HC-SR04");
    Serial.println("  Trig=GPIO8 | Echo=GPIO9");
    Serial.println("========================================\n");

    if (!initUltrasonic()) {
        Serial.println("[US_TEST] Init failed, halting.");
        return;
    }
    Serial.println("[US_TEST] HC-SR04 initialized OK\n");
}

void loop() {
    static unsigned long lastMeasure = 0;
    if (millis() - lastMeasure >= US_REPORT_MS) {
        lastMeasure = millis();
        measureDistance();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

#endif // ENABLE_ULTRASONIC_TEST
