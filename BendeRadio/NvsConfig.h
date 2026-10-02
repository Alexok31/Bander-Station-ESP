#pragma once

#include <Arduino.h>
#include "CharacterSettings.h"

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

// Пустая строка = брать LOCAL_WS_HOST из secrets.h (дом). Иначе полный ws:// или wss:// URL.
void nvsLoadAiWsUrl(String& outUrl);
void nvsSaveAiWsUrl(const String& url);
bool nvsNormalizeAiWsUrl(String& url);

bool nvsLoadAiDebug();
void nvsSaveAiDebug(bool on);

bool nvsLoadWakeOnShake();
void nvsSaveWakeOnShake(bool on);
uint16_t nvsLoadShakeThreshold();
void nvsSaveShakeThreshold(uint16_t value);
uint8_t nvsLoadCalmMinutes();
void nvsSaveCalmMinutes(uint8_t value);
BenderCharacter::Settings nvsLoadCharacter();
bool nvsSaveCharacter(const BenderCharacter::Settings& settings);
