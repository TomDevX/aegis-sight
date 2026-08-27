#ifndef MPU_MANAGER_H
#define MPU_MANAGER_H

#include <Arduino.h>

bool mpu_manager_init(void);
bool mpu_manager_read_sv_g(float *out_svG);
bool mpu_manager_read_accel_g(float *out_ax, float *out_ay, float *out_az, float *out_svG);
float mpu_manager_get_last_sv_g(void);
float mpu_manager_get_ema_sv_g(void);
unsigned long mpu_manager_get_last_read_ms(void);
int mpu_manager_get_error_count(void);
bool mpu_manager_is_healthy(void);

#endif