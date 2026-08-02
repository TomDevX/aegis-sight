#include <Arduino.h>
#include "config.h"
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "tone_driver.h"
#include "motion_gate.h"

#ifdef ENABLE_MOTION_ULTRA_TEST

// ============================================================
// Motion-Gated Ultrasonic Test - MPU6050 + HC-SR04 combined
// Verifies: ultrasonic beeps ONLY while the user is moving.
//   - MPU6050 accelerometer -> motion gate (stddev of SV)
//   - HC-SR04 -> distance + zone
//   - Beeps play through I2S speaker when gate ON and in zone
// Kích hoạt: uncomment #define ENABLE_MOTION_ULTRA_TEST trong config.h
//             và comment TẤT CẢ main flags để tránh conflict
// ============================================================

#define US_TIMEOUT_US     30000
#define US_MEASURE_MS     100
#define US_SPEED_CM_US    0.0343f
#define MPU_READ_MS       20
#define REPORT_MS         200
#define BEEP_FREQ_HZ      2000
#define BEEP_DURATION_MS  50

#define G_TO_MS2          9.80665f

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

static Adafruit_MPU6050 mpu;

static bool initUltrasonic(void) {
    pinMode(ULTRASONIC_TRIG, OUTPUT);
    pinMode(ULTRASONIC_ECHO, INPUT);
    digitalWrite(ULTRASONIC_TRIG, LOW);
    attachInterrupt(digitalPinToInterrupt(ULTRASONIC_ECHO), echo_isr, CHANGE);
    Serial.println("[ULTRA_TEST] HC-SR04 initialized OK");
    return true;
}

static bool initMPU6050(void) {
    Wire.begin(MPU_SDA, MPU_SCL);
    if (!mpu.begin()) {
        Serial.println("[ULTRA_TEST] MPU6050 not found!");
        return false;
    }
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
    mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("[ULTRA_TEST] MPU6050 initialized OK");
    return true;
}

static float measureDistanceCm(void) {
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
        return -1.0f;
    }
    return echoDurationUs * US_SPEED_CM_US / 2.0f;
}

static void readMPU(void) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float sv = sqrtf(a.acceleration.x * a.acceleration.x +
                     a.acceleration.y * a.acceleration.y +
                     a.acceleration.z * a.acceleration.z);
    motion_gate_update(sv / G_TO_MS2);
}

static bool mpuOk = false;

static void reportZone(float dist) {
    if (dist <= DISTANCE_DANGER) {
        Serial.println("[ULTRA_TEST] >>> DANGER ZONE");
    } else if (dist <= DISTANCE_WARNING) {
        Serial.println("[ULTRA_TEST] >>> WARNING ZONE");
    } else {
        Serial.println("[ULTRA_TEST] >>> SAFE (silent)");
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n========================================");
    Serial.println("  MOTION-GATED ULTRASONIC TEST");
    Serial.println("  MPU6050 + HC-SR04 | beep only while moving");
    Serial.println("  Trig=GPIO8 | Echo=GPIO9 | SDA=47 SCL=39");
    Serial.println("========================================\n");

    if (!initUltrasonic()) return;
    mpuOk = initMPU6050();
    if (!mpuOk) {
        Serial.println("[ULTRA_TEST] MPU6050 FAILED - gate stays OFF. Check wiring: SDA=47 SCL=39");
    }

    motion_gate_reset();
    if (tone_driver_init()) {
        tone_driver_start_task();
        Serial.println("[ULTRA_TEST] Tone driver (I2S speaker) OK");
    } else {
        Serial.println("[ULTRA_TEST] Tone driver init FAILED!");
    }

    Serial.printf("[ULTRA_TEST] Zones: DANGER<=%dcm | WARNING<=%dcm | SAFE\n\n",
                  DISTANCE_DANGER, DISTANCE_WARNING);
}

void loop() {
    static unsigned long lastMeasure = 0;
    static unsigned long lastMpu = 0;
    static unsigned long lastReport = 0;
    static unsigned long lastBeep = 0;
    static float lastDistance = -1;
    static bool lastGate = false;

    unsigned long now = millis();

    if (now - lastMpu >= MPU_READ_MS) {
        lastMpu = now;
        if (mpuOk) readMPU();
    }

    if (now - lastMeasure >= US_MEASURE_MS) {
        lastMeasure = now;
        float dist = measureDistanceCm();
        if (dist > 0) {
            lastDistance = dist;
        }
    }

    if (now - lastReport >= REPORT_MS) {
        lastReport = now;
        bool gate = motion_gate_enabled();
        if (lastDistance < 0) {
            Serial.printf("[ULTRA_TEST] Gate=%s | No echo (no object)\n",
                          gate ? "ON " : "OFF");
        } else {
            Serial.printf("[ULTRA_TEST] Gate=%s | Dist=%.1f cm\n",
                          gate ? "ON " : "OFF", lastDistance);
            reportZone(lastDistance);
        }
        if (gate != lastGate) {
            Serial.printf("[ULTRA_TEST] >>> GATE %s (user %s)\n",
                          gate ? "ON" : "OFF", gate ? "moving" : "still");
            lastGate = gate;
        }
    }

    // Beep only when gate ON and object in a warning zone
    if (lastDistance >= 0 && motion_gate_enabled()) {
        uint32_t intervalMs = 0;
        if (lastDistance <= DISTANCE_DANGER) {
            intervalMs = 80;
        } else if (lastDistance <= DISTANCE_WARNING) {
            intervalMs = 300;
        }
        if (intervalMs > 0 && (now - lastBeep >= intervalMs)) {
            lastBeep = now;
            tone_driver_play(BEEP_FREQ_HZ, BEEP_DURATION_MS, tone_driver_get_volume());
        }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}

#endif // ENABLE_MOTION_ULTRA_TEST
