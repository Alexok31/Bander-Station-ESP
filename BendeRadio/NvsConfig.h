#pragma once

#include <Arduino.h>

struct WifiStored {
    String staSsid;
    String staPass;
    String apSsid;
    String apPass;
};

void nvsLoadWifi(WifiStored& w);
void nvsSaveWifi(const String& staSsid, const String& staPass, const String& apSsid, const String& apPass);
void nvsSeedDefaultsIfNeeded(WifiStored& w);
String nvsEffectiveApSsid(const WifiStored& w);
String nvsEffectiveApPass(const WifiStored& w);
