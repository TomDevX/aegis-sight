#ifndef FALL_DETECTION_H
#define FALL_DETECTION_H

void fall_detection_task_start(void);   // Core 1: MPU6050 3-phase state machine
bool fall_alarm_busy(void);             // true khi đang cảnh báo / đếm ngược / hú SOS

#endif
