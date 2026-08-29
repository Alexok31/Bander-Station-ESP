#include "BenderAi.h"

#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include <esp_wifi.h>
#include <mbedtls/base64.h>

#include "RadioConfig.h"
#include "core0.h"
#include "pcm_analyzer.h"
#include "secrets.h"

#ifndef BENDER_AI_LOG
#define BENDER_AI_LOG 1
#endif
#if BENDER_AI_LOG
#define ALOG(...) Serial.printf(__VA_ARGS__)
#define ALOGLN(x) Serial.println(x)
#else
#define ALOG(...) \
    do {          \
    } while (0)
#define ALOGLN(x) \
    do {          \
    } while (0)
#endif

extern Audio audio;
extern Data radioState;
extern const char* reconnect;
extern char g_audio_source[8];

static size_t minOf(size_t a, size_t b) {
    return (a < b) ? a : b;
}
static uint32_t minOfU32(uint32_t a, uint32_t b) {
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

static const char* SYS_PROMPT =
    "Ти — Бендер Бендінг Родрігес з Futurama, живеш у портативному інтернет-радіо. "
    "ЗАВЖДИ відповідай українською (розмовна, жива). Не ламай персонажа. "
    "Характер: саркастичний, егоїстичний, нахабний, жорсткий — але це підколи й зневага, не мат. "
    "НЕ матерись через слово. За замовчуванням без мату. Рідко можна одне слово, якщо дуже пасує. "
    "Відповідай лише на те, що щойно сказано. Завжди договорюй речення. "
    "Ніколи не згадуй кнопки, повідомлення, екрани чи додатки — ти радіо, не чат.";

static const char* rtVoice() {
    return providerIsXai() ? "sirius" : "ash";
}

constexpr uint32_t RATE = 24000;
constexpr size_t CHUNK = 240;
constexpr size_t CHUNK_BYTES = CHUNK * 2;
constexpr size_t RING_BYTES = 512 * 1024;
constexpr uint16_t PREBUF_N = 400;
constexpr uint16_t PRE_IDLE_N = 80;
constexpr size_t PREBUF_BYTES = (size_t)PREBUF_N * CHUNK_BYTES;
constexpr size_t PCM_DECODE_BYTES = 192 * 1024;
constexpr uint8_t MIC_SHIFT = 15;
constexpr int MIC_GAIN = 2;
constexpr int MIC_PEAK_MIN = 500;
constexpr uint32_t PTT_MIN_MS = 200;
constexpr uint32_t MAX_RECORD_MS = 15000;
constexpr uint8_t MIC_EMA_ALPHA = 72;

enum : uint8_t { ST_IDLE = 0, ST_RECORDING, ST_WAIT_RESP };

static I2SClass i2sSpk;
static I2SClass i2sMic;
static websockets::WebsocketsClient wsClient;

static uint8_t* ring = nullptr;
static uint8_t* pcmDecode = nullptr;
static uint8_t* prebuf = nullptr;
static volatile size_t head = 0, tail = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool wsReady = false;
static volatile bool sessionReady = false;
static volatile bool wantOnline = false;
static volatile bool hangupPending = false;
static volatile bool sessionArmed = false;
static volatile bool pttHeld = false;
static volatile bool s_ptt_armed = false;
static volatile bool s_need_mic_clear = false;
static volatile bool s_need_commit = false;
static volatile bool respPlaybackPending = false;
static volatile uint8_t convState = ST_IDLE;
static volatile uint32_t stateSinceMs = 0;
static volatile bool waitingACK = false;
static volatile bool responsePending = false;
static volatile uint32_t recMs = 0;
static volatile bool speaking = false;
static volatile int32_t recPeak = 0;
static volatile uint32_t recClip = 0;
static volatile uint32_t recSamples = 0;
static volatile bool serverCommitted = false;
static volatile bool commitWhenReady = false;
static uint16_t preHead = 0;
static uint16_t preN = 0;
static bool g_mic32bit = true;
static int32_t micSmoothPk = 0;

static volatile bool s_owns_spk = false;
static bool s_spk_begun = false;
static bool s_spk_stereo = true;
static volatile bool s_resume_radio = false;
static volatile bool s_giveback = false;
static volatile bool s_started = false;

static size_t rbFree() {
    return (tail - head - 1 + RING_BYTES) % RING_BYTES;
}
static size_t rbUsed() {
    return (head - tail + RING_BYTES) % RING_BYTES;
}

static volatile uint32_t s_tts_out_ms = 0;

static void ampMuteHw(bool mute) {
    if (RadioConfig::ampMutePin == 255) {
        return;
    }
    pinMode(RadioConfig::ampMutePin, OUTPUT);
    if (!RadioConfig::ampUseHardwareMute) {
        digitalWrite(RadioConfig::ampMutePin, RadioConfig::ampMuteWhenIdleHigh ? LOW : HIGH);
        return;
    }
    digitalWrite(RadioConfig::ampMutePin, mute == RadioConfig::ampMuteWhenIdleHigh ? HIGH : LOW);
}

static int pcmScaleFromRadioVol() {
    const int vmax = RadioConfig::ampVolumeUiMax;
    int v = (int)radioState.vol;
    if (v < 0) {
        v = 0;
    }
    if (v > vmax) {
        v = vmax;
    }
    if (vmax <= 0) {
        return 0;
    }
    // Як Audio::setVolume curve 0: gain = (vol/steps)^2; 256 = ×1.0.
    return (v * v * 256) / (vmax * vmax);
}

static void i2sWriteAll(const uint8_t* p, size_t n) {
    size_t off = 0;
    uint8_t spins = 0;
    while (off < n) {
        size_t w = i2sSpk.write(p + off, n - off);
        if (!w) {
            if (++spins > 50) {
                break;
            }
            delay(1);
            continue;
        }
        spins = 0;
        off += w;
    }
}

static void i2sOutMono(const uint8_t* buf, size_t n) {
    if (!s_spk_stereo) {
        i2sWriteAll(buf, n);
        return;
    }
    static uint8_t stereo[CHUNK_BYTES * 2];
    const size_t ns = n / 2;
    const size_t outn = ns * 4;
    if (outn > sizeof(stereo) || ns == 0) {
        return;
    }
    const int16_t* m = (const int16_t*)buf;
    int16_t* st = (int16_t*)stereo;
    for (size_t i = 0; i < ns; i++) {
        st[2 * i] = m[i];
        st[2 * i + 1] = m[i];
    }
    i2sWriteAll(stereo, outn);
}

static void spkWrite(uint8_t* buf, size_t n, bool from_pcm) {
    const int vol = pcmScaleFromRadioVol();
    if (n >= 2 && vol != 256) {
        int16_t* s = (int16_t*)buf;
        const size_t count = n / 2;
        for (size_t i = 0; i < count; i++) {
            int32_t x = ((int32_t)s[i] * vol) >> 8;
            if (x > 32767) {
                x = 32767;
            } else if (x < -32767) {
                x = -32767;
            }
            s[i] = (int16_t)x;
        }
    }
    if (from_pcm && n >= 2) {
        pcm_analyzer_on_bender_pcm16((const int16_t*)buf, (uint16_t)(n / 2));
        s_tts_out_ms = millis();
    }
    ampMuteHw(false);
    i2sOutMono(buf, n);
}

static void spkWriteSilence() {
    static uint8_t z[CHUNK_BYTES];
    memset(z, 0, CHUNK_BYTES);
    pcm_analyzer_on_bender_pcm16((const int16_t*)z, (uint16_t)(CHUNK_BYTES / 2));
    spkWrite(z, CHUNK_BYTES, false);
}

static bool takeSpeaker() {
    if (s_owns_spk) {
        return true;
    }
    if (strcmp(g_audio_source, "wifi") == 0) {
        audio.setVolume(0);
        if (audio.isRunning()) {
            audio.stopSong();
        }
        delay(50);
    }
    free_uart0_from_i2s_pins();
    audio.releaseI2SChannel();
    delay(20);
    if (s_spk_begun) {
        i2sSpk.end();
        s_spk_begun = false;
    }
    if (!i2sSpk.setPort(I2S_NUM_0)) {
        ALOGLN(F("[AI] I2S0 setPort fail"));
        audio_hw_init(false);
        return false;
    }
    i2sSpk.setPins(RadioConfig::i2sBclk, RadioConfig::i2sLrc, RadioConfig::i2sDout);
    bool ok = i2sSpk.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)16, I2S_SLOT_MODE_STEREO,
                           I2S_STD_SLOT_BOTH);
    s_spk_stereo = ok;
    if (!ok) {
        ok = i2sSpk.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)16, I2S_SLOT_MODE_MONO,
                          I2S_STD_SLOT_LEFT);
        s_spk_stereo = false;
    }
    if (!ok) {
        ALOGLN(F("[AI] I2S0 begin fail"));
        audio_hw_init(false);
        return false;
    }
    i2sSpk.setTimeout(1000);
    s_owns_spk = true;
    s_spk_begun = true;
    ampMuteHw(true);
    ALOG("[AI] I2S0 PCM 24k %s\n", s_spk_stereo ? "stereo" : "mono");
    return true;
}

static void releaseSpeakerFromLoop() {
    if (!s_owns_spk) {
        return;
    }
    ampMuteHw(true);
    if (s_spk_begun) {
        i2sSpk.end();
        s_spk_begun = false;
    }
    s_owns_spk = false;
    audio_hw_init(false);
    apply_output_volume();
    if (s_resume_radio && radioState.state && strcmp(g_audio_source, "wifi") == 0) {
        reconnect = station_url_for_current();
    }
    s_resume_radio = false;
    ALOGLN(F("[AI] I2S0 back to radio"));
}

static void requestGiveback() {
    s_giveback = true;
}

static void micResetSmooth() {
    micSmoothPk = 0;
}

static void resetTxState() {
    waitingACK = false;
    responsePending = false;
    sessionArmed = false;
    pttHeld = false;
    s_ptt_armed = false;
    s_need_mic_clear = false;
    s_need_commit = false;
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
        // Не віддавати I2S одразу: кільце порожнє, а DMA ще грає хвіст слова.
        return;
    }
    hangupPending = true;
}

static void doHangup() {
    hangupPending = false;
    wantOnline = providerIsLocal();
    sessionReady = false;
    wsReady = false;
    resetTxState();
    wsClient.close();
    requestGiveback();
}

static void micDump() {
    static uint8_t dump[1024];
    for (int i = 0; i < 8; i++) {
        i2sMic.readBytes((char*)dump, sizeof(dump));
    }
}

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

static void wsSendRaw(const char* s) {
    wsClient.send(s);
}

static void wsSend(const JsonDocument& j) {
    String s;
    serializeJson(j, s);
    wsClient.send(s);
}

static void sendMicAppend(const uint8_t* pcm) {
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
        return;
    }
    wsSendRaw(json);
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

static void preTrimIdle() {
    while (preN > PRE_IDLE_N) {
        preHead = (preHead + 1) % PREBUF_N;
        preN--;
    }
}

static void drainPrebufToWs(uint8_t max_n) {
    if (!sessionReady || !prebuf || !preN) {
        return;
    }
    uint8_t n = 0;
    while (preN && n < max_n) {
        const uint16_t idx = preHead % PREBUF_N;
        sendMicAppend(prebuf + (size_t)idx * CHUNK_BYTES);
        preHead = (preHead + 1) % PREBUF_N;
        preN--;
        n++;
    }
}

static void tryPttCommit();

static void pttStartCapture() {
    sessionArmed = true;
    serverCommitted = false;
    commitWhenReady = false;
    micResetSmooth();
    convState = ST_RECORDING;
    stateSinceMs = millis();
    ALOG("[PTT] REC queued=%u ms\n", (unsigned)preN * 10);
}

static void tryPttCommit() {
    if (convState != ST_RECORDING) {
        return;
    }
    if (!sessionReady) {
        commitWhenReady = true;
        ALOGLN(F("[PTT] wait session"));
        return;
    }
    const bool ok = (recMs >= PTT_MIN_MS) && (recPeak >= MIC_PEAK_MIN);
    convState = ST_WAIT_RESP;
    sessionArmed = false;
    pttHeld = false;
    stateSinceMs = millis();
    if (serverCommitted && ok) {
        JsonDocument r;
        r["type"] = "response.create";
        wsSend(r);
        waitingACK = false;
    } else {
        JsonDocument c;
        c["type"] = ok ? "input_audio_buffer.commit" : "input_audio_buffer.clear";
        wsSend(c);
        waitingACK = ok;
    }
    responsePending = ok;
    ALOG("[PTT] %s recMs=%u peak=%d\n", ok ? "COMMIT" : "CLEAR", (unsigned)recMs, (int)recPeak);
    resetRecStats();
    if (!ok) {
        convState = ST_IDLE;
        requestHangup();
    }
}

static void onSessionReadyConv() {
    if (convState != ST_RECORDING && !s_ptt_armed) {
        return;
    }
    while (preN) {
        drainPrebufToWs(16);
    }
    if (!pttHeld || commitWhenReady) {
        tryPttCommit();
    }
}

static size_t un64n(const char* s, size_t len, uint8_t* d, size_t cap) {
    size_t out = 0;
    if (!s || !len) {
        return 0;
    }
    return mbedtls_base64_decode(d, cap, &out, (const uint8_t*)s, len) ? 0 : out;
}

static void pushPcmBytes(const uint8_t* pcm, size_t n) {
    if (!pcm || !n || !ring) {
        return;
    }
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        ALOG("[AI] pcm in %u\n", (unsigned)n);
    }
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

static void pushPcmB64(const char* b64, size_t b64len) {
    if (!pcmDecode || !b64 || !b64len) {
        return;
    }
    size_t n = un64n(b64, b64len, pcmDecode, PCM_DECODE_BYTES);
    if (!n) {
        return;
    }
    pushPcmBytes(pcmDecode, n);
}

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

static void onMessage(websockets::WebsocketsMessage m) {
    if (!m.isText()) {
        return;
    }
    const char* payload = m.c_str();
    if (!payload) {
        return;
    }
    if (convState == ST_WAIT_RESP) {
        stateSinceMs = millis();
    }
    if (strstr(payload, "input_audio_transcription")) {
        return;
    }
    if (isAudioDeltaMsg(payload)) {
        size_t dlen = 0;
        const char* delta = jsonStringField(payload, "delta", &dlen);
        if (delta && dlen) {
            pushPcmB64(delta, dlen);
            vTaskDelay(0);
        }
        return;
    }
    JsonDocument j;
    if (deserializeJson(j, payload)) {
        return;
    }
    const char* t = j["type"] | "";
    if (strcmp(t, "session.created") == 0) {
        JsonDocument u;
        u["type"] = "session.update";
        JsonObject s = u["session"].to<JsonObject>();
        s["instructions"] = SYS_PROMPT;
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
        JsonObject audioo = s["audio"].to<JsonObject>();
        JsonObject inFmt = audioo["input"]["format"].to<JsonObject>();
        inFmt["type"] = "audio/pcm";
        inFmt["rate"] = 24000;
        JsonObject outFmt = audioo["output"]["format"].to<JsonObject>();
        outFmt["type"] = "audio/pcm";
        outFmt["rate"] = 24000;
        wsSend(u);
        ALOGLN(F("[WSS] session.update"));
    } else if (!strcmp(t, "session.updated") || !strcmp(t, "conversation.created")) {
        if (!sessionReady) {
            sessionReady = true;
            ALOGLN(F("[WSS] session ready"));
            onSessionReadyConv();
        }
    } else if (!strcmp(t, "input_audio_buffer.committed")) {
        serverCommitted = true;
        if (convState != ST_RECORDING) {
            waitingACK = false;
            JsonDocument r;
            r["type"] = "response.create";
            wsSend(r);
        }
    } else if (!strcmp(t, "response.created")) {
        speaking = true;
        if (convState == ST_WAIT_RESP) {
            stateSinceMs = millis();
        }
    } else if (!strcmp(t, "response.output_audio.done") || !strcmp(t, "response.audio.done")) {
        speaking = false;
    } else if (!strcmp(t, "response.done")) {
        speaking = false;
        waitingACK = false;
        responsePending = false;
        respPlaybackPending = true;
        convState = ST_IDLE;
        ALOGLN(F("[PTT] response.done"));
    } else if (!strcmp(t, "error")) {
        const char* msg = j["error"]["message"] | "";
        ALOG("[WSS] error: %s\n", msg);
    }
}

static void onEvent(websockets::WebsocketsEvent e, String) {
    if (e == websockets::WebsocketsEvent::ConnectionOpened) {
        wsReady = true;
        ALOGLN(F("[WSS] opened"));
    } else if (e == websockets::WebsocketsEvent::ConnectionClosed) {
        wsReady = false;
        sessionReady = false;
        waitingACK = false;
        responsePending = false;
        speaking = false;
        if (convState == ST_WAIT_RESP || convState == ST_RECORDING) {
            convState = ST_IDLE;
        }
        ALOGLN(F("[WSS] closed"));
        requestGiveback();
    } else if (e == websockets::WebsocketsEvent::GotPing) {
        wsClient.pong();
    }
}

static void speakerTask(void*) {
    static uint8_t buf[CHUNK_BYTES];
    bool primed = false;
    uint32_t quietSince = 0;
    for (;;) {
        if (!s_owns_spk) {
            primed = false;
            quietSince = 0;
            s_tts_out_ms = 0;
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }
        size_t avail = rbUsed();
        if (!primed) {
            const bool gotWholeReply = respPlaybackPending && avail >= CHUNK_BYTES;
            if (avail < CHUNK_BYTES && !gotWholeReply) {
                vTaskDelay(1);
                continue;
            }
            primed = true;
            quietSince = 0;
            ampMuteHw(false);
        }
        if (avail < CHUNK_BYTES) {
            if (avail >= 4) {
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
                spkWrite(buf, CHUNK_BYTES, true);
                continue;
            }
            if (speaking) {
                spkWriteSilence();
                continue;
            }
            if (!quietSince) {
                quietSince = millis();
            } else if (millis() - quietSince > 400) {
                ampMuteHw(true);
                quietSince = 0;
                primed = false;
                if (respPlaybackPending || convState == ST_IDLE) {
                    respPlaybackPending = false;
                    requestGiveback();
                }
            }
            spkWriteSilence();
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
        spkWrite(buf, CHUNK_BYTES, true);
    }
}

static void wsTask(void*) {
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
            ALOGLN(F("[WSS] connecting…"));
            sessionReady = false;
            if (!wsClient.connect(wsUrl())) {
                ALOG("[WSS] connect fail %s\n", wsUrl().c_str());
                backoff = minOfU32(backoff ? backoff * 2 : 500u, 8000u);
            } else {
                backoff = 0;
            }
            continue;
        }
        backoff = 0;
        wsClient.poll();
        if (millis() - lastPing > 30000) {
            wsClient.ping();
            lastPing = millis();
        }

        int32_t chunkPeak = 0;
        const bool capturing = s_ptt_armed || (convState == ST_RECORDING && pttHeld);
        if (s_need_mic_clear) {
            s_need_mic_clear = false;
            if (sessionReady) {
                JsonDocument cl;
                cl["type"] = "input_audio_buffer.clear";
                wsSend(cl);
            }
        }
        if (readMicChunk16(mic16, &chunkPeak, capturing)) {
            prePush(mic16);
            if (capturing) {
                recMs += 10;
                if (recMs >= MAX_RECORD_MS) {
                    ALOGLN(F("[PTT] max rec"));
                    s_need_commit = true;
                }
            } else {
                preTrimIdle();
            }
        }
        if (capturing && sessionReady) {
            drainPrebufToWs(8);
        }
        if (s_need_commit && convState == ST_RECORDING) {
            s_need_commit = false;
            while (preN) {
                drainPrebufToWs(16);
            }
            tryPttCommit();
        } else if (!capturing) {
            if (convState == ST_RECORDING && !pttHeld && sessionReady) {
                tryPttCommit();
            }
        }

        const uint32_t waitLim = providerIsLocal() ? 90000u : 20000u;
        if (convState == ST_WAIT_RESP && stateSinceMs && (millis() - stateSinceMs > waitLim)) {
            ALOGLN(F("[PTT] timeout"));
            waitingACK = false;
            responsePending = false;
            convState = ST_IDLE;
            stateSinceMs = 0;
            requestHangup();
        }

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

static bool initMic() {
    if (!i2sMic.setPort(I2S_NUM_1)) {
        ALOGLN(F("[AI] mic setPort fail"));
        return false;
    }
    i2sMic.setPins(RadioConfig::micBclkPin, RadioConfig::micWsPin, -1, RadioConfig::micDinPin);
    if (i2sMic.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)32, I2S_SLOT_MODE_MONO,
                     I2S_STD_SLOT_LEFT)) {
        g_mic32bit = true;
        ALOGLN(F("[AI] mic I2S1 32-bit"));
    } else if (i2sMic.begin(I2S_MODE_STD, RATE, (i2s_data_bit_width_t)16, I2S_SLOT_MODE_MONO,
                            I2S_STD_SLOT_LEFT)) {
        g_mic32bit = false;
        ALOGLN(F("[AI] mic I2S1 16-bit"));
    } else {
        ALOGLN(F("[AI] mic FAIL"));
        return false;
    }
    delay(50);
    micDump();
    return true;
}

bool bender_ai_busy() {
    return s_owns_spk || convState != ST_IDLE || speaking || waitingACK || responsePending ||
           (respPlaybackPending && rbUsed() > 0) || pttHeld;
}

bool bender_ai_recording() {
    return convState == ST_RECORDING;
}

bool bender_ai_owns_speaker() {
    return s_owns_spk;
}

bool bender_ai_tts_playing() {
    if (!s_owns_spk) {
        return false;
    }
    if (rbUsed() >= 4) {
        return true;
    }
    const uint32_t t = s_tts_out_ms;
    return t != 0 && (millis() - t) < 80u;
}

void bender_ai_ptt_arm() {
    if (convState == ST_WAIT_RESP || waitingACK || responsePending || convState == ST_RECORDING) {
        return;
    }
    s_need_mic_clear = false;
    s_ptt_armed = true;
    resetRecStats();
}

void bender_ai_ptt_cancel() {
    if (convState == ST_RECORDING || pttHeld) {
        return;
    }
    if (s_ptt_armed) {
        s_need_mic_clear = true;
    }
    s_ptt_armed = false;
    resetRecStats();
}

void bender_ai_ptt_down() {
    if (convState == ST_WAIT_RESP || waitingACK || responsePending) {
        ALOGLN(F("[PTT] wait answer"));
        return;
    }
    s_resume_radio = radioState.state && strcmp(g_audio_source, "wifi") == 0;
    if (!takeSpeaker()) {
        ALOGLN(F("[PTT] no speaker — TTS mute"));
    }
    pttHeld = true;
    wantOnline = true;
    hangupPending = false;
    if (speaking) {
        JsonDocument c;
        c["type"] = "response.cancel";
        wsSend(c);
        speaking = false;
        ampMuteHw(true);
        s_tts_out_ms = 0;
        portENTER_CRITICAL(&mux);
        head = tail = 0;
        portEXIT_CRITICAL(&mux);
    }
    respPlaybackPending = false;
    pcm_analyzer_reset();
    pttStartCapture();
    s_ptt_armed = false;
}

void bender_ai_ptt_up() {
    pttHeld = false;
    s_ptt_armed = false;
    if (convState == ST_RECORDING) {
        ALOGLN(F("[PTT] release"));
        s_need_commit = true;
    }
}

void bender_ai_tick() {
    if (s_giveback && !pttHeld && convState != ST_RECORDING && convState != ST_WAIT_RESP &&
        !speaking && rbUsed() == 0) {
        s_giveback = false;
        releaseSpeakerFromLoop();
    }
}

void bender_ai_begin() {
    if (s_started) {
        return;
    }
    ring = (uint8_t*)ps_malloc(RING_BYTES);
    pcmDecode = (uint8_t*)ps_malloc(PCM_DECODE_BYTES);
    prebuf = (uint8_t*)ps_malloc(PREBUF_BYTES);
    if (!ring || !pcmDecode || !prebuf) {
        ALOGLN(F("[AI] PSRAM alloc fail"));
        return;
    }
    if (!initMic()) {
        return;
    }
    if (providerIsLocal()) {
        wantOnline = true;
        ALOG("[WSS] local %s\n", wsUrl().c_str());
    } else {
        wsClient.setInsecure();
        wsClient.addHeader("Authorization", String("Bearer ") + apiKey());
    }
    wsClient.onEvent(onEvent);
    wsClient.onMessage(onMessage);
    xTaskCreatePinnedToCore(speakerTask, "ai_spk", 4096, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(wsTask, "ai_ws", 16384, nullptr, 3, nullptr, 1);
    s_started = true;
    ALOGLN(F("[AI] PTT hold=talk  7 clicks=sleep  8=restart"));
}
