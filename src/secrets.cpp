#include <Preferences.h>
#include "secrets.h"

static Preferences prefs;
static bool opened = false;

void secrets_begin(void) {
    if (!opened) {
        opened = prefs.begin(SECRETS_NS, false);
    }
}

bool secrets_has(const char *key) {
    secrets_begin();
    return opened && prefs.isKey(key);
}

String secrets_get(const char *key, const char *def) {
    secrets_begin();
    if (!opened) return String(def);
    return prefs.getString(key, def);
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
