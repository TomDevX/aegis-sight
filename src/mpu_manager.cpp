#include "mpu_manager.h"
#include "config.h"
#include <Wire.h>

// MPU6050 Registers
#define MPU_REG_SMPLRT_DIV   0x19
#define MPU_REG_CONFIG       0x1A
#define MPU_REG_ACCEL_CONFIG 0x1C
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_WHO_AM_I     0x75

static SemaphoreHandle_t i2cMutex = NULL;
static bool mpuInitialized = false;
static float lastSvG = 1.0f;
static float lastAxG = 0.0f;
static float lastAyG = 0.0f;
static float lastAzG = 1.0f;
static unsigned long lastReadMs = 0;
static int i2cErrorCount = 0;

// EMA filter to smooth out transient spikes
static float emaSvG = 1.0f;
#define MPU_EMA_ALPHA  0.25f   // Low-pass: 25% new data, 75% history
static uint8_t activeMpuAddr = 0x68;

static bool write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return (Wire.endTransmission(true) == 0);
}

static void recover_i2c_bus(void) {
    Wire.end();
    pinMode(MPU_SDA, INPUT_PULLUP);
    pinMode(MPU_SCL, OUTPUT);
    digitalWrite(MPU_SCL, HIGH);
    delayMicroseconds(10);

    // Gửi 9 xung SCL clock để MPU nhả chân SDA đang bị kéo giữ
    for (int i = 0; i < 9; i++) {
        digitalWrite(MPU_SCL, LOW);
        delayMicroseconds(10);
        digitalWrite(MPU_SCL, HIGH);
        delayMicroseconds(10);
    }

    // Tạo I2C Stop Condition thủ công
    pinMode(MPU_SDA, OUTPUT);
    digitalWrite(MPU_SDA, LOW);
    delayMicroseconds(10);
    digitalWrite(MPU_SCL, HIGH);
    delayMicroseconds(10);
    digitalWrite(MPU_SDA, HIGH);
    delayMicroseconds(10);

    // Khởi động lại I2C Bus & Khởi tạo lại MPU6050
    pinMode(MPU_SDA, INPUT_PULLUP);
    pinMode(MPU_SCL, INPUT_PULLUP);
    Wire.begin(MPU_SDA, MPU_SCL);
    Wire.setClock(50000);
    Wire.setTimeOut(30);

    write_reg(activeMpuAddr, MPU_REG_PWR_MGMT_1, 0x00);
    delayMicroseconds(2000);
    write_reg(activeMpuAddr, MPU_REG_CONFIG, 0x03);
    write_reg(activeMpuAddr, MPU_REG_ACCEL_CONFIG, 0x10);
}

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

    pinMode(MPU_SDA, INPUT_PULLUP);
    pinMode(MPU_SCL, INPUT_PULLUP);
    Wire.begin(MPU_SDA, MPU_SCL);
    Wire.setClock(50000); // 50kHz - Tần số tối ưu cho dây nối dài chống suy hao
    Wire.setTimeOut(50);

    bool mpuFound = false;
    uint8_t foundAddr = 0x68;

    for (int attempt = 0; attempt < 8; attempt++) {
        Wire.beginTransmission(0x68);
        Wire.write(MPU_REG_WHO_AM_I);
        if (Wire.endTransmission(false) == 0) {
            if (Wire.requestFrom((uint8_t)0x68, (size_t)1, (bool)true) == 1) {
                mpuFound = true;
                foundAddr = 0x68;
                break;
            }
        }

        Wire.beginTransmission(0x69);
        Wire.write(MPU_REG_WHO_AM_I);
        if (Wire.endTransmission(false) == 0) {
            if (Wire.requestFrom((uint8_t)0x69, (size_t)1, (bool)true) == 1) {
                mpuFound = true;
                foundAddr = 0x69;
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }

    if (!mpuFound) {
        Serial.println("[MPU_MGR] WARN: MPU6050 not responding at 0x68 or 0x69. Check SDA=47, SCL=39, VCC/GND!");
        foundAddr = 0x68;
    }

    activeMpuAddr = foundAddr;

    // Đánh thức và cấu hình MPU6050
    write_reg(activeMpuAddr, MPU_REG_PWR_MGMT_1, 0x00);
    delay(10);
    write_reg(activeMpuAddr, MPU_REG_CONFIG, 0x03);       // DLPF 44Hz
    write_reg(activeMpuAddr, MPU_REG_ACCEL_CONFIG, 0x10); // +-8G range

    mpuInitialized = true;
    i2cErrorCount = 0;
    emaSvG = 1.0f;
    xSemaphoreGive(i2cMutex);
    Serial.printf("[MPU_MGR] MPU6050 I2C Ready at 0x%02X (+-8G, 44Hz DLPF, 50kHz)\n", foundAddr);
    return true;
}

bool mpu_manager_read_accel_g(float *out_ax, float *out_ay, float *out_az, float *out_svG) {
    if (!mpuInitialized) return false;

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    bool readSuccess = false;
    int16_t rawAx = 0, rawAy = 0, rawAz = 0;

    Wire.beginTransmission(activeMpuAddr);
    Wire.write(MPU_REG_ACCEL_XOUT_H);
    if (Wire.endTransmission(false) == 0) {
        if (Wire.requestFrom((uint8_t)activeMpuAddr, (size_t)6, (bool)true) == 6) {
            rawAx = (int16_t)((Wire.read() << 8) | Wire.read());
            rawAy = (int16_t)((Wire.read() << 8) | Wire.read());
            rawAz = (int16_t)((Wire.read() << 8) | Wire.read());
            readSuccess = true;
        }
    }

    if (!readSuccess) {
        i2cErrorCount++;
        if (i2cErrorCount >= 3) {
            recover_i2c_bus();
            i2cErrorCount = 0;
        }
        xSemaphoreGive(i2cMutex);
        return false;
    }

    i2cErrorCount = 0;
    xSemaphoreGive(i2cMutex);

    // Chuyển đổi sang đơn vị G (+-8G range -> 4096.0f LSB/G)
    float ax = (float)rawAx / 4096.0f;
    float ay = (float)rawAy / 4096.0f;
    float az = (float)rawAz / 4096.0f;
    float sv = sqrtf(ax * ax + ay * ay + az * az);

    // Lọc dữ liệu lỗi bất thường
    if (isnan(sv) || sv > 16.0f || sv < 0.25f || (rawAx == 0 && rawAy == 0 && rawAz == 0)) {
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