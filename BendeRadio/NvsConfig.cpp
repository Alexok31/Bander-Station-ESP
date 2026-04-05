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
