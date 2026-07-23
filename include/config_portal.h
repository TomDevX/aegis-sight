#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include <Arduino.h>

#define PORTAL_AP_SSID   "AegisSight-Setup"
#define PORTAL_AP_TIMEOUT_MS  300000  // 5 min before reboot if no config

void config_portal_start(void);

#endif
