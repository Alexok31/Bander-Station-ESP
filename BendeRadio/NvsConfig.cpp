#include "NvsConfig.h"

#include <Preferences.h>

#include "RadioConfig.h"

static constexpr char kNs[] = "bende";

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
    if (!has) {
        p.end();
        return false;
    }
    int v = p.getUChar("br_ovr", 8u);
    v = constrain(v, 0, 15);
    outValue = (uint8_t)v;
    p.remove("br_ovr");
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
