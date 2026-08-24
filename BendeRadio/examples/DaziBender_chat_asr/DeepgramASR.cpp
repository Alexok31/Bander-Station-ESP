#include "DeepgramASR.h"
#include <mbedtls/base64.h>
#include <math.h>

DeepgramASR::DeepgramASR(const char* apiKey) : _apiKey(apiKey) {}

void DeepgramASR::setAudioParams(int sampleRate, int bitsPerSample, int channels) {
    _sampleRate = sampleRate;
    _bitsPerSample = bitsPerSample;
    _channels = channels;
}

void DeepgramASR::setSilenceTimeoutMs(unsigned long ms) {
    _silenceTimeoutMs = ms;
}

void DeepgramASR::setTimeoutNoSpeechCallback(TimeoutNoSpeechCallback cb) {
    _timeoutCb = cb;
}

bool DeepgramASR::initINMP441Microphone(int bclk, int ws, int din) {
    // Динамики (DAZI Audio) занимают I2S0 — микрофон только на I2S1.
    if (!_I2S.setPort(I2S_NUM_1)) {
        Serial.println(F("[DG] setPort(I2S1) failed"));
        return false;
    }
    _I2S.setPins(bclk, ws, -1, din);

    // INMP441 на ESP32-S3 обычно отдаёт данные в 32-bit слоте (см. RadioConfig micPcmShiftRight).
    if (!_I2S.begin(I2S_MODE_STD, _sampleRate, I2S_DATA_BIT_WIDTH_32BIT,
                    I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
        Serial.println(F("[DG] INMP441 32-bit init failed, trying 16-bit..."));
        if (!_I2S.begin(I2S_MODE_STD, _sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
            Serial.println(F("[DG] INMP441 I2S init failed"));
            return false;
        }
        _use32bitSlot = false;
    } else {
        _use32bitSlot = true;
    }

    delay(400);
    for (int i = 0; i < 2000; i++) {
        _I2S.read();
    }
    Serial.printf("[DG] INMP441 ready (I2S1, %s)\n", _use32bitSlot ? "32-bit" : "16-bit");
    return true;
}

String DeepgramASR::generateWebSocketKey() {
    uint8_t random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = (uint8_t)random(0, 256);
    }
    size_t out_len = 0;
    unsigned char out[32];
    mbedtls_base64_encode(out, sizeof(out), &out_len, random_bytes, 16);
    return String((char*)out);
}

bool DeepgramASR::connectWebSocket() {
    Serial.println(F("[DG] Connecting WebSocket..."));
    _client.setInsecure();
    if (!_client.connect(_wsHost, _wsPort)) {
        Serial.println(F("[DG] SSL connect failed"));
        return false;
    }
    _client.setNoDelay(true);

    // nova-2 + ru; endpointing ≈ end of utterance after silence
    String path =
        "/v1/listen?model=nova-2&language=ru&encoding=linear16"
        "&sample_rate=16000&channels=1&punctuate=true&smart_format=true"
        "&interim_results=true&endpointing=500&utterance_end_ms=1500&vad_events=true";

    String ws_key = generateWebSocketKey();
    String req;
    req.reserve(512);
    req += "GET ";
    req += path;
    req += " HTTP/1.1\r\n";
    req += "Host: ";
    req += _wsHost;
    req += "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: ";
    req += ws_key;
    req += "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "Authorization: Token ";
    req += _apiKey;
    req += "\r\n\r\n";

    _client.print(req);

    unsigned long t0 = millis();
    while (_client.connected() && !_client.available()) {
        if (millis() - t0 > 8000) {
            Serial.println(F("[DG] handshake timeout"));
            _client.stop();
            return false;
        }
        delay(10);
    }

    String response;
    while (_client.available()) {
        String line = _client.readStringUntil('\n');
        response += line;
        response += '\n';
        if (line == "\r" || line.length() == 0) {
            break;
        }
    }

    if (response.indexOf("101") >= 0) {
        Serial.println(F("[DG] WebSocket connected"));
        _wsConnected = true;
        _needReconnect = false;
        _lastKeepAliveMs = millis();
        return true;
    }

    Serial.println(F("[DG] handshake failed:"));
    Serial.println(response.substring(0, min((int)response.length(), 400)));
    _client.stop();
    _wsConnected = false;
    return false;
}

void DeepgramASR::disconnectWebSocket() {
    if (_wsConnected) {
        _client.stop();
        _wsConnected = false;
        Serial.println(F("[DG] WebSocket disconnected"));
    }
}

bool DeepgramASR::isWebSocketConnected() {
    return _wsConnected && _client.connected();
}

bool DeepgramASR::startRecording() {
    if (_needReconnect || !_wsConnected || !_client.connected()) {
        disconnectWebSocket();
        delay(80);
        if (!connectWebSocket()) {
            return false;
        }
    }

    if (_isRecording) {
        return true;
    }

    _isRecording = true;
    _hasNewResult = false;
    _hasSpeech = false;
    _partialText = "";
    _finalText = "";
    _recognizedText = "";
    _sendPos = 0;
    _recordingStartMs = millis();
    _lastSpeechMs = millis();
    _lastKeepAliveMs = millis();
    _lastDebugMs = millis();
    _dbgChunks = 0;
    _dbgEnergy = 0;
    _dbgSamples = 0;

    Serial.println(F("[DG] Listening..."));
    return true;
}

void DeepgramASR::stopRecording() {
    if (!_isRecording) {
        return;
    }
    if (_sendPos > 0) {
        sendBinaryAudio((uint8_t*)_sendBuf, (size_t)_sendPos * 2);
        _sendPos = 0;
    }
    sendCloseStream();
    _isRecording = false;
    _needReconnect = true;
    Serial.println(F("[DG] Recording stopped"));
}

bool DeepgramASR::isRecording() const {
    return _isRecording;
}

bool DeepgramASR::hasNewResult() const {
    return _hasNewResult;
}

String DeepgramASR::getRecognizedText() const {
    return _recognizedText;
}

void DeepgramASR::clearResult() {
    _hasNewResult = false;
    _recognizedText = "";
}

void DeepgramASR::sendWebSocketFrame(const uint8_t* data, size_t len, uint8_t opcode) {
    if (!_wsConnected || !_client.connected()) {
        return;
    }

    uint8_t header[14];
    int header_len = 2;
    header[0] = 0x80 | opcode;
    header[1] = 0x80;

    if (len < 126) {
        header[1] |= (uint8_t)len;
    } else if (len < 65536) {
        header[1] |= 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        return;
    }

    uint8_t mask[4] = {
        (uint8_t)random(0, 256), (uint8_t)random(0, 256),
        (uint8_t)random(0, 256), (uint8_t)random(0, 256)};
    memcpy(header + header_len, mask, 4);
    header_len += 4;

    _client.write(header, header_len);

    // mask payload
    const size_t chunk = 256;
    uint8_t tmp[chunk];
    size_t off = 0;
    while (off < len) {
        size_t n = min(chunk, len - off);
        for (size_t i = 0; i < n; i++) {
            tmp[i] = data[off + i] ^ mask[(off + i) % 4];
        }
        _client.write(tmp, n);
        off += n;
    }
}

void DeepgramASR::sendTextFrame(const char* json) {
    sendWebSocketFrame((const uint8_t*)json, strlen(json), 0x01);
}

void DeepgramASR::sendBinaryAudio(const uint8_t* data, size_t len) {
    sendWebSocketFrame(data, len, 0x02);
}

void DeepgramASR::sendKeepAlive() {
    sendTextFrame("{\"type\":\"KeepAlive\"}");
    _lastKeepAliveMs = millis();
}

void DeepgramASR::sendCloseStream() {
    sendTextFrame("{\"type\":\"CloseStream\"}");
}

void DeepgramASR::parseServerJson(const char* json, size_t len) {
    // Полный Results от Deepgram с words[] не влезает в RAM — берём только нужные поля.
    StaticJsonDocument<192> filter;
    filter["type"] = true;
    filter["is_final"] = true;
    filter["speech_final"] = true;
    filter["channel"]["alternatives"][0]["transcript"] = true;
    filter["message"] = true;

    StaticJsonDocument<768> doc;
    DeserializationError err =
        deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[DG] json err: %s (len=%u)\n", err.c_str(), (unsigned)len);
        return;
    }

    const char* type = doc["type"] | "";
    if (strcmp(type, "Results") == 0) {
        bool is_final = doc["is_final"] | false;
        bool speech_final = doc["speech_final"] | false;
        const char* transcript =
            doc["channel"]["alternatives"][0]["transcript"] | "";

        if (transcript[0] != '\0') {
            _hasSpeech = true;
            _lastSpeechMs = millis();
            if (is_final) {
                if (_finalText.length() > 0) {
                    _finalText += ' ';
                }
                _finalText += transcript;
                _partialText = "";
                Serial.printf("[DG] final: %s\n", transcript);
            } else {
                _partialText = transcript;
                Serial.printf("[DG] ~ %s\n", transcript);
            }
        }

        if (speech_final && _finalText.length() > 0 && _isRecording) {
            _recognizedText = _finalText;
            _hasNewResult = true;
            stopRecording();
        }
        return;
    }

    if (strcmp(type, "UtteranceEnd") == 0) {
        if (_isRecording && (_finalText.length() > 0 || _partialText.length() > 0)) {
            _recognizedText = _finalText.length() > 0 ? _finalText : _partialText;
            _hasNewResult = true;
            Serial.printf("[DG] utterance: %s\n", _recognizedText.c_str());
            stopRecording();
        }
        return;
    }

    if (strcmp(type, "SpeechStarted") == 0) {
        _hasSpeech = true;
        _lastSpeechMs = millis();
        Serial.println(F("[DG] SpeechStarted"));
        return;
    }

    if (strcmp(type, "Error") == 0) {
        Serial.print(F("[DG] Error: "));
        serializeJson(doc, Serial);
        Serial.println();
    }
}

void DeepgramASR::handleWebSocketData() {
    if (!_client.available()) {
        return;
    }

    uint8_t hdr[2];
    if (_client.readBytes(hdr, 2) != 2) {
        return;
    }

    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = hdr[1] & 0x80;
    uint64_t payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (_client.readBytes(ext, 2) != 2) {
            return;
        }
        payload_len = ((uint16_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (_client.readBytes(ext, 8) != 8) {
            return;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (_client.readBytes(mask, 4) != 4) {
            return;
        }
    }

    if (payload_len > 24576) {
        // drain oversized
        while (payload_len-- && _client.available()) {
            _client.read();
        }
        Serial.println(F("[DG] frame too big, dropped"));
        return;
    }

    // Read into buffer (PSRAM if available) — String += char слишком медленно для больших JSON
    char* buf = (char*)ps_malloc((size_t)payload_len + 1);
    if (!buf) {
        buf = (char*)malloc((size_t)payload_len + 1);
    }
    if (!buf) {
        while (payload_len-- && _client.available()) {
            _client.read();
        }
        Serial.println(F("[DG] OOM dropping frame"));
        return;
    }

    size_t got = 0;
    while (got < payload_len) {
        int c = _client.read();
        if (c < 0) {
            delay(1);
            c = _client.read();
            if (c < 0) {
                break;
            }
        }
        if (masked) {
            c ^= mask[got % 4];
        }
        buf[got++] = (char)c;
    }
    buf[got] = '\0';

    if (opcode == 0x01 || opcode == 0x02) {
        parseServerJson(buf, got);
    } else if (opcode == 0x08) {
        _wsConnected = false;
        _needReconnect = true;
        _isRecording = false;
        Serial.println(F("[DG] server closed"));
    } else if (opcode == 0x09) {
        sendWebSocketFrame(nullptr, 0, 0x0A);
    }

    free(buf);
}

void DeepgramASR::processMicSend() {
    if (!_isRecording) {
        return;
    }

    for (int i = 0; i < 800; i++) {
        if (!_I2S.available()) {
            break;
        }
        int raw = _I2S.read();
        if (raw == 0 || raw == -1 || raw == 1) {
            continue;
        }

        int16_t pcm;
        if (_use32bitSlot) {
            pcm = (int16_t)(((int32_t)raw) >> kPcmShiftRight);
        } else {
            pcm = (int16_t)raw;
        }

        _sendBuf[_sendPos++] = pcm;
        _dbgEnergy += (uint32_t)(pcm * pcm);
        _dbgSamples++;

        if (_sendPos >= kSendSamples) {
            sendBinaryAudio((uint8_t*)_sendBuf, (size_t)_sendPos * 2);
            _sendPos = 0;
            _dbgChunks++;
        }
    }

    if (millis() - _lastDebugMs >= 1000) {
        uint32_t rms = 0;
        if (_dbgSamples > 0) {
            rms = (uint32_t)sqrt((double)_dbgEnergy / (double)_dbgSamples);
        }
        Serial.printf("[DG] audio chunks/s=%u rms=%u speech=%d\n",
                      (unsigned)_dbgChunks, (unsigned)rms, (int)_hasSpeech);
        _dbgChunks = 0;
        _dbgEnergy = 0;
        _dbgSamples = 0;
        _lastDebugMs = millis();
    }

    yield();
}

void DeepgramASR::checkNoSpeechTimeout() {
    if (!_isRecording || _hasSpeech) {
        return;
    }
    if (millis() - _recordingStartMs > _silenceTimeoutMs) {
        Serial.println(F("[DG] no speech timeout"));
        stopRecording();
        if (_timeoutCb) {
            _timeoutCb();
        }
    }
}

void DeepgramASR::loop() {
    if (_wsConnected && !_client.connected()) {
        _wsConnected = false;
        _needReconnect = true;
        _isRecording = false;
        Serial.println(F("[DG] connection lost"));
    }

    while (_client.available()) {
        handleWebSocketData();
    }

    if (_isRecording) {
        processMicSend();
        if (millis() - _lastKeepAliveMs > 4000) {
            sendKeepAlive();
        }
        checkNoSpeechTimeout();
    }
}
