#include <Preferences.h>
#include "secrets.h"
#include "config.h"  // để dùng WIFI_SSID, GEMINI_API_KEY hardcode

static Preferences prefs;
static bool opened = false;

void secrets_begin(void) {
    if (!opened) {
        opened = prefs.begin(SECRETS_NS, false);
    }
}

bool secrets_has(const char *key) {
    secrets_begin();
    if (!opened) return false;
    // Coi như có key nếu NVS có HOẶC config.h có giá trị default
    bool inNvs = prefs.isKey(key);
    bool inConfig = false;
    if (strcmp(key, SK_WIFI_SSID) == 0) inConfig = (strlen(WIFI_SSID) > 0);
    else if (strcmp(key, SK_WIFI_PASS) == 0) inConfig = (strlen(WIFI_PASS) > 0);
    else if (strcmp(key, SK_GEMINI_KEY) == 0) inConfig = (strlen(GEMINI_API_KEY) > 0);
    else if (strcmp(key, SK_WIFI_SSID2) == 0) inConfig = (strlen(WIFI_SSID2) > 0);
    else if (strcmp(key, SK_WIFI_PASS2) == 0) inConfig = (strlen(WIFI_PASS2) > 0);
    else if (strcmp(key, SK_WIFI_SSID3) == 0) inConfig = (strlen(WIFI_SSID3) > 0);
    else if (strcmp(key, SK_WIFI_PASS3) == 0) inConfig = (strlen(WIFI_PASS3) > 0);
    return inNvs || inConfig;
}

String secrets_get(const char *key, const char *def) {
    secrets_begin();
    if (!opened) return String(def);
    // Ưu tiên config.h (hardcode) hơn NVS - để code định nghĩa mạng luôn ưu tiên
    String val = "";
    if (strcmp(key, SK_WIFI_SSID) == 0) val = String(WIFI_SSID);
    else if (strcmp(key, SK_WIFI_PASS) == 0) val = String(WIFI_PASS);
    else if (strcmp(key, SK_GEMINI_KEY) == 0) val = String(GEMINI_API_KEY);
    else if (strcmp(key, SK_WIFI_SSID2) == 0) val = String(WIFI_SSID2);
    else if (strcmp(key, SK_WIFI_PASS2) == 0) val = String(WIFI_PASS2);
    else if (strcmp(key, SK_WIFI_SSID3) == 0) val = String(WIFI_SSID3);
    else if (strcmp(key, SK_WIFI_PASS3) == 0) val = String(WIFI_PASS3);
    
    if (val.length() > 0) return val;
    
    // Fallback NVS
    val = prefs.getString(key, "");
    if (val.length() > 0) return val;
    return String(def);
}

bool secrets_set(const char *key, const String &val) {
    secrets_begin();
    if (!opened) return false;
    return prefs.putString(key, val) > 0;
}

void secrets_remove(const char *key) {
    secrets_begin();
    if (opened) prefs.remove(key);
}

void secrets_clear(void) {
    secrets_begin();
    if (opened) prefs.clear();
}

bool secrets_has_all(void) {
    if (!opened) secrets_begin();
    if (!opened) return false;
    bool hasSSID = secrets_has(SK_WIFI_SSID);
    bool hasKey  = secrets_has(SK_GEMINI_KEY);
    return hasSSID && hasKey;
}

void secrets_end(void) {
    if (opened) {
        prefs.end();
        opened = false;
    }
}
