#ifndef SECRETS_H
#define SECRETS_H

#include <Arduino.h>

#define SECRETS_NS   "aegis"       // Preferences namespace

// NVS keys — credentials only come from portal or NVS, never compiled in
// NOTE: ESP-IDF NVS key names are limited to 15 characters max
#define SK_WIFI_SSID   "ssid"
#define SK_WIFI_PASS   "pass"
#define SK_WIFI_SSID2  "ssid2"
#define SK_WIFI_PASS2  "pass2"
#define SK_WIFI_SSID3  "ssid3"
#define SK_WIFI_PASS3  "pass3"
#define SK_GEMINI_KEY  "api_key"
#define SK_LAST_SSID   "last_ssid"

void     secrets_begin(void);
bool     secrets_has(const char *key);
String   secrets_get(const char *key, const char *def = "");
bool     secrets_set(const char *key, const String &val);
void     secrets_remove(const char *key);
void     secrets_clear(void);
bool     secrets_has_all(void);
void     secrets_end(void);

#endif
