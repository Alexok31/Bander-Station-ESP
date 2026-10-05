#include "NvsConfig.h"

#include <Preferences.h>

#include "RadioConfig.h"
#include "WakeVoiceSettings.h"

static constexpr char kNs[] = "bende";

bool nvsLoadWakeVoiceEnabled() {
    Preferences p;
    if (!p.begin(kNs, true)) return false;
    const bool enabled = p.getBool("wake_voice", false);
    p.end();
    return enabled;
}

bool nvsSaveWakeVoiceEnabled(bool enabled) {
    Preferences p;
    if (!p.begin(kNs, false)) return false;
    const bool ok = p.getBool("wake_voice", false) == enabled ||
                    p.putBool("wake_voice", enabled) == 1;
    p.end();
    return ok;
}

uint8_t nvsLoadWakeFollowupSeconds() {
    Preferences p;
    if(!p.begin(kNs,true)) return WakeVoiceSettings::defaultFollowupSeconds;
    const auto seconds=p.getUChar("wake_follow",WakeVoiceSettings::defaultFollowupSeconds);
    p.end();
    return WakeVoiceSettings::validFollowup(seconds)?seconds:WakeVoiceSettings::defaultFollowupSeconds;
}
bool nvsSaveWakeFollowupSeconds(uint8_t seconds) {
    if(!WakeVoiceSettings::validFollowup(seconds)) return false;
    if(seconds==nvsLoadWakeFollowupSeconds()) return true;
    Preferences p;
    if(!p.begin(kNs,false)) return false;
    const bool ok=p.putUChar("wake_follow",seconds)==1;
    p.end();return ok;
}

void nvsLoadWifi(WifiStored& w) {
    Preferences p;
    if (!p.begin(kNs, true)) {
        w.staSsid = "";
        w.staPass = "";
        w.apSsid = "";
        w.apPass = "";
        return;
    }
    w.staSsid = p.getString("sta_ssid", "");
    w.staPass = p.getString("sta_pass", "");
    w.apSsid = p.getString("ap_ssid", "");
    w.apPass = p.getString("ap_pass", "");
    p.end();
}

void nvsSaveWifi(const String& staSsid, const String& staPass, const String& apSsid, const String& apPass) {
    Preferences p;
    if (!p.begin(kNs, false)) {
        return;
    }
    p.putString("sta_ssid", staSsid);
    p.putString("sta_pass", staPass);
    p.putString("ap_ssid", apSsid);
    p.putString("ap_pass", apPass);
    p.end();
}

void nvsSeedDefaultsIfNeeded(WifiStored& w) {
    if (w.staSsid.length() || w.staPass.length() || w.apSsid.length() || w.apPass.length()) {
        return;
    }
    nvsSaveWifi(String(RadioConfig::wifiSsid), String(RadioConfig::wifiPass), String(RadioConfig::apSsid),
                String(RadioConfig::apPassDefault));
    nvsLoadWifi(w);
}

String nvsEffectiveApSsid(const WifiStored& w) {
    if (w.apSsid.length() > 0) {
        return w.apSsid;
    }
    return String(RadioConfig::apSsid);
}

String nvsEffectiveApPass(const WifiStored& w) {
    if (w.apPass.length() > 0) {
        return w.apPass;
    }
    return String(RadioConfig::apPassDefault);
}

void nvsLoadCustomStations(String* outStations, uint8_t capacity, uint8_t& outCount) {
    outCount = 0;
    if (outStations == nullptr || capacity == 0) {
        return;
    }
    Preferences p;
    if (!p.begin(kNs, true)) {
        return;
    }
    uint8_t cnt = p.getUChar("st_cnt", 0u);
    if (cnt > capacity) {
        cnt = capacity;
    }
    for (uint8_t i = 0; i < cnt; i++) {
        char key[8];
        snprintf(key, sizeof(key), "st%u", (unsigned)i);
        String v = p.getString(key, "");
        v.trim();
        if (v.length() == 0) {
            continue;
        }
        outStations[outCount++] = v;
    }
    p.end();
}

void nvsSaveCustomStations(const String* stations, uint8_t count) {
    if (stations == nullptr) {
        return;
    }
    if (count > RadioConfig::customStationMaxCount) {
        count = RadioConfig::customStationMaxCount;
    }
    Preferences p;
    if (!p.begin(kNs, false)) {
        return;
    }
    p.putUChar("st_cnt", count);
    for (uint8_t i = 0; i < RadioConfig::customStationMaxCount; i++) {
        char key[8];
        snprintf(key, sizeof(key), "st%u", (unsigned)i);
        if (i < count) {
            p.putString(key, stations[i]);
        } else {
            p.remove(key);
        }
    }
    p.end();
}

void nvsLoadMatrixBrightnessTrim(int8_t* outTrim, uint8_t count) {
    if (outTrim == nullptr || count == 0) {
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        outTrim[i] = 0;
    }
    Preferences p;
    if (!p.begin(kNs, true)) {
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "mbr%u", (unsigned)i);
        const int v = p.getChar(key, 0);
        outTrim[i] = (int8_t)constrain(v, (int)RadioConfig::matrixBrightnessTrimMin,
                                        (int)RadioConfig::matrixBrightnessTrimMax);
    }
    p.end();
}

void nvsSaveMatrixBrightnessTrim(const int8_t* trim, uint8_t count) {
    if (trim == nullptr || count == 0) {
        return;
    }
    Preferences p;
    if (!p.begin(kNs, false)) {
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "mbr%u", (unsigned)i);
        const int v = constrain((int)trim[i], (int)RadioConfig::matrixBrightnessTrimMin,
                                (int)RadioConfig::matrixBrightnessTrimMax);
        p.putChar(key, (int8_t)v);
    }
    p.end();
}

bool nvsTakePendingBrightnessOverride(uint8_t& outValue) {
    Preferences p;
    if (!p.begin(kNs, false)) {
        return false;
    }
    const bool has = p.isKey("br_ovr");
    // Apply the requested level once, even if the previous playing cap saved 4
    // to EEPROM. Subsequent boots keep the user's encoder adjustment.
    const bool migrate = !p.getBool("br_uniform8", false);
    if (!has && !migrate) {
        p.end();
        return false;
    }
    int v = has ? p.getUChar("br_ovr", RadioConfig::matrixBrightnessDefaultBase)
                : RadioConfig::matrixBrightnessDefaultBase;
    v = constrain(v, 0, 15);
    outValue = (uint8_t)v;
    if (has) p.remove("br_ovr");
    if (migrate) p.putBool("br_uniform8", true);
    p.end();
    return true;
}

void nvsSetPendingBrightnessOverride(uint8_t value) {
    Preferences p;
    if (!p.begin(kNs, false)) {
        return;
    }
    p.putUChar("br_ovr", (uint8_t)constrain((int)value, 0, 15));
    p.end();
}

bool nvsNormalizeAiWsUrl(String& url) {
    url.trim();
    url.replace(" ", "");
    if (url.length() == 0) {
        return true;
    }
    if (url.startsWith("https://")) {
        url = String("wss://") + url.substring(8);
    } else if (url.startsWith("http://")) {
        url = String("ws://") + url.substring(7);
    }
    if (!url.startsWith("ws://") && !url.startsWith("wss://")) {
        return false;
    }
    const int schemeEnd = url.startsWith("wss://") ? 6 : 5;
    if (url.indexOf('/', schemeEnd) < 0) {
        if (url.endsWith("/")) {
            url.remove(url.length() - 1);
        }
        url += F("/v1/realtime");
    }
    return url.length() > 0 && url.length() <= 160;
}

void nvsLoadAiWsUrl(String& outUrl) {
    outUrl = "";
    Preferences p;
    if (!p.begin(kNs, true)) {
        return;
    }
    String v = p.getString("ai_wsurl", "");
    p.end();
    if (!nvsNormalizeAiWsUrl(v)) {
        return;
    }
    outUrl = v;
}

void nvsSaveAiWsUrl(const String& url) {
    String v = url;
    if (!nvsNormalizeAiWsUrl(v)) {
        v = "";
    }
    Preferences p;
    if (!p.begin(kNs, false)) {
        return;
    }
    if (v.length() == 0) {
        p.remove("ai_wsurl");
    } else {
        p.putString("ai_wsurl", v);
    }
    p.end();
}

bool nvsLoadAiDebug() {
    Preferences p;
    if (!p.begin(kNs, true)) {
        return false;
    }
    const bool on = p.getBool("ai_debug", false);
    p.end();
    return on;
}

void nvsSaveAiDebug(bool on) {
    Preferences p;
    if (!p.begin(kNs, false)) {
        return;
    }
    p.putBool("ai_debug", on);
    p.end();
}

bool nvsLoadWakeOnShake() {
    Preferences p;
    if (!p.begin(kNs, true)) return true;
    const bool on = p.getBool("shake_wake", true);
    p.end();
    return on;
}

void nvsSaveWakeOnShake(bool on) {
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.putBool("shake_wake", on);
    p.end();
}

uint16_t nvsLoadShakeThreshold() {
    Preferences p;
    if (!p.begin(kNs, true)) return BenderBehavior::shakeDefault;
    const uint16_t value = p.getUShort("shake_delta", BenderBehavior::shakeDefault);
    p.end();
    return BenderBehavior::validShake(value) ? value : BenderBehavior::shakeDefault;
}

void nvsSaveShakeThreshold(uint16_t value) {
    if (!BenderBehavior::validShake(value)) return;
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.putUShort("shake_delta", value);
    p.end();
}

uint8_t nvsLoadCalmMinutes() {
    Preferences p;
    if (!p.begin(kNs, true)) return BenderBehavior::calmDefaultMinutes;
    const uint8_t value = p.getUChar("calm_min", BenderBehavior::calmDefaultMinutes);
    p.end();
    return BenderBehavior::validCalm(value) ? value : BenderBehavior::calmDefaultMinutes;
}

void nvsSaveCalmMinutes(uint8_t value) {
    if (!BenderBehavior::validCalm(value)) return;
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.putUChar("calm_min", value);
    p.end();
}

BenderCharacter::Settings nvsLoadCharacter() {
    BenderCharacter::Settings result;
    Preferences p;
    if (!p.begin(kNs, true)) return result;
    const char* key = p.isKey("character_v2") ? "character_v2" : "character_v1";
    const size_t size = p.getBytesLength(key);
    uint8_t stored[BenderCharacter::count] = {};
    const bool ok = (size == 5 || size == sizeof(stored)) && p.getBytes(key, stored, size) == size;
    p.end();
    return ok ? BenderCharacter::fromStored(stored, size) : result;
}

bool nvsSaveCharacter(const BenderCharacter::Settings& settings) {
    if (!BenderCharacter::valid(settings)) return false;
    Preferences p;
    if (!p.begin(kNs, false)) return false;
    const bool ok = p.putBytes("character_v2", settings.values, sizeof(settings.values)) == sizeof(settings.values);
    p.end();
    return ok;
}
