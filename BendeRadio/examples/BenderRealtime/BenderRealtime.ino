/*
 * Bender V3 — OpenAI Realtime (адаптация OpenDino)
 * https://github.com/RASPIAUDIO/OpenDino
 *
 * Удержание кнопки энкодера (GPIO 6) = говоришь (PTT).
 * Отпустил = commit → ответ голосом.
 *
 * Libraries: ArduinoWebsockets (>=0.5), ArduinoJson 7, ESP_I2S (ESP32 core 3.x)
 * Board: ESP32-S3, OPI PSRAM, Huge APP, USB CDC On Boot
 * Serial: 115200
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <ESP_I2S.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

#include "secrets.h"

// Не template в .ino — Arduino auto-prototype ломает T.
static size_t minOf(size_t a, size_t b) {
    return (a < b) ? a : b;
}
static uint32_t minOf(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

/*────────── Bender pins ──────────*/
constexpr uint8_t SPK_BCLK = 44;
constexpr uint8_t SPK_LRC = 2;
constexpr uint8_t SPK_DOUT = 21;
constexpr uint8_t AMP_MUTE = 41;  // HIGH = muted (idle)

constexpr uint8_t MIC_BCLK = 9;
constexpr uint8_t MIC_WS = 10;
constexpr uint8_t MIC_DIN = 8;

constexpr uint8_t PTT_PIN = 6;  // encoder button, active LOW

/*────────── Realtime ──────────*/
#define WS_URL "wss://api.openai.com/v1/realtime?model=" REALTIME_MODEL

const char* SYS_PROMPT =
    "You are Bender from Futurama inside a speaker. "
    "Speak Russian, short, rude and funny. Do not mention ChatGPT. "
    "Only answer what the user just said into the microphone. "
    "If speech is unclear, ask once to repeat briefly.";

// Параметры из OpenAI Realtime dashboard (session JSON).
// Голос: alloy ash ballad coral echo sage shimmer verse marin cedar
constexpr const char* RT_VOICE = "ash";
constexpr const char* RT_TRANSCRIBE_MODEL = "gpt-realtime-whisper";
// true = как в дашборде (сам ловит паузу). false = только кнопка энкодера (PTT).
constexpr bool RT_USE_SERVER_VAD = false;

/*────────── Audio ──────────*/
constexpr uint32_t RATE = 24000;
constexpr size_t CHUNK = 240;  // 10 ms
constexpr size_t CHUNK_BYTES = CHUNK * 2;
constexpr size_t RING_BYTES = 512 * 1024;  // 512 KiB in PSRAM
constexpr uint8_t MIC_SHIFT = 14;  // как RadioConfig / Deepgram на этом железе
constexpr int MIC_GAIN = 1;
constexpr int MIC_PEAK_MIN = 200;
constexpr uint32_t PTT_MIN_MS = 400;

I2SClass i2sSpk;
I2SClass i2sMic;
websockets::WebsocketsClient ws;

uint8_t* ring = nullptr;
volatile size_t head = 0, tail = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool wsReady = false;
volatile bool sessionReady = false;
volatile bool pttHeld = false;
volatile bool txActive = false;
volatile bool commitPending = false;
volatile bool waitingACK = false;
volatile uint32_t releaseT = 0;
volatile uint32_t recMs = 0;
volatile bool speaking = false;
volatile int32_t recPeak = 0;
volatile uint32_t recClip = 0;
volatile uint32_t recSamples = 0;
bool g_mic32bit = true;

inline size_t rbFree() {
    return (tail - head - 1 + RING_BYTES) % RING_BYTES;
}
inline size_t rbUsed() {
    return (head - tail + RING_BYTES) % RING_BYTES;
}

void ampMute(bool mute) {
    digitalWrite(AMP_MUTE, mute ? HIGH : LOW);
}

String b64(const uint8_t* d, size_t n) {
    size_t out = 0;
    size_t cap = ((n + 2) / 3) * 4 + 1;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        return "";
    }
    mbedtls_base64_encode((uint8_t*)buf, cap, &out, d, n);
    buf[out] = 0;
    String s(buf);
    free(buf);
    return s;
}

size_t un64(const char* s, uint8_t* d, size_t cap) {
    size_t out = 0;
    if (!s || !*s) {
        return 0;
    }
    return mbedtls_base64_decode(d, cap, &out, (const uint8_t*)s, strlen(s)) ? 0 : out;
}

void wsSend(const JsonDocument& j) {
    String s;
    serializeJson(j, s);
    ws.send(s);
}

void pushPcm(const char* b64str) {
    static uint8_t tmp[CHUNK_BYTES * 160];
    size_t n = un64(b64str, tmp, sizeof(tmp));
    if (!n) {
        return;
    }

    ampMute(false);
    size_t idx = 0;
    while (idx < n) {
        portENTER_CRITICAL(&mux);
        size_t freeb = rbFree();
        size_t chunk = minOf(freeb, n - idx);
        if (chunk) {
            size_t first = minOf(chunk, RING_BYTES - head);
            memcpy(ring + head, tmp + idx, first);
            memcpy(ring, tmp + idx + first, chunk - first);
            head = (head + chunk) % RING_BYTES;
            idx += chunk;
        }
        portEXIT_CRITICAL(&mux);
        if (idx < n) {
            vTaskDelay(1);
        }
    }
    speaking = true;
}

void onMessage(websockets::WebsocketsMessage m) {
    if (!m.isText()) {
        return;
    }

    // Не вызывать m.data()/rawData(): на ESP32 core 3.x конфликт с std::data
    const char* payload = m.c_str();
    JsonDocument j;
    DeserializationError err = deserializeJson(j, payload);
    if (err) {
        return;
    }

    const char* t = j["type"] | "";
    Serial.printf("[WSS] %s\n", t);

    if (!strcmp(t, "session.created")) {
        // Совпадает с dashboard session JSON (voice / noise_reduction / transcription).
        JsonDocument u;
        u["type"] = "session.update";
        JsonObject s = u["session"].to<JsonObject>();
        s["type"] = "realtime";
        s["instructions"] = SYS_PROMPT;
        s["max_output_tokens"] = "inf";
        JsonArray outMods = s["output_modalities"].to<JsonArray>();
        outMods.add("audio");
        s["tools"].to<JsonArray>();  // пусто

        JsonObject audio = s["audio"].to<JsonObject>();
        JsonObject inA = audio["input"].to<JsonObject>();
        JsonObject inFmt = inA["format"].to<JsonObject>();
        inFmt["type"] = "audio/pcm";
        inFmt["rate"] = 24000;
        JsonObject tr = inA["transcription"].to<JsonObject>();
        tr["model"] = RT_TRANSCRIBE_MODEL;
        JsonObject nr = inA["noise_reduction"].to<JsonObject>();
        nr["type"] = "far_field";
        if (RT_USE_SERVER_VAD) {
            JsonObject vad = inA["turn_detection"].to<JsonObject>();
            vad["type"] = "server_vad";
            vad["threshold"] = 0.88;
            vad["prefix_padding_ms"] = 980;
            vad["silence_duration_ms"] = 500;
            vad["idle_timeout_ms"] = nullptr;
        } else {
            inA["turn_detection"] = nullptr;  // PTT: commit по отпусканию кнопки
        }

        JsonObject outA = audio["output"].to<JsonObject>();
        JsonObject outFmt = outA["format"].to<JsonObject>();
        outFmt["type"] = "audio/pcm";
        outFmt["rate"] = 24000;
        outA["voice"] = RT_VOICE;
        wsSend(u);
    } else if (!strcmp(t, "session.updated")) {
        sessionReady = true;
        if (RT_USE_SERVER_VAD) {
            Serial.println(F("[WSS] session ready — VAD on (speak freely)"));
        } else {
            Serial.println(F("[WSS] session ready — HOLD ≥0.5s, speak, then release"));
        }
    } else if (!strcmp(t, "input_audio_buffer.committed") && waitingACK) {
        waitingACK = false;
        JsonDocument r;
        r["type"] = "response.create";
        wsSend(r);
    } else if (!strcmp(t, "response.output_audio.delta") || !strcmp(t, "response.audio.delta")) {
        const char* delta = j["delta"] | "";
        if (delta[0]) {
            pushPcm(delta);
        }
    } else if (!strcmp(t, "response.output_audio.done") || !strcmp(t, "response.audio.done")) {
        speaking = false;
    } else if (!strcmp(t, "response.content_part.added")) {
        const char* partType = j["part"]["type"] | "";
        const char* audio = j["part"]["audio"] | "";
        if ((!strcmp(partType, "audio") || !strcmp(partType, "output_audio")) && audio[0]) {
            pushPcm(audio);
        }
    } else if (!strcmp(t, "error")) {
        Serial.print(F("[WSS] error: "));
        serializeJson(j["error"], Serial);
        Serial.println();
    } else if (!strcmp(t, "response.output_audio_transcript.done") ||
               !strcmp(t, "response.audio_transcript.done")) {
        Serial.printf("[TTS text] %s\n", j["transcript"] | "");
    } else if (!strcmp(t, "conversation.item.input_audio_transcription.delta")) {
        const char* d = j["delta"] | "";
        if (d[0]) {
            Serial.printf("[ASR+] %s\n", d);
        }
    } else if (!strcmp(t, "conversation.item.input_audio_transcription.completed")) {
        const char* asr = j["transcript"] | "";
        if (!asr[0]) {
            asr = j["item"]["transcript"] | "";
        }
        Serial.printf("[ASR] %s\n", asr[0] ? asr : "(empty)");
    }
}

void onEvent(websockets::WebsocketsEvent e, String data) {
    if (e == websockets::WebsocketsEvent::ConnectionOpened) {
        wsReady = true;
        Serial.println(F("[WSS] opened"));
    } else if (e == websockets::WebsocketsEvent::ConnectionClosed) {
        wsReady = false;
        sessionReady = false;
        Serial.print(F("[WSS] closed"));
        if (data.length()) {
            Serial.print(F(" "));
            Serial.print(data);
        }
        Serial.println();
    } else if (e == websockets::WebsocketsEvent::GotPing) {
        ws.pong();
    }
}

void speakerTask(void*) {
    static uint8_t buf[CHUNK_BYTES * 4 + 4];
    bool primed = false;
    constexpr size_t PRIME_BYTES = RATE * 2 * 300 / 1000;

    for (;;) {
        size_t avail = rbUsed();
        if (!primed) {
            if (avail < PRIME_BYTES) {
                vTaskDelay(1);
                continue;
            }
            primed = true;
            Serial.println(F("[Audio] primed"));
        }
        if (avail == 0) {
            memset(buf, 0, CHUNK_BYTES);
            i2sSpk.write(buf, CHUNK_BYTES);
            if (!speaking) {
                static uint32_t quietSince = 0;
                if (!quietSince) {
                    quietSince = millis();
                } else if (millis() - quietSince > 800) {
                    ampMute(true);
                    quietSince = 0;
                    primed = false;
                }
            }
            vTaskDelay(1);
            continue;
        }

        portENTER_CRITICAL(&mux);
        size_t take = minOf(avail, (size_t)(sizeof(buf) - 4));
        size_t first = minOf(take, RING_BYTES - tail);
        memcpy(buf, ring + tail, first);
        memcpy(buf + first, ring, take - first);
        tail = (tail + take) % RING_BYTES;
        portEXIT_CRITICAL(&mux);

        if (take & 3) {
            memset(buf + take, 0, 4 - (take & 3));
            take = (take + 3) & ~3;
        }
        ampMute(false);
        i2sSpk.write(buf, take);
        vTaskDelay(1);
    }
}

void wsTask(void*) {
    static uint8_t micRaw[CHUNK * 4];  // room for 32-bit
    static uint8_t mic16[CHUNK_BYTES];
    uint32_t lastPing = millis();
    uint32_t backoff = 1000;

    for (;;) {
        if (!wsReady) {
            vTaskDelay(backoff / portTICK_PERIOD_MS);
            Serial.printf("[WSS] connecting %s …\n", WS_URL);
            if (!ws.connect(WS_URL)) {
                Serial.println(F("[WSS] connect failed (TLS/DNS/handshake)"));
            }
            backoff = minOf(backoff * 2, (uint32_t)16000);
            continue;
        }
        backoff = 1000;
        ws.poll();
        if (millis() - lastPing > 30000) {
            ws.ping();
            lastPing = millis();
        }

        if (sessionReady && txActive) {
            size_t need = g_mic32bit ? (CHUNK * 4) : CHUNK_BYTES;
            size_t got = i2sMic.readBytes((char*)micRaw, need);
            if (got == need) {
                int16_t* s16 = (int16_t*)mic16;
                if (g_mic32bit) {
                    int32_t* s32 = (int32_t*)micRaw;
                    for (size_t i = 0; i < CHUNK; i++) {
                        // INMP441: 24-bit left-justified in 32-bit word
                        int32_t v = (s32[i] >> MIC_SHIFT) * MIC_GAIN;
                        bool clipped = false;
                        if (v > 32767) {
                            v = 32767;
                            clipped = true;
                        } else if (v < -32767) {
                            v = -32767;
                            clipped = true;
                        }
                        s16[i] = (int16_t)v;
                        int32_t a = v < 0 ? -v : v;
                        if (a > recPeak) {
                            recPeak = a;
                        }
                        if (clipped) {
                            recClip++;
                        }
                        recSamples++;
                    }
                } else {
                    memcpy(mic16, micRaw, CHUNK_BYTES);
                    for (size_t i = 0; i < CHUNK; i++) {
                        int32_t a = s16[i] < 0 ? -s16[i] : s16[i];
                        if (a > recPeak) {
                            recPeak = a;
                        }
                        recSamples++;
                    }
                }
                JsonDocument a;
                a["type"] = "input_audio_buffer.append";
                a["audio"] = b64(mic16, CHUNK_BYTES);
                wsSend(a);
                recMs += 10;
                if ((recMs % 500u) == 0u) {
                    uint32_t clipPct = recSamples ? (recClip * 100u / recSamples) : 0;
                    Serial.printf("[MIC] peak=%d clip=%u%% rec=%ums\n", (int)recPeak,
                                  (unsigned)clipPct, (unsigned)recMs);
                }
            }
        }

        if (commitPending && millis() - releaseT > 60) {
            const bool enoughTime = recMs >= PTT_MIN_MS;
            const bool enoughLevel = recPeak >= MIC_PEAK_MIN;
            const bool tooClipped = recSamples && (recClip * 100u / recSamples) > 40u;
            const bool ok = enoughTime && enoughLevel && !tooClipped;
            JsonDocument c;
            c["type"] = ok ? "input_audio_buffer.commit" : "input_audio_buffer.clear";
            wsSend(c);
            waitingACK = ok;
            commitPending = false;
            uint32_t clipPct = recSamples ? (recClip * 100u / recSamples) : 0;
            Serial.printf("[PTT] %s (rec=%ums peak=%d clip=%u%%)%s\n", ok ? "commit" : "clear",
                          (unsigned)recMs, (int)recPeak, (unsigned)clipPct,
                          ok            ? ""
                          : !enoughTime ? " — hold longer"
                          : tooClipped  ? " — mic clipped/garbage, check wiring"
                                        : " — too quiet");
            recMs = 0;
            recPeak = 0;
            recClip = 0;
            recSamples = 0;
        }
        vTaskDelay(1);
    }
}

bool initMic() {
    if (!i2sMic.setPort(I2S_NUM_1)) {
        Serial.println(F("[I2S] mic setPort fail"));
        return false;
    }
    i2sMic.setPins(MIC_BCLK, MIC_WS, -1, MIC_DIN);
    // Не I2S_DATA_BIT_WIDTH_* — на части тулчейнов токен data конфликтует с std::data
    if (i2sMic.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)32, I2S_SLOT_MODE_MONO,
                     I2S_STD_SLOT_LEFT)) {
        g_mic32bit = true;
        Serial.println(F("[I2S] mic I2S1 32-bit LEFT OK"));
    } else if (i2sMic.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)16, I2S_SLOT_MODE_MONO,
                            I2S_STD_SLOT_LEFT)) {
        g_mic32bit = false;
        Serial.println(F("[I2S] mic I2S1 16-bit LEFT OK"));
    } else {
        Serial.println(F("[I2S] mic init FAIL"));
        return false;
    }

    // Быстрый замер: тишина / живой сигнал
    delay(50);
    int32_t peak14 = 0, peak16 = 0;
    int32_t raw0 = 0;
    if (g_mic32bit) {
        int32_t buf[240];
        size_t got = i2sMic.readBytes((char*)buf, sizeof(buf));
        size_t n = got / 4;
        if (n > 0) {
            raw0 = buf[0];
        }
        for (size_t i = 0; i < n; i++) {
            int32_t a14 = buf[i] >> 14;
            int32_t a16 = buf[i] >> 16;
            if (a14 < 0) {
                a14 = -a14;
            }
            if (a16 < 0) {
                a16 = -a16;
            }
            if (a14 > peak14) {
                peak14 = a14;
            }
            if (a16 > peak16) {
                peak16 = a16;
            }
        }
    }
    Serial.printf("[MIC probe] raw0=0x%08lx peak>>14=%ld peak>>16=%ld (speak near mic: peak should move)\n",
                  (unsigned long)raw0, (long)peak14, (long)peak16);
    return true;
}

bool initSpk() {
    if (!i2sSpk.setPort(I2S_NUM_0)) {
        Serial.println(F("[I2S] spk setPort fail"));
        return false;
    }
    i2sSpk.setPins(SPK_BCLK, SPK_LRC, SPK_DOUT);
    if (!i2sSpk.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)16, I2S_SLOT_MODE_MONO,
                      I2S_STD_SLOT_LEFT)) {
        Serial.println(F("[I2S] spk init FAIL"));
        return false;
    }
    Serial.println(F("[I2S] spk I2S0 OK"));
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println(F("\nBender Realtime (OpenDino port)"));

    pinMode(PTT_PIN, INPUT_PULLUP);
    pinMode(AMP_MUTE, OUTPUT);
    ampMute(true);

    if (OPENAI_API_KEY[0] == '\0') {
        Serial.println(F("Set OPENAI_API_KEY in secrets.h"));
        while (true) {
            delay(1000);
        }
    }
    {
        const char* k = OPENAI_API_KEY;
        size_t n = strlen(k);
        Serial.printf("OpenAI key: %.12s…%s (len=%u) model=%s\n", k, n > 8 ? k + n - 8 : "",
                      (unsigned)n, REALTIME_MODEL);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("WiFi SSID=%s ", WIFI_SSID);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        Serial.print('.');
        delay(250);
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("WiFi failed"));
        return;
    }
    Serial.println(WiFi.localIP());
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    {
        IPAddress ip;
        if (WiFi.hostByName("api.openai.com", ip)) {
            Serial.printf("[DNS] api.openai.com -> %s\n", ip.toString().c_str());
        } else {
            Serial.println(F("[DNS] api.openai.com FAILED"));
        }
        WiFiClientSecure probe;
        probe.setInsecure();
        probe.setTimeout(12);
        Serial.println(F("[TLS] probe https://api.openai.com …"));
        if (probe.connect("api.openai.com", 443)) {
            probe.print("GET /v1/models HTTP/1.0\r\nHost: api.openai.com\r\nConnection: close\r\n\r\n");
            uint32_t t0 = millis();
            while (probe.connected() && !probe.available() && millis() - t0 < 5000) {
                delay(10);
            }
            String line = probe.readStringUntil('\n');
            Serial.printf("[TLS] %s\n", line.c_str());
            probe.stop();
        } else {
            Serial.println(F("[TLS] connect FAIL — сеть/firewall режет 443 к OpenAI"));
        }
    }

    ring = (uint8_t*)ps_malloc(RING_BYTES);
    if (!ring) {
        Serial.println(F("PSRAM alloc failed — enable OPI PSRAM"));
        while (true) {
            delay(1000);
        }
    }

    if (!initSpk() || !initMic()) {
        return;
    }

    // ESP32 + устаревший root CA → connect failed за ~100ms. Для теста без pin CA.
    ws.setInsecure();
    ws.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
    // GA realtime — без OpenAI-Beta. Для старого gpt-4o-*-realtime-preview раскомментируй:
    // ws.addHeader("OpenAI-Beta", "realtime=v1");
    ws.onEvent(onEvent);
    ws.onMessage(onMessage);

    xTaskCreatePinnedToCore(speakerTask, "spk", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(wsTask, "ws", 12288, nullptr, 4, nullptr, 0);

    Serial.println(F("Hold encoder button to talk, release to send"));
}

void loop() {
    bool held = (digitalRead(PTT_PIN) == LOW);

    if (held && !pttHeld) {
        if (!sessionReady) {
            Serial.println(F("Wait for session…"));
        } else {
            // Прервать ответ бота только если он реально говорит
            if (speaking) {
                JsonDocument c;
                c["type"] = "response.cancel";
                wsSend(c);
                speaking = false;
                ampMute(true);
                portENTER_CRITICAL(&mux);
                head = tail = 0;
                portEXIT_CRITICAL(&mux);
            }
            {
                JsonDocument cl;
                cl["type"] = "input_audio_buffer.clear";
                wsSend(cl);
            }
            // Слить старый буфер I2S микрофона
            static uint8_t dump[1024];
            for (int i = 0; i < 8; i++) {
                i2sMic.readBytes((char*)dump, sizeof(dump));
            }
            txActive = true;
            recMs = 0;
            recPeak = 0;
            recClip = 0;
            recSamples = 0;
            releaseT = millis();
            Serial.println(F("► REC (speak now)"));
        }
    }
    if (!held && pttHeld) {
        txActive = false;
        commitPending = true;
        releaseT = millis();
        Serial.printf("■ STOP peak=%d\n", (int)recPeak);
    }
    pttHeld = held;
    delay(5);
}
