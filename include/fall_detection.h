#ifndef FALL_DETECTION_H
#define FALL_DETECTION_H

void fall_detection_task_start(void);   // Core 1: MPU6050 3-phase state machine
void fall_detection_process_sample(float ax, float ay, float az, float svG); // Xử lý mẫu gia tốc tuần tự
void fall_detection_alarm_tick(void);   // Duy trì còi hú SOS
bool fall_alarm_busy(void);             // true khi đang cảnh báo / đếm ngược / hú SOS
bool fall_alarm_was_cancelled_recently(void); // true nếu alarm vừa bị tắt và đang trong cooldown
void fall_alarm_dismiss(void);          // Tắt cảnh báo té ngã SOS

#endif
