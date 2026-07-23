#include <Arduino.h>
#include "config.h"
#include "tone_driver.h"

// ============================================================
// Ultrasonic Proximity Beep - HC-SR04 on Core 1
// Logic: closer object -> faster beep
//   D > 150cm: silence
//   100cm < D <= 150cm: slow beep (600ms interval)
//   50cm < D <= 100cm: medium beep (300ms interval)
//   D <= 50cm: continuous / rapid beep
// Beep tone generated via shared I2S Tone Driver
// ============================================================

#define US_MEASURE_MS     100
#define US_TIMEOUT_US     30000
#define US_SPEED_CM_US    0.0343f
#define US_TASK_STACK     3072
#define US_TASK_PRIO      2

#define BEEP_FREQ_HZ      2000
#define BEEP_DURATION_MS  50

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

static float measure_distance_cm(void) {
    echoReady = false;
    echoDurationUs = 0;

    digitalWrite(ULTRASONIC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(ULTRASONIC_TRIG, LOW);

    unsigned long timeout = micros() + US_TIMEOUT_US;
    while (!echoReady && micros() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!echoReady || echoDurationUs == 0) {
        return -1.0f;
    }

    return echoDurationUs * US_SPEED_CM_US / 2.0f;
}

static void ultrasonic_task(void *pvParameters) {
    pinMode(ULTRASONIC_TRIG, OUTPUT);
    pinMode(ULTRASONIC_ECHO, INPUT);
    digitalWrite(ULTRASONIC_TRIG, LOW);
    attachInterrupt(digitalPinToInterrupt(ULTRASONIC_ECHO), echo_isr, CHANGE);

    Serial.println("[US] Proximity beep task started");
    Serial.printf("[US] Zones: DANGER<=%dcm | WARNING<=%dcm | SLOW<=%dcm\n",
                  DISTANCE_DANGER, DISTANCE_WARNING, DISTANCE_SLOW);

    unsigned long lastMeasure = 0;
    unsigned long lastBeep = 0;
    float lastDistance = -1;

    while (true) {
        unsigned long now = millis();

        if (now - lastMeasure >= US_MEASURE_MS) {
            lastMeasure = now;
            float dist = measure_distance_cm();
            if (dist > 0) {
                lastDistance = dist;
            }
        }

        if (lastDistance < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t beepIntervalMs = 0;
        if (lastDistance <= DISTANCE_DANGER) {
            beepIntervalMs = 80;
        } else if (lastDistance <= DISTANCE_WARNING) {
            beepIntervalMs = 300;
        } else if (lastDistance <= DISTANCE_SLOW) {
            beepIntervalMs = 600;
        }

        if (beepIntervalMs > 0 && (now - lastBeep >= beepIntervalMs)) {
            lastBeep = now;
            tone_driver_play(BEEP_FREQ_HZ, BEEP_DURATION_MS, tone_driver_get_volume());
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ultrasonic_task_start(void) {
    xTaskCreatePinnedToCore(ultrasonic_task, "us_prox", US_TASK_STACK,
                            NULL, US_TASK_PRIO, NULL, 1);
}
