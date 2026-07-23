#ifdef ENABLE_MPU6050_TEST

#include <Arduino.h>
#include "config.h"
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// ============================================================
// MPU6050 Test - Đọc accelerometer + gyroscope liên tục
// Tính SV (sqrt(ax^2+ay^2+az^2)), hiển thị gia tốc + gyro
// Kích hoạt: uncomment #define ENABLE_MPU6050_TEST trong config.h
//             và comment TẤT CẢ main flags để tránh conflict
// ============================================================

#define MPU_REPORT_MS     200
#define G_TO_MS2          9.80665f

static Adafruit_MPU6050 mpu;

static bool initMPU6050(void) {
    Wire.begin(MPU_SDA, MPU_SCL);

    if (!mpu.begin()) {
        Serial.println("[MPU_TEST] MPU6050 not found at I2C address!");
        Serial.println("[MPU_TEST] Check wiring: SDA=GPIO47 SCL=GPIO48");
        return false;
    }

    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
    mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    Serial.printf("[MPU_TEST] Chip ID: 0x%x\n", mpu.getChipID());

    return true;
}

static void readAndReport(void) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    float ax = a.acceleration.x;
    float ay = a.acceleration.y;
    float az = a.acceleration.z;
    float sv = sqrtf(ax * ax + ay * ay + az * az);

    Serial.printf("[MPU_TEST] Accel (m/s2): X=%.2f Y=%.2f Z=%.2f | SV=%.2f (%.2fg)\n",
                  ax, ay, az, sv, sv / G_TO_MS2);
    Serial.printf("[MPU_TEST] Gyro  (dps) : X=%.1f Y=%.1f Z=%.1f | Temp=%.1f C\n",
                  g.gyro.x * 57.2958f,
                  g.gyro.y * 57.2958f,
                  g.gyro.z * 57.2958f,
                  temp.temperature);

    if (sv / G_TO_MS2 < FALL_FREEFALL_G) {
        Serial.println("[MPU_TEST] >>> FREE-FALL DETECTED!");
    } else if (sv / G_TO_MS2 > FALL_IMPACT_G) {
        Serial.println("[MPU_TEST] >>> IMPACT DETECTED!");
    } else if (sv / G_TO_MS2 > 0.9f && sv / G_TO_MS2 < 1.1f) {
        Serial.println("[MPU_TEST] >>> Stationary (~1g)");
    }
    Serial.println();
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("\n========================================");
    Serial.println("  MPU6050 TEST - Accelerometer + Gyro");
    Serial.println("  I2C: SDA=GPIO47 SCL=GPIO48");
    Serial.println("========================================\n");

    if (!initMPU6050()) {
        Serial.println("[MPU_TEST] Init failed, halting.");
        return;
    }
    Serial.println("[MPU_TEST] MPU6050 initialized OK\n");
}

void loop() {
    static unsigned long lastReport = 0;
    if (millis() - lastReport >= MPU_REPORT_MS) {
        lastReport = millis();
        readAndReport();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

#endif // ENABLE_MPU6050_TEST
