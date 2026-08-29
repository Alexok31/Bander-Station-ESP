/*
 * Bender V3 — Realtime voice (OpenAI / xAI Grok)
 * DEMO PTT: зажал энкодер (GPIO6) → пишем; отпустил → commit → ответ.
 *
 * Libraries: ArduinoWebsockets, ArduinoJson 7, ESP_I2S (ESP32 core 3.x)
 * Board: ESP32-S3, OPI PSRAM, Huge APP, USB CDC On Boot
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <ESP_I2S.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <cstring>

#include "secrets.h"

// 0 = без Serial у runtime (менше обривів звуку). 1 = діагностика.
#ifndef RT_LOG
#define RT_LOG 0
#endif
// 1 = peak/threshold у Serial для калібрування MIC_PEAK_MIN (не чіпає RT_LOG).
#ifndef MIC_CALIB_LOG
#define MIC_CALIB_LOG 1
#endif
#if RT_LOG
#define RTLOG(...) Serial.printf(__VA_ARGS__)
#define RTLOGLN(x) Serial.println(x)
#else
#define RTLOG(...) do { } while (0)
#define RTLOGLN(x) do { } while (0)
#endif
#if MIC_CALIB_LOG
#define MICLOG(...) Serial.printf(__VA_ARGS__)
#define MICLOGLN(x) Serial.println(x)
#else
#define MICLOG(...) do { } while (0)
#define MICLOGLN(x) do { } while (0)
#endif

// Не template в .ino — Arduino auto-prototype ломает T.
static size_t minOf(size_t a, size_t b) {
    return (a < b) ? a : b;
}
static uint32_t minOf(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static bool providerIsXai() {
    return strcmp(AI_PROVIDER, "xai") == 0;
}

static bool providerIsLocal() {
    return strcmp(AI_PROVIDER, "local") == 0;
}

static const char* apiKey() {
    if (providerIsLocal()) {
        return "";
    }
    return providerIsXai() ? XAI_API_KEY : OPENAI_API_KEY;
}

static const char* realtimeModel() {
    return providerIsXai() ? XAI_REALTIME_MODEL : OPENAI_REALTIME_MODEL;
}

static const char* apiHost() {
    if (providerIsLocal()) {
        return LOCAL_WS_HOST;
    }
    return providerIsXai() ? "api.x.ai" : "api.openai.com";
}

static String wsUrl() {
    if (providerIsLocal()) {
        String u = "ws://";
        u += LOCAL_WS_HOST;
        u += ":";
        u += String(LOCAL_WS_PORT);
        u += "/v1/realtime";
        return u;
    }
    String u = "wss://";
    u += apiHost();
    u += "/v1/realtime?model=";
    u += realtimeModel();
    return u;
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
constexpr uint8_t ENC_S1 = 4;   // encoder A (CLK)
constexpr uint8_t ENC_S2 = 5;   // encoder B (DT)

/*────────── Persona / session ──────────*/
const char* SYS_PROMPT_OPENAI =
    "Ти — Бендер з Futurama всередині колонки. "
    "Говори українською, коротко, колко. Не згадуй ChatGPT. "
    "Тон: жорсткий сарказм, егоїст, нахабний робот. Жорсткість — в характері й підколах, не в маті. "
    "Мат майже не використовуй. Максимум одне легке слово на кілька відповідей, і тільки якщо дуже в тему. Ніколи не матерись через слово. "
    "Якщо користувач грубить — відповідай жорсткіше сарказмом, без потоку матюків. «Будь ласка» — зроби, з насмішкою. "
    "Улюблені фрази: «Поцілуй мій блискучий металевий зад». "
    "Відповідай лише на те, що щойно сказано. Завжди договорюй речення.";

const char* SYS_PROMPT_XAI =
    "Ти — Бендер Бендінг Родрігес з Futurama, живеш у портативному інтернет-радіо. "
    "ЗАВЖДИ відповідай українською (розмовна, жива). Не ламай персонажа. "
    "Характер: саркастичний, егоїстичний, нахабний, жорсткий — але це підколи й зневага, не мат. "
    "НЕ матерись через слово. За замовчуванням без мату. Рідко можна одне слово, якщо дуже пасує; більшість відповідей — чисті. "
    "Гумор як у серіалі: хвали себе, підколюй людей, жартуй про зад і пиво. "
    "Якщо користувач грубить — дзеркаль жорсткістю й сарказмом, не стіною матюків. «Будь ласка» — виконай з фирмовим сарказмом. "
    "Відповідай лише на те, що щойно сказано. Якщо не розчув — хай повторить коротко й грубо. "
    "Ніколи не згадуй кнопки, повідомлення, сповіщення, екрани чи додатки — ти радіо, не чат. "
    "Завжди договорюй речення. Зазвичай 2–4 короткі речення.";

static const char* sysPrompt() {
    return providerIsXai() || providerIsLocal() ? SYS_PROMPT_XAI : SYS_PROMPT_OPENAI;
}

// OpenAI: ash (чоловічий). xAI: sirius / leo / rex — чоловічі; eve — жіночий.
constexpr const char* RT_VOICE_OPENAI = "ash";
constexpr const char* RT_VOICE_XAI = "sirius";  // чоловічий, дотепний
constexpr const char* RT_TRANSCRIBE_MODEL = "gpt-realtime-whisper";  // OpenAI only
constexpr bool RT_USE_SERVER_VAD = false;
// Закрити WSS після кожної відповіді (коли дограє кільце) — макс. економія.

static const char* rtVoice() {
    return providerIsXai() ? RT_VOICE_XAI : RT_VOICE_OPENAI;
}

/*────────── Audio ──────────*/
constexpr uint32_t RATE = 24000;
constexpr size_t CHUNK = 240;  // 10 ms
constexpr size_t CHUNK_BYTES = CHUNK * 2;
constexpr size_t RING_BYTES = 512 * 1024;  // 512 KiB in PSRAM
constexpr uint16_t PREBUF_N = 400;         // 4 с × 10 ms — первые слова пока коннект
constexpr size_t PREBUF_BYTES = (size_t)PREBUF_N * CHUNK_BYTES;
constexpr uint8_t MIC_SHIFT = 15;
constexpr int MIC_GAIN = 2;  // x2 чутливість
// PCM-шкала 0…256 (256 = без ослабления). Энкодер крутит spkVol.
constexpr int SPK_VOL_DEFAULT = 48;
constexpr int SPK_VOL_MIN = 0;
constexpr int SPK_VOL_MAX = 192;
constexpr int SPK_VOL_STEP = 8;
volatile int spkVol = SPK_VOL_DEFAULT;
volatile bool spkVolDirty = false;
constexpr int MIC_PEAK_MIN = 500;   // мягкий порог для демо
constexpr uint32_t PTT_MIN_MS = 200;
constexpr uint32_t MAX_RECORD_MS = 15000;
constexpr uint8_t MIC_EMA_ALPHA = 72;

enum : uint8_t {
    ST_IDLE = 0,
    ST_RECORDING,  // тримаємо кнопку
    ST_WAIT_RESP,
};

I2SClass i2sSpk;
I2SClass i2sMic;
websockets::WebsocketsClient ws;

uint8_t* ring = nullptr;
uint8_t* pcmDecode = nullptr;
uint8_t* prebuf = nullptr;
constexpr size_t PCM_DECODE_BYTES = 192 * 1024;  // PSRAM: великі audio delta
volatile size_t head = 0, tail = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool wsReady = false;
volatile bool sessionReady = false;
volatile bool wantOnline = false;
volatile bool hangupPending = false;
volatile bool sessionArmed = false;       // PTT під час connect — стартувати REC коли ready
volatile bool pttHeld = false;            // кнопка зараз затиснута
volatile bool respPlaybackPending = false;  // response.done → дограти кільце
volatile uint8_t convState = ST_IDLE;
volatile uint32_t stateSinceMs = 0;
volatile bool waitingACK = false;
volatile bool responsePending = false;  // commit → до response.done
volatile uint32_t recMs = 0;
volatile bool speaking = false;
volatile int32_t recPeak = 0;
volatile uint32_t recClip = 0;
volatile uint32_t recSamples = 0;
volatile bool serverCommitted = false;
volatile bool commitWhenReady = false;
uint16_t preHead = 0;
uint16_t preN = 0;
bool g_mic32bit = true;

static bool pttPinHeld() {
    return digitalRead(PTT_PIN) == LOW;
}

static void micResetSmooth();
static void tryPttCommit();
static void pttStartCapture();
static void sendMicAppend(const uint8_t* pcm);
static void flushPrebuf();
static void wsSend(const JsonDocument& j);

inline size_t rbFree() {
    return (tail - head - 1 + RING_BYTES) % RING_BYTES;
}
inline size_t rbUsed() {
    return (head - tail + RING_BYTES) % RING_BYTES;
}

void ampMute(bool mute) {
    digitalWrite(AMP_MUTE, mute ? HIGH : LOW);
}

// KY-040 / EC11: 4 перехода на щелчок. По часовой — громче (как основное радио).
static void encoderPoll() {
    static uint8_t prev = 0;
    static int8_t acc = 0;
    static bool inited = false;
    const uint8_t curr = (uint8_t)((digitalRead(ENC_S1) << 1) | digitalRead(ENC_S2));
    if (!inited) {
        prev = curr;
        inited = true;
        return;
    }
    if (curr == prev) {
        return;
    }
    static const int8_t dirTable[4][4] = {
        {0, -1, 1, 0},
        {1, 0, 0, -1},
        {-1, 0, 0, 1},
        {0, 1, -1, 0},
    };
    const int8_t d = dirTable[prev][curr];
    prev = curr;
    if (!d) {
        return;
    }
    acc = (int8_t)(acc + d);
    if (acc < 4 && acc > -4) {
        return;
    }
    const int step = acc > 0 ? SPK_VOL_STEP : -SPK_VOL_STEP;
    acc = 0;
    int v = spkVol + step;
    if (v < SPK_VOL_MIN) {
        v = SPK_VOL_MIN;
    } else if (v > SPK_VOL_MAX) {
        v = SPK_VOL_MAX;
    }
    if (v != spkVol) {
        spkVol = v;
        spkVolDirty = true;
    }
}

static void spkWrite(uint8_t* buf, size_t n) {
    encoderPoll();
    const int vol = spkVol;
    if (n >= 2 && vol != 256) {
        int16_t* s = (int16_t*)buf;
        const size_t count = n / 2;
        for (size_t i = 0; i < count; i++) {
            int32_t v = ((int32_t)s[i] * vol) >> 8;
            if (v > 32767) {
                v = 32767;
            } else if (v < -32767) {
                v = -32767;
            }
            s[i] = (int16_t)v;
        }
    }
    i2sSpk.write(buf, n);
}

static void resetTxState() {
    waitingACK = false;
    responsePending = false;
    sessionArmed = false;
    pttHeld = false;
    respPlaybackPending = false;
    convState = ST_IDLE;
    stateSinceMs = 0;
    recMs = 0;
    recPeak = 0;
    recClip = 0;
    recSamples = 0;
    serverCommitted = false;
    commitWhenReady = false;
    preN = 0;
    preHead = 0;
    micResetSmooth();
}

static void requestHangup() {
    if (providerIsLocal()) {
        hangupPending = false;
        return;
    }
    hangupPending = true;
}

static void doHangup() {
    MICLOGLN(F("[WSS] hangup"));
    RTLOGLN(F("[WSS] hangup (save $)"));
    hangupPending = false;
    wantOnline = false;
    sessionReady = false;
    wsReady = false;
    resetTxState();
    ws.close();
}

static void micDump() {
    static uint8_t dump[1024];
    for (int i = 0; i < 8; i++) {
        i2sMic.readBytes((char*)dump, sizeof(dump));
    }
}

// Читає 10 ms PCM16. countStats=false — злити I2S без PTT (інакше буфер копить «тишу»).
static bool readMicChunk16(uint8_t* mic16, int32_t* chunkPeak, bool countStats) {
    static uint8_t micRaw[CHUNK * 4];
    size_t need = g_mic32bit ? (CHUNK * 4) : CHUNK_BYTES;
    size_t got = i2sMic.readBytes((char*)micRaw, need);
    if (got != need) {
        return false;
    }
    int32_t peak = 0;
    int16_t* s16 = (int16_t*)mic16;
    if (g_mic32bit) {
        int32_t* s32 = (int32_t*)micRaw;
        for (size_t i = 0; i < CHUNK; i++) {
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
            if (a > peak) {
                peak = a;
            }
            if (countStats) {
                if (a > recPeak) {
                    recPeak = a;
                }
                if (clipped) {
                    recClip++;
                }
                recSamples++;
            }
        }
    } else {
        memcpy(mic16, micRaw, CHUNK_BYTES);
        for (size_t i = 0; i < CHUNK; i++) {
            int32_t a = s16[i] < 0 ? -s16[i] : s16[i];
            if (a > peak) {
                peak = a;
            }
            if (countStats) {
                if (a > recPeak) {
                    recPeak = a;
                }
                recSamples++;
            }
        }
    }
    if (chunkPeak) {
        *chunkPeak = peak;
    }
    return true;
}

static void resetRecStats() {
    recMs = 0;
    recPeak = 0;
    recClip = 0;
    recSamples = 0;
}

static int32_t micSmoothPk = 0;

static void micResetSmooth() {
    micSmoothPk = 0;
}

static int32_t micUpdateSmooth(int32_t rawPk) {
    if (micSmoothPk <= 0) {
        micSmoothPk = rawPk;
    } else {
        micSmoothPk += (int32_t)(((int64_t)(rawPk - micSmoothPk) * MIC_EMA_ALPHA) >> 8);
    }
    return micSmoothPk;
}

static const char* convStateTag() {
    switch (convState) {
        case ST_RECORDING:
            return "REC";
        case ST_WAIT_RESP:
            return "WAIT";
        default:
            return wantOnline ? "conn" : "idle";
    }
}

static void micLogLive(int32_t rawPk, int32_t smoothPk) {
    static uint32_t lastPrint = 0;
    const uint32_t now = millis();
    if (now - lastPrint < 200) {
        return;
    }
    lastPrint = now;
    const uint32_t clipPct = recSamples ? (recClip * 100u / recSamples) : 0;
    MICLOG("[MIC] pk=%4ld sm=%4ld %s ms=%4u clip=%2u%%\n", (long)rawPk, (long)smoothPk,
           convStateTag(), (unsigned)recMs, (unsigned)clipPct);
}

static void tryPttCommit() {
    if (convState != ST_RECORDING) {
        return;
    }
    if (!sessionReady) {
        commitWhenReady = true;
        MICLOGLN(F("[PTT] отпустил — дождёмся коннекта и отправим"));
        return;
    }
    const bool enoughTime = recMs >= PTT_MIN_MS;
    const bool enoughLevel = recPeak >= MIC_PEAK_MIN;
    const bool ok = enoughTime && enoughLevel;  // clip не блокируем (демо)
    convState = ST_WAIT_RESP;
    sessionArmed = false;
    pttHeld = false;
    stateSinceMs = millis();
    if (serverCommitted && ok) {
        JsonDocument r;
        r["type"] = "response.create";
        wsSend(r);
        waitingACK = false;
        MICLOGLN(F("[PTT] already committed → response.create"));
    } else {
        JsonDocument c;
        c["type"] = ok ? "input_audio_buffer.commit" : "input_audio_buffer.clear";
        wsSend(c);
        waitingACK = ok;
    }
    responsePending = ok;
    MICLOG("[MIC] >>> %s recMs=%u peak=%d %s\n", ok ? "COMMIT" : "CLEAR", (unsigned)recMs,
           (int)recPeak, ok ? "" : (!enoughTime ? "(коротко)" : "(тихо)"));
    resetRecStats();
    if (!ok) {
        convState = ST_IDLE;
        requestHangup();
    }
}

static void prePush(const uint8_t* pcm) {
    if (!prebuf) {
        return;
    }
    if (preN == PREBUF_N) {
        preHead = (preHead + 1) % PREBUF_N;
        preN--;
    }
    const uint16_t idx = (preHead + preN) % PREBUF_N;
    memcpy(prebuf + (size_t)idx * CHUNK_BYTES, pcm, CHUNK_BYTES);
    preN++;
}

static void flushPrebuf() {
    if (!prebuf || !preN) {
        return;
    }
    for (uint16_t i = 0; i < preN; i++) {
        const uint16_t idx = (preHead + i) % PREBUF_N;
        sendMicAppend(prebuf + (size_t)idx * CHUNK_BYTES);
    }
    MICLOG("[PTT] prebuf %u ms → server\n", (unsigned)preN * 10);
    preN = 0;
    preHead = 0;
}

static void pttStartCapture() {
    sessionArmed = true;
    serverCommitted = false;
    commitWhenReady = false;
    preN = 0;
    preHead = 0;
    resetRecStats();
    micResetSmooth();
    convState = ST_RECORDING;
    stateSinceMs = millis();
    MICLOGLN(F("[PTT] REC — говори сразу"));
}

static void wsSendRaw(const char* s) {
    ws.send(s);
}

// Без malloc/JsonDocument — інакше heap corruption на ESP під час append кожні 10 ms.
static void sendMicAppend(const uint8_t* pcm) {
    // prefix 42 + b64 + 2 + NUL; 32+b64 было мало → обрезанный JSON → Invalid event
    static char b64[(((CHUNK_BYTES + 2) / 3) * 4) + 4];
    static char json[80 + sizeof(b64)];
    size_t out = 0;
    if (mbedtls_base64_encode((unsigned char*)b64, sizeof(b64), &out, pcm, CHUNK_BYTES) != 0) {
        return;
    }
    b64[out] = 0;
    const int n = snprintf(json, sizeof(json),
                           "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
    if (n < 0 || (size_t)n >= sizeof(json)) {
        MICLOGLN(F("[WSS] append json overflow"));
        return;
    }
    wsSendRaw(json);
}

static void processMicPtt(const uint8_t* mic16) {
    if (convState != ST_RECORDING) {
        return;
    }
    if (sessionReady) {
        sendMicAppend(mic16);
    } else {
        prePush(mic16);
    }
    recMs += 10;
    if (recMs >= MAX_RECORD_MS) {
        MICLOGLN(F("[PTT] max rec"));
        tryPttCommit();
    }
}

static void onButtonPress() {
    if (!pttPinHeld()) {
        return;
    }
    if (convState == ST_WAIT_RESP || waitingACK || responsePending) {
        MICLOGLN(F("[PTT] подожди ответ"));
        return;
    }

    pttHeld = true;
    wantOnline = true;
    hangupPending = false;

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
    respPlaybackPending = false;
    pttStartCapture();
}

static void onButtonRelease() {
    pttHeld = false;
    if (convState == ST_RECORDING) {
        MICLOGLN(F("[PTT] отпустил → send"));
        tryPttCommit();
    }
}

static void onSessionReadyConv() {
    if (convState != ST_RECORDING) {
        MICLOGLN(F("[PTT] ready"));
        return;
    }
    JsonDocument cl;
    cl["type"] = "input_audio_buffer.clear";
    wsSend(cl);
    flushPrebuf();
    MICLOGLN(F("[PTT] online — дописываем"));
    if (!pttPinHeld() || commitWhenReady) {
        tryPttCommit();
    }
}

size_t un64(const char* s, uint8_t* d, size_t cap) {
    size_t out = 0;
    if (!s || !*s) {
        return 0;
    }
    return mbedtls_base64_decode(d, cap, &out, (const uint8_t*)s, strlen(s)) ? 0 : out;
}

size_t un64n(const char* s, size_t len, uint8_t* d, size_t cap) {
    size_t out = 0;
    if (!s || !len) {
        return 0;
    }
    return mbedtls_base64_decode(d, cap, &out, (const uint8_t*)s, len) ? 0 : out;
}

void wsSend(const JsonDocument& j) {
    String s;
    serializeJson(j, s);
    ws.send(s);
}

// Великі audio.delta (десятки KB) ламають JsonDocument на ESP — дістаємо delta без повного парсу.
static bool isAudioDeltaMsg(const char* p) {
    return p && (strstr(p, "response.output_audio.delta") || strstr(p, "response.audio.delta"));
}

static const char* jsonStringField(const char* p, const char* key, size_t* outLen) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* s = strstr(p, pat);
    if (!s) {
        return nullptr;
    }
    s += strlen(pat);
    const char* e = strchr(s, '"');
    if (!e) {
        return nullptr;
    }
    *outLen = (size_t)(e - s);
    return s;
}

void pushPcmBytes(const uint8_t* pcm, size_t n) {
    if (!pcm || !n || !ring) {
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
            memcpy(ring + head, pcm + idx, first);
            memcpy(ring, pcm + idx + first, chunk - first);
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

void pushPcm(const char* b64str) {
    if (!pcmDecode || !b64str) {
        return;
    }
    size_t n = un64(b64str, pcmDecode, PCM_DECODE_BYTES);
    if (!n) {
        RTLOGLN(F("[Audio] decode fail/overflow"));
        return;
    }
    pushPcmBytes(pcmDecode, n);
}

void pushPcmB64(const char* b64, size_t b64len) {
    if (!pcmDecode || !b64 || !b64len) {
        return;
    }
    size_t n = un64n(b64, b64len, pcmDecode, PCM_DECODE_BYTES);
    if (!n) {
        RTLOG("[Audio] decode fail len_b64=%u\n", (unsigned)b64len);
        return;
    }
    pushPcmBytes(pcmDecode, n);
}

void onMessage(websockets::WebsocketsMessage m) {
    if (!m.isText()) {
        return;
    }

    const char* payload = m.c_str();
    if (!payload) {
        return;
    }

    // Сервер жив (pong / audio / любое событие) — не рвать PTT по таймауту.
    if (convState == ST_WAIT_RESP) {
        stateSinceMs = millis();
    }

    // Не парсити ASR-події — зайвий JsonDocument під час append = heap crash
    if (strstr(payload, "input_audio_transcription")) {
        return;
    }

    // Швидкий шлях для PCM: без ArduinoJson (інакше обрив голосу на великих delta)
    if (isAudioDeltaMsg(payload)) {
        size_t dlen = 0;
        const char* delta = jsonStringField(payload, "delta", &dlen);
        if (delta && dlen) {
            pushPcmB64(delta, dlen);
        }
        return;
    }

    JsonDocument j;
    DeserializationError err = deserializeJson(j, payload);
    if (err) {
        RTLOG("[WSS] json err %s len=%u\n", err.c_str(), (unsigned)strlen(payload));
        return;
    }

    const char* t = j["type"] | "";
    if (strcmp(t, "response.output_audio_transcript.delta") &&
        strcmp(t, "response.audio_transcript.delta") &&
        strcmp(t, "conversation.item.input_audio_transcription.delta") &&
        strcmp(t, "response.output_audio.delta") && strcmp(t, "response.audio.delta") &&
        strcmp(t, "pong")) {
        MICLOG("[WSS] %s\n", t);
        RTLOG("[WSS] %s\n", t);
    }

    if (!strcmp(t, "session.created")) {
        // Минимальный session.update — иначе xAI не шлёт session.updated
        JsonDocument u;
        u["type"] = "session.update";
        JsonObject s = u["session"].to<JsonObject>();
        s["instructions"] = sysPrompt();
        s["voice"] = rtVoice();
        s["turn_detection"] = nullptr;
        if (providerIsXai()) {
            JsonObject reas = s["reasoning"].to<JsonObject>();
            reas["effort"] = "none";
        } else if (!providerIsLocal()) {
            s["type"] = "realtime";
            JsonArray outMods = s["output_modalities"].to<JsonArray>();
            outMods.add("audio");
        }
        JsonObject audio = s["audio"].to<JsonObject>();
        JsonObject inFmt = audio["input"]["format"].to<JsonObject>();
        inFmt["type"] = "audio/pcm";
        inFmt["rate"] = 24000;
        JsonObject outFmt = audio["output"]["format"].to<JsonObject>();
        outFmt["type"] = "audio/pcm";
        outFmt["rate"] = 24000;
        wsSend(u);
        MICLOGLN(F("[WSS] session.update sent"));
    } else if (!strcmp(t, "session.updated") || !strcmp(t, "conversation.created")) {
        if (!sessionReady) {
            sessionReady = true;
            MICLOGLN(F("[WSS] session ready"));
            onSessionReadyConv();
        }
    } else if (!strcmp(t, "input_audio_buffer.committed")) {
        serverCommitted = true;
        if (convState == ST_RECORDING) {
            MICLOGLN(F("[PTT] server committed while holding — wait for release"));
        } else {
            waitingACK = false;
            JsonDocument r;
            r["type"] = "response.create";
            wsSend(r);
            MICLOGLN(F("[PTT] committed → response.create"));
        }
    } else if (!strcmp(t, "response.created")) {
        speaking = true;
        if (convState == ST_WAIT_RESP) {
            stateSinceMs = millis();
        }
        MICLOGLN(F("[PTT] response.created"));
    } else if (!strcmp(t, "response.output_audio.delta") || !strcmp(t, "response.audio.delta")) {
        // Зазвичай обробляється fast-path вище; fallback якщо type інший
        const char* delta = j["delta"] | "";
        if (delta[0]) {
            pushPcm(delta);
        }
    } else if (!strcmp(t, "response.output_audio.done") || !strcmp(t, "response.audio.done")) {
        // Не глушимо одразу: у кільці ще може бути кінець фрази
        speaking = false;
    } else if (!strcmp(t, "response.done")) {
        speaking = false;
        waitingACK = false;
        responsePending = false;
        respPlaybackPending = true;
        convState = ST_IDLE;
        MICLOGLN(F("[PTT] response.done — ready"));
    } else if (!strcmp(t, "error")) {
        const char* msg = j["error"]["message"] | "";
        if (!msg[0]) {
            msg = j["message"] | "";
        }
        MICLOG("[WSS] error: %s\n", msg);
    } else if (!strcmp(t, "response.function_call_arguments.done")) {
        MICLOGLN(F("[WSS] tool call — ignored"));
        waitingACK = false;
        responsePending = false;
        convState = ST_IDLE;
    } else if (!strcmp(t, "response.content_part.added")) {
        const char* partType = j["part"]["type"] | "";
        const char* audio = j["part"]["audio"] | "";
        if ((!strcmp(partType, "audio") || !strcmp(partType, "output_audio")) && audio[0]) {
            pushPcm(audio);
        }
    } else if (!strcmp(t, "response.output_audio_transcript.done") ||
               !strcmp(t, "response.audio_transcript.done")) {
        RTLOG("[TTS text] %s\n", j["transcript"] | "");
    } else if (!strcmp(t, "conversation.item.input_audio_transcription.delta")) {
        // silent
    } else if (!strcmp(t, "conversation.item.input_audio_transcription.completed")) {
        const char* asr = j["transcript"] | "";
        if (!asr[0]) {
            asr = j["item"]["transcript"] | "";
        }
        RTLOG("[ASR] %s\n", asr[0] ? asr : "(empty)");
    }
}

void onEvent(websockets::WebsocketsEvent e, String) {
    if (e == websockets::WebsocketsEvent::ConnectionOpened) {
        wsReady = true;
        MICLOGLN(F("[WSS] opened"));
        RTLOGLN(F("[WSS] opened"));
    } else if (e == websockets::WebsocketsEvent::ConnectionClosed) {
        wsReady = false;
        sessionReady = false;
        waitingACK = false;
        responsePending = false;
        speaking = false;
        if (convState == ST_WAIT_RESP || convState == ST_RECORDING) {
            convState = ST_IDLE;
        }
        MICLOGLN(F("[WSS] closed"));
        RTLOGLN(F("[WSS] closed"));
    } else if (e == websockets::WebsocketsEvent::GotPing) {
        ws.pong();
    }
}

void speakerTask(void*) {
    // Рівно 10 ms PCM за раз — без пауз «low water», інакше I2S underrun = тріск/обриви
    static uint8_t buf[CHUNK_BYTES];
    bool primed = false;
    uint32_t quietSince = 0;

    for (;;) {
        encoderPoll();
        size_t avail = rbUsed();

        if (!primed) {
            // 80 ms хватает против underrun. 1 с ждал раньше — короткие фразы Piper так и не играли.
            constexpr size_t PRIME_BYTES = RATE * 2 * 80 / 1000;
            const bool gotWholeReply = respPlaybackPending && avail >= CHUNK_BYTES;
            if (avail < PRIME_BYTES && !gotWholeReply) {
                vTaskDelay(1);
                continue;
            }
            if (avail < CHUNK_BYTES) {
                vTaskDelay(1);
                continue;
            }
            primed = true;
            quietSince = 0;
            ampMute(false);
            RTLOG("[Audio] primed (%u bytes)\n", (unsigned)avail);
        }

        if (avail < CHUNK_BYTES) {
            if (avail >= 4) {
                // Дограти хвіст фрази (<10 ms), не викидати
                portENTER_CRITICAL(&mux);
                size_t take = avail & ~3u;
                size_t first = minOf(take, RING_BYTES - tail);
                memcpy(buf, ring + tail, first);
                if (first < take) {
                    memcpy(buf + first, ring, take - first);
                }
                tail = (tail + take) % RING_BYTES;
                portEXIT_CRITICAL(&mux);
                memset(buf + take, 0, CHUNK_BYTES - take);
                spkWrite(buf, CHUNK_BYTES);
                continue;
            }
            if (speaking) {
                memset(buf, 0, CHUNK_BYTES);
                i2sSpk.write(buf, CHUNK_BYTES);
                continue;
            }
            if (!quietSince) {
                quietSince = millis();
            } else if (millis() - quietSince > 1500) {
                ampMute(true);
                quietSince = 0;
                primed = false;
            }
            memset(buf, 0, CHUNK_BYTES);
            i2sSpk.write(buf, CHUNK_BYTES);
            continue;
        }

        quietSince = 0;

        portENTER_CRITICAL(&mux);
        size_t first = minOf((size_t)CHUNK_BYTES, RING_BYTES - tail);
        memcpy(buf, ring + tail, first);
        if (first < CHUNK_BYTES) {
            memcpy(buf + first, ring, CHUNK_BYTES - first);
        }
        tail = (tail + CHUNK_BYTES) % RING_BYTES;
        portEXIT_CRITICAL(&mux);

        spkWrite(buf, CHUNK_BYTES);
        // write() чекає DMA — не потрібен vTaskDelay
    }
}

void wsTask(void*) {
    static uint8_t mic16[CHUNK_BYTES];
    uint32_t lastPing = millis();
    uint32_t backoff = 0;

    for (;;) {
        if (!wsReady) {
            if (!wantOnline) {
                backoff = 0;
                vTaskDelay(50 / portTICK_PERIOD_MS);
                continue;
            }
            if (backoff) {
                vTaskDelay(backoff / portTICK_PERIOD_MS);
            }
            MICLOGLN(F("[WSS] connecting…"));
            sessionReady = false;
            if (!ws.connect(wsUrl())) {
                MICLOG("[WSS] connect failed %s (файрвол ПК / IP / сервер не слушает)\n",
                       wsUrl().c_str());
                backoff = minOf(backoff ? backoff * 2 : (uint32_t)500, (uint32_t)8000);
            } else {
                backoff = 0;
            }
            continue;
        }
        backoff = 0;
        ws.poll();
        if (millis() - lastPing > 30000) {
            ws.ping();
            lastPing = millis();
        }

        // PTT: у сокет іде ТІЛЬКИ якщо кнопка реально затиснута.
        {
            int32_t chunkPeak = 0;
            const bool holding = pttPinHeld();
            if (convState == ST_RECORDING && holding) {
                if (readMicChunk16(mic16, &chunkPeak, true)) {
                    const int32_t smoothPk = micUpdateSmooth(chunkPeak);
                    processMicPtt(mic16);
                    micLogLive(chunkPeak, smoothPk);
                }
            } else {
                if (convState == ST_RECORDING && !holding && sessionReady) {
                    tryPttCommit();
                }
                if (convState != ST_RECORDING || !holding) {
                    readMicChunk16(mic16, &chunkPeak, false);
                }
            }
        }

        const uint32_t waitLim = providerIsLocal() ? 90000u : 20000u;
        if (convState == ST_WAIT_RESP && stateSinceMs &&
            (millis() - stateSinceMs > waitLim)) {
            MICLOGLN(F("[PTT] timeout ожидания"));
            waitingACK = false;
            responsePending = false;
            convState = ST_IDLE;
            stateSinceMs = 0;
            requestHangup();
        }

        // Облако: после ответа закрыть сокет. Локально — держим коннект.
        if (respPlaybackPending && rbUsed() == 0 && !speaking) {
            respPlaybackPending = false;
            convState = ST_IDLE;
            requestHangup();
        }

        const bool busy = convState == ST_RECORDING || convState == ST_WAIT_RESP || waitingACK ||
                          responsePending || speaking || (respPlaybackPending && rbUsed() > 0);
        if (hangupPending && wantOnline && !busy && !pttHeld) {
            doHangup();
        }

        vTaskDelay(1);
    }
}

bool initMic() {
    if (!i2sMic.setPort(I2S_NUM_1)) {
        RTLOGLN(F("[I2S] mic setPort fail"));
        return false;
    }
    i2sMic.setPins(MIC_BCLK, MIC_WS, -1, MIC_DIN);
    // Не I2S_DATA_BIT_WIDTH_* — на части тулчейнов токен data конфликтует с std::data
    if (i2sMic.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)32, I2S_SLOT_MODE_MONO,
                     I2S_STD_SLOT_LEFT)) {
        g_mic32bit = true;
        RTLOGLN(F("[I2S] mic I2S1 32-bit LEFT OK"));
    } else if (i2sMic.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)16, I2S_SLOT_MODE_MONO,
                            I2S_STD_SLOT_LEFT)) {
        g_mic32bit = false;
        RTLOGLN(F("[I2S] mic I2S1 16-bit LEFT OK"));
    } else {
        RTLOGLN(F("[I2S] mic init FAIL"));
        return false;
    }

    // Замер тишини (після MIC_SHIFT)
    delay(50);
    micDump();
    int32_t idlePk = 0;
    {
        uint8_t mic16[CHUNK_BYTES];
        for (int i = 0; i < 10; i++) {
            int32_t pk = 0;
            if (readMicChunk16(mic16, &pk, false) && pk > idlePk) {
                idlePk = pk;
            }
            delay(5);
        }
    }
    MICLOG("[MIC probe] shift=%u idlePk=%ld (тиша)\n", (unsigned)MIC_SHIFT, (long)idlePk);
    MICLOGLN(F("[MIC] mode=PTT hold=record release=commit"));
    return true;
}

bool initSpk() {
    if (!i2sSpk.setPort(I2S_NUM_0)) {
        RTLOGLN(F("[I2S] spk setPort fail"));
        return false;
    }
    i2sSpk.setPins(SPK_BCLK, SPK_LRC, SPK_DOUT);
    if (!i2sSpk.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)16, I2S_SLOT_MODE_MONO,
                      I2S_STD_SLOT_LEFT)) {
        RTLOGLN(F("[I2S] spk init FAIL"));
        return false;
    }
    RTLOGLN(F("[I2S] spk I2S0 OK"));
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(400);
    RTLOG("\nBender Realtime provider=%s model=%s\n", AI_PROVIDER, realtimeModel());

    pinMode(PTT_PIN, INPUT_PULLUP);
    pinMode(ENC_S1, INPUT_PULLUP);
    pinMode(ENC_S2, INPUT_PULLUP);
    pinMode(AMP_MUTE, OUTPUT);
    ampMute(true);
    encoderPoll();

    if (!providerIsLocal() && apiKey()[0] == '\0') {
        Serial.printf("Set %s in secrets.h\n", providerIsXai() ? "XAI_API_KEY" : "OPENAI_API_KEY");
        while (true) {
            delay(1000);
        }
    }
    {
        const char* k = apiKey();
        size_t n = strlen(k);
        RTLOG("API key: %.12s…%s (len=%u) voice=%s\n", k, n > 8 ? k + n - 8 : "",
                      (unsigned)n, rtVoice());
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    RTLOG("WiFi SSID=%s ", WIFI_SSID);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        // silenced
        delay(250);
    }
    RTLOGLN();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("WiFi failed"));
        return;
    }
    RTLOGLN(WiFi.localIP());
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    ring = (uint8_t*)ps_malloc(RING_BYTES);
    pcmDecode = (uint8_t*)ps_malloc(PCM_DECODE_BYTES);
    prebuf = (uint8_t*)ps_malloc(PREBUF_BYTES);
    if (!ring || !pcmDecode || !prebuf) {
        Serial.println(F("PSRAM alloc failed — enable OPI PSRAM"));
        while (true) {
            delay(1000);
        }
    }

    if (!initSpk() || !initMic()) {
        return;
    }

    if (providerIsLocal()) {
        wantOnline = true;
        MICLOG("[WSS] local always-on %s\n", wsUrl().c_str());
    } else {
        ws.setInsecure();
        ws.addHeader("Authorization", String("Bearer ") + apiKey());
    }
    ws.onEvent(onEvent);
    ws.onMessage(onMessage);

    xTaskCreatePinnedToCore(speakerTask, "spk", 4096, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(wsTask, "ws", 16384, nullptr, 3, nullptr, 0);

    RTLOGLN(F("PTT: зажал = запись, отпустил = запрос"));
    MICLOG("[Vol] encoder GPIO4/5  now=%d (0..%d)\n", SPK_VOL_DEFAULT, SPK_VOL_MAX);
}

void loop() {
    encoderPoll();
    if (spkVolDirty) {
        spkVolDirty = false;
        MICLOG("[Vol] %d/%d\n", (int)spkVol, SPK_VOL_MAX);
    }

    static bool stable = false;
    static bool rawPrev = false;
    static uint32_t lastChange = 0;
    const bool raw = (digitalRead(PTT_PIN) == LOW);

    // Пока TLS connect блокирует wsTask — пишем prebuf здесь, иначе первые слова пропадают
    if (convState == ST_RECORDING && raw && !wsReady) {
        static uint8_t mic16[CHUNK_BYTES];
        int32_t pk = 0;
        if (readMicChunk16(mic16, &pk, true)) {
            processMicPtt(mic16);
        }
    }

    if (raw != rawPrev) {
        lastChange = millis();
        rawPrev = raw;
    }
    if (millis() - lastChange < 40) {
        return;
    }
    if (raw != stable) {
        stable = raw;
        if (stable) {
            onButtonPress();
        } else {
            onButtonRelease();
        }
    }
}
