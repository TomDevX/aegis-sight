#ifndef MOTION_GATE_H
#define MOTION_GATE_H

#include <Arduino.h>
#include "config.h"

// Motion Gate - MPU6050 accelerometer movement detection.
// Ultrasonic beeps only while the user is moving.
// Detection: stddev of SV (accel vector magnitude in g) over a
// sliding window; hysteresis timers prevent gate flicker.

// Feed SV sample (in g) and optional gyro rate (in deg/s) per call
void    motion_gate_update(float svG, float gyroDegS = 0.0f);
// Current gate state: true = user is moving (US beeps allowed).
bool    motion_gate_enabled(void);
// Reset buffers + state (e.g. on task start).
void    motion_gate_reset(void);

#endif
