#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ESP_I2S.h>
#include <ArduinoJson.h>

// Streaming ASR via Deepgram Live (wss://api.deepgram.com/v1/listen).
class DeepgramASR {
public:
    typedef void (*TimeoutNoSpeechCallback)();

    explicit DeepgramASR(const char* apiKey);

    bool initINMP441Microphone(int bclk, int ws, int din);
    void setAudioParams(int sampleRate, int bitsPerSample, int channels);
    void setSilenceTimeoutMs(unsigned long ms);
    void setTimeoutNoSpeechCallback(TimeoutNoSpeechCallback cb);

    bool connectWebSocket();
    void disconnectWebSocket();
    bool isWebSocketConnected();

    bool startRecording();
    void stopRecording();
    bool isRecording() const;

    void loop();

    bool hasNewResult() const;
    String getRecognizedText() const;
    void clearResult();

private:
    String generateWebSocketKey();
    void sendWebSocketFrame(const uint8_t* data, size_t len, uint8_t opcode);
    void sendTextFrame(const char* json);
    void sendBinaryAudio(const uint8_t* data, size_t len);
    void sendKeepAlive();
    void sendCloseStream();
    void handleWebSocketData();
    void parseServerJson(const char* json, size_t len);
    void processMicSend();
    void checkNoSpeechTimeout();

    const char* _apiKey;
    const char* _wsHost = "api.deepgram.com";
    const int _wsPort = 443;

    int _sampleRate = 16000;
    int _bitsPerSample = 16;
    int _channels = 1;
    unsigned long _silenceTimeoutMs = 12000;

    I2SClass _I2S;
    WiFiClientSecure _client;
    bool _use32bitSlot = true;
    // INMP441 в 32-bit слоте: сдвиг как в RadioConfig (14).
    static constexpr uint8_t kPcmShiftRight = 14;

    unsigned long _lastDebugMs = 0;
    uint32_t _dbgChunks = 0;
    uint64_t _dbgEnergy = 0;
    uint32_t _dbgSamples = 0;

    bool _wsConnected = false;
    bool _isRecording = false;
    bool _needReconnect = false;
    bool _hasNewResult = false;
    bool _hasSpeech = false;

    String _partialText;
    String _finalText;
    String _recognizedText;

    unsigned long _recordingStartMs = 0;
    unsigned long _lastKeepAliveMs = 0;
    unsigned long _lastSpeechMs = 0;

    static const int kSendSamples = 1600;  // 100 ms @ 16 kHz
    int16_t _sendBuf[kSendSamples];
    int _sendPos = 0;

    TimeoutNoSpeechCallback _timeoutCb = nullptr;
};
