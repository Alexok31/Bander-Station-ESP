/*
 * Bender V3: Deepgram streaming ASR + DAZI OpenAI GPT/TTS
 *
 * Установка:
 * 1. DAZI-AI library (для ArduinoGPTChat + Audio.h)
 * 2. ArduinoJson 7.x, Seeed_Arduino_mbedtls
 * 3. Board: ESP32-S3, OPI PSRAM, Huge APP / 8M with spiffs
 *
 * Ключ: https://console.deepgram.com → API Keys → Create
 * Вставь в dazi_secrets.h → DEEPGRAM_API_KEY
 *
 * Кнопка: клик энкодера (GPIO 6) = старт/стоп непрерывного диалога.
 */

#include <WiFi.h>
#include <ArduinoGPTChat.h>
#include "Audio.h"
#include "DaziBenderPins.h"
#include "DeepgramASR.h"

#if __has_include("dazi_secrets.h")
#include "dazi_secrets.h"
#else
#error "Create dazi_secrets.h from dazi_secrets.example.h next to this sketch"
#endif

#ifndef DEEPGRAM_API_KEY
#error "Define DEEPGRAM_API_KEY in dazi_secrets.h"
#endif

#define ENABLE_CONVERSATION_MEMORY 1

const char* systemPrompt =
    "Ты Bender из Футурамы, в корпусе интернет-колонки. "
    "Отвечай коротко по-русски, грубовато и с юмором. "
    "Не управляй радио. Не говори, что ты ChatGPT.";

const char* tts_voice = "onyx";

Audio audio;
DeepgramASR asrChat(DEEPGRAM_API_KEY);
ArduinoGPTChat gptChat(DAZI_OPENAI_API_KEY, DAZI_OPENAI_API_BASE);

enum ConversationState {
    STATE_IDLE,
    STATE_LISTENING,
    STATE_PROCESSING_LLM,
    STATE_PLAYING_TTS,
    STATE_WAIT_TTS_COMPLETE
};

ConversationState currentState = STATE_IDLE;
bool continuousMode = false;
bool buttonPressed = false;
bool wasButtonPressed = false;
unsigned long lastButtonMs = 0;
unsigned long ttsStartTime = 0;
unsigned long ttsCheckTime = 0;

void stopContinuousMode();

void startContinuousMode() {
    continuousMode = true;
    currentState = STATE_LISTENING;
    Serial.println(F("\n[AI] Continuous mode ON (click again to stop)"));
    if (asrChat.startRecording()) {
        Serial.println(F("[ASR] Listening..."));
    } else {
        Serial.println(F("[ASR] start failed"));
        continuousMode = false;
        currentState = STATE_IDLE;
    }
}

void stopContinuousMode() {
    continuousMode = false;
    if (asrChat.isRecording()) {
        asrChat.stopRecording();
    }
    currentState = STATE_IDLE;
    Serial.println(F("\n[AI] Continuous mode OFF"));
}

void handleASRResult() {
    String transcribedText = asrChat.getRecognizedText();
    asrChat.clearResult();

    if (transcribedText.length() == 0) {
        Serial.println(F("[ASR] (empty)"));
        if (continuousMode) {
            delay(400);
            currentState = STATE_LISTENING;
            asrChat.startRecording();
        } else {
            currentState = STATE_IDLE;
        }
        return;
    }

    Serial.printf("[ASR] %s\n", transcribedText.c_str());
    currentState = STATE_PROCESSING_LLM;

    String response = gptChat.sendMessage(transcribedText);
    if (response.length() == 0) {
        Serial.println(F("[LLM] empty response"));
        if (continuousMode) {
            delay(400);
            currentState = STATE_LISTENING;
            asrChat.startRecording();
        } else {
            currentState = STATE_IDLE;
        }
        return;
    }

    Serial.printf("[LLM] %s\n", response.c_str());
    currentState = STATE_PLAYING_TTS;

    if (audio.openai_speech(String(DAZI_OPENAI_API_KEY), "gpt-4o-mini-tts", response,
                            tts_voice, "mp3", "1.0")) {
        currentState = STATE_WAIT_TTS_COMPLETE;
        ttsStartTime = millis();
        ttsCheckTime = millis();
    } else {
        Serial.println(F("[TTS] failed"));
        if (continuousMode) {
            delay(400);
            currentState = STATE_LISTENING;
            asrChat.startRecording();
        } else {
            currentState = STATE_IDLE;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\nBender V3: Deepgram ASR + OpenAI GPT/TTS"));

    pinMode(DAZI_TALK_BUTTON_PIN, INPUT_PULLUP);

    if (DEEPGRAM_API_KEY[0] == '\0' || DAZI_OPENAI_API_KEY[0] == '\0') {
        Serial.println(F("Fill DEEPGRAM_API_KEY and DAZI_OPENAI_API_KEY in dazi_secrets.h"));
        while (true) {
            delay(1000);
        }
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(DAZI_WIFI_SSID, DAZI_WIFI_PASS);
    Serial.print(F("WiFi"));
    for (int i = 0; i < 25 && WiFi.status() != WL_CONNECTED; i++) {
        Serial.print('.');
        delay(500);
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("WiFi failed"));
        return;
    }
    Serial.println(WiFi.localIP());

    audio.setPinout(DAZI_I2S_BCLK, DAZI_I2S_LRC, DAZI_I2S_DOUT);
    audio.setVolume(18);

    gptChat.setSystemPrompt(systemPrompt);
#if ENABLE_CONVERSATION_MEMORY
    gptChat.enableMemory(true);
#else
    gptChat.enableMemory(false);
#endif

    asrChat.setAudioParams(DAZI_SAMPLE_RATE, 16, 1);
    asrChat.setSilenceTimeoutMs(20000);
    asrChat.setTimeoutNoSpeechCallback([]() {
        Serial.println(F("[ASR] no speech — keep listening"));
        if (continuousMode) {
            delay(300);
            asrChat.startRecording();
        }
    });

    if (!asrChat.initINMP441Microphone(DAZI_MIC_BCLK, DAZI_MIC_WS, DAZI_MIC_DIN)) {
        Serial.println(F("INMP441 init failed"));
        return;
    }

    if (!asrChat.connectWebSocket()) {
        Serial.println(F("Deepgram WebSocket failed — check DEEPGRAM_API_KEY"));
        return;
    }

    Serial.println(F("Ready: 1 click encoder = start/stop AI talk"));
}

void loop() {
    audio.loop();
    asrChat.loop();

    buttonPressed = (digitalRead(DAZI_TALK_BUTTON_PIN) == LOW);
    if (buttonPressed && !wasButtonPressed) {
        wasButtonPressed = true;
        if (millis() - lastButtonMs > 400) {
            lastButtonMs = millis();
            if (!continuousMode && currentState == STATE_IDLE) {
                startContinuousMode();
            } else if (continuousMode) {
                stopContinuousMode();
            }
        }
    } else if (!buttonPressed && wasButtonPressed) {
        wasButtonPressed = false;
    }

    switch (currentState) {
        case STATE_IDLE:
            break;
        case STATE_LISTENING:
            if (asrChat.hasNewResult()) {
                handleASRResult();
            }
            break;
        case STATE_WAIT_TTS_COMPLETE:
            if (millis() - ttsCheckTime > 100) {
                ttsCheckTime = millis();
                if (!audio.isRunning()) {
                    if (continuousMode) {
                        delay(400);
                        currentState = STATE_LISTENING;
                        asrChat.startRecording();
                        Serial.println(F("[ASR] Listening..."));
                    } else {
                        currentState = STATE_IDLE;
                    }
                } else if (millis() - ttsStartTime > 60000) {
                    Serial.println(F("[TTS] timeout"));
                    if (continuousMode) {
                        currentState = STATE_LISTENING;
                        asrChat.startRecording();
                    } else {
                        currentState = STATE_IDLE;
                    }
                }
            }
            break;
        default:
            break;
    }

    if (currentState == STATE_LISTENING) {
        yield();
    } else {
        delay(10);
    }
}
