#include "mpu_manager.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

static Adafruit_MPU6050 mpu;
static SemaphoreHandle_t i2cMutex = NULL;
static bool mpuInitialized = false;
static float lastSvG = 1.0f;
static float lastAxG = 0.0f;
static float lastAyG = 0.0f;
static float lastAzG = 1.0f;
static unsigned long lastReadMs = 0;
static int i2cErrorCount = 0;

// EMA filter to smooth out transient spikes from I2C noise
static float emaSvG = 1.0f;
#define MPU_EMA_ALPHA  0.25f   // Low-pass: 25% new data, 75% history

bool mpu_manager_init(void) {
    if (mpuInitialized) return true;

    if (i2cMutex == NULL) {
        i2cMutex = xSemaphoreCreateMutex();
        if (i2cMutex == NULL) {
            Serial.println("[MPU_MGR] Failed to create mutex");
            return false;
        }
    }

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return false;
    }

    if (mpuInitialized) {
        xSemaphoreGive(i2cMutex);
        return true;
    }

    Wire.begin(MPU_SDA, MPU_SCL);
    Wire.setClock(50000); // 50kHz - chống nhiễu bus I2C cho dây nối mềm
    Wire.setTimeOut(40);  // 40ms timeout

    bool mpuFound = false;
    uint8_t foundAddr = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (mpu.begin(0x68, &Wire)) {
            mpuFound = true;
            foundAddr = 0x68;
            break;
        }
        if (mpu.begin(0x69, &Wire)) {
            mpuFound = true;
            foundAddr = 0x69;
            break;
        }
        Serial.printf("[MPU_MGR] Init attempt %d failed, retrying...\n", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!mpuFound) {
        Serial.println("[MPU_MGR] MPU6050 not found at 0x68 or 0x69!");
        xSemaphoreGive(i2cMutex);
        return false;
    }

    // 8G range: đủ để detect impact ngã người (~2-8G)
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    mpuInitialized = true;
    i2cErrorCount = 0;
    emaSvG = 1.0f;
    xSemaphoreGive(i2cMutex);
    Serial.printf("[MPU_MGR] MPU6050 initialized at 0x%02X (8G range, 21Hz DLPF, 50kHz I2C)\n", foundAddr);
    return true;
}

bool mpu_manager_read_accel_g(float *out_ax, float *out_ay, float *out_az, float *out_svG) {
    if (!mpuInitialized) return false;

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(25)) != pdTRUE) {
        return false;
    }

    sensors_event_t a, g, temp;
    bool success = mpu.getEvent(&a, &g, &temp);

    xSemaphoreGive(i2cMutex);

    if (!success) {
        i2cErrorCount++;
        return false;
    }

    // Convert m/s^2 to G
    float ax = a.acceleration.x / 9.80665f;
    float ay = a.acceleration.y / 9.80665f;
    float az = a.acceleration.z / 9.80665f;
    float sv = sqrtf(ax * ax + ay * ay + az * az);

    // Lọc lỗi I2C bus buffer rỗng:
    // Cảm biến cơ điện tử thật không bao giờ trả về đúng chính xác tuyệt đối ax=0, ay=0, az=0.
    // Nếu cả 3 trục đều = 0.000000 hoặc nhiệt độ = 0/NaN -> 100% là I2C bus error, bỏ qua ngay.
    if (isnan(sv) || sv > 16.0f || (fabsf(ax) < 0.0001f && fabsf(ay) < 0.0001f && fabsf(az) < 0.0001f)) {
        i2cErrorCount++;
        return false;
    }

    // Update EMA
    emaSvG = MPU_EMA_ALPHA * sv + (1.0f - MPU_EMA_ALPHA) * emaSvG;

    if (out_ax) *out_ax = ax;
    if (out_ay) *out_ay = ay;
    if (out_az) *out_az = az;
    if (out_svG) *out_svG = sv;

    lastAxG = ax;
    lastAyG = ay;
    lastAzG = az;
    lastSvG = sv;
    lastReadMs = millis();
    return true;
}

bool mpu_manager_read_sv_g(float *out_svG) {
    return mpu_manager_read_accel_g(NULL, NULL, NULL, out_svG);
}

float mpu_manager_get_last_sv_g(void) {
    return lastSvG;
}

float mpu_manager_get_ema_sv_g(void) {
    return emaSvG;
}

unsigned long mpu_manager_get_last_read_ms(void) {
    return lastReadMs;
}

int mpu_manager_get_error_count(void) {
    return i2cErrorCount;
}

bool mpu_manager_is_healthy(void) {
    return mpuInitialized && (millis() - lastReadMs < 1000) && i2cErrorCount < 100;
}