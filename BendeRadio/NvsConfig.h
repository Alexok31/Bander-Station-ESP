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

void nvsLoadCustomStations(String* outStations, uint8_t capacity, uint8_t& outCount);
void nvsSaveCustomStations(const String* stations, uint8_t count);
void nvsLoadMatrixBrightnessTrim(int8_t* outTrim, uint8_t count);
void nvsSaveMatrixBrightnessTrim(const int8_t* trim, uint8_t count);
bool nvsTakePendingBrightnessOverride(uint8_t& outValue);
void nvsSetPendingBrightnessOverride(uint8_t value);
