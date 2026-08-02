#ifndef MOTION_GATE_H
#define MOTION_GATE_H

#include <Arduino.h>
#include "config.h"

// Motion Gate - MPU6050 accelerometer movement detection.
// Ultrasonic beeps only while the user is moving.
// Detection: stddev of SV (accel vector magnitude in g) over a
// sliding window; hysteresis timers prevent gate flicker.

// Feed one SV sample (in g) per call, e.g. every 20ms from the MPU task.
void    motion_gate_update(float svG);
// Current gate state: true = user is moving (US beeps allowed).
bool    motion_gate_enabled(void);
// Reset buffers + state (e.g. on task start).
void    motion_gate_reset(void);

#endif
