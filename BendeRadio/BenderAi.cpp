#include "BenderAi.h"

#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include <atomic>
#include <esp_wifi.h>
#include <mbedtls/base64.h>

#include "AirPlay.h"
#include "NvsConfig.h"
#include "RadioConfig.h"
#include "core0.h"
#include "pcm_analyzer.h"
#include "Pcm16.h"
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
static String defaultLocalWsUrl() {
    String u = LOCAL_WS_USE_TLS ? String("wss://") : String("ws://");
    u += LOCAL_WS_HOST;
    const bool omitPort = LOCAL_WS_USE_TLS && ((int)LOCAL_WS_PORT == 443);
    if (!omitPort) {
        u += ":";
        u += String(LOCAL_WS_PORT);
    }
    u += "/v1/realtime";
    return u;
}

static String publicWsUrl() {
#ifdef LOCAL_WS_PUBLIC_HOST
    const char* host = LOCAL_WS_PUBLIC_HOST;
    if (host == nullptr || host[0] == '\0') {
        return String();
    }
    String u = "wss://";
    u += host;
    u += "/v1/realtime";
    return u;
#else
    return String();
#endif
}

static uint32_t s_lan_chk_ms = 0;
static bool s_lan_ok = false;

static bool lanHostReachable() {
    if (WiFi.status() != WL_CONNECTED) {
        s_lan_ok = false;
        return false;
    }
    if (s_lan_chk_ms && (uint32_t)(millis() - s_lan_chk_ms) < 15000u) {
        return s_lan_ok;
    }
    s_lan_chk_ms = millis();
    WiFiClient c;
    c.setTimeout(2);
    s_lan_ok = c.connect(LOCAL_WS_HOST, (uint16_t)LOCAL_WS_PORT);
    c.stop();
    return s_lan_ok;
}

static String wsUrl() {
    if (providerIsLocal()) {
        if (lanHostReachable()) {
            return defaultLocalWsUrl();
        }
        String pub = publicWsUrl();
        if (pub.length()) {
            return pub;
        }
        String ov;
        nvsLoadAiWsUrl(ov);
        if (ov.length()) {
            return ov;
        }
        return defaultLocalWsUrl();
    }
    String u = "wss://";
    u += apiHost();
    u += "/v1/realtime?model=";
    u += realtimeModel();
    return u;
}

static bool wsUrlUsesTls(const String& u) {
    return u.startsWith("wss://");
}

static String localBasicAuthHeader() {
    const String raw = String(LOCAL_WS_USER) + ":" + String(LOCAL_WS_PASS);
    size_t outLen = 0;
    const size_t cap = ((raw.length() + 2u) / 3u) * 4u + 8u;
    unsigned char* out = (unsigned char*)malloc(cap);
    if (!out) {
        return String();
    }
    const int rc = mbedtls_base64_encode(out, cap, &outLen, (const unsigned char*)raw.c_str(),
                                         raw.length());
    String hdr;
    if (rc == 0 && outLen > 0) {
        hdr = "Basic ";
        hdr += String((const char*)out, (unsigned)outLen);
    }
    free(out);
    return hdr;
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
static_assert(RING_BYTES % 2 == 0 && CHUNK_BYTES % 2 == 0, "PCM16 ring and chunks must be sample-aligned");
constexpr uint16_t PREBUF_N = 1700;
constexpr uint16_t PRE_IDLE_N = 80;
constexpr size_t PREBUF_BYTES = (size_t)PREBUF_N * CHUNK_BYTES;
constexpr size_t PCM_DECODE_BYTES = 192 * 1024;
constexpr uint8_t MIC_SHIFT = RadioConfig::micPcmShiftRight;
constexpr int MIC_GAIN = RadioConfig::micDigitalGain;
constexpr int MIC_PEAK_MIN = 500;
constexpr uint32_t PTT_MIN_MS = 200;
constexpr uint32_t MAX_RECORD_MS = 15000;
constexpr uint8_t MIC_EMA_ALPHA = 72;
// 10 мс/чанк. Натискання — коротко (інакше з'їдає перші літери). Відпускання — довший хвіст.
// Після idle-преролу: клац натискання (~80 мс). Хвіст — коротко, користувач чує старт.
constexpr uint8_t PTT_DROP_HEAD = 8;
constexpr uint8_t PTT_DROP_TAIL = 4;

enum : uint8_t { ST_IDLE = 0, ST_RECORDING, ST_WAIT_RESP };

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
static volatile bool s_ai_awake = false;
static uint32_t s_ai_last_live_ms = 0;
static bool s_mic_on = false;
static volatile bool hangupPending = false;
static volatile bool sessionArmed = false;
static volatile bool pttHeld = false;
static volatile bool s_ptt_armed = false;
static std::atomic<uint32_t> s_favorites_lo{0}, s_favorites_hi{0};
static std::atomic<bool> s_event_voice_enabled{true};
static std::atomic<uint32_t> s_pending_event{0};
static std::atomic<uint32_t> s_pending_event_ms{0};
static volatile bool s_event_reply = false;
static volatile bool s_drop_event_reply = false;
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
static volatile uint16_t s_preN_at_arm = 0;
static char s_voice_cmd[24] = {0};
static volatile int s_voice_station = -1;
static volatile bool s_voice_pending = false;
static bool g_mic32bit = true;
static int32_t micSmoothPk = 0;

static volatile bool s_owns_spk = false;
static volatile bool s_taking = false;
static bool s_spk_stereo = true;
static uint16_t s_take_n = 0;
static volatile bool s_resume_radio = false;
static volatile bool s_giveback = false;
static volatile bool s_need_speaker = false;
static volatile bool s_started = false;
static volatile uint8_t s_demo = 0;
static uint8_t* s_demo_pcm = nullptr;
static size_t s_demo_cap = 0;
static volatile uint32_t s_progress_ms = 0;
static volatile uint32_t s_last_pcm_ms = 0;

static bool s_ai_debug = false;
static uint8_t* s_dbg_pcm = nullptr;
static size_t s_dbg_cap = 0;
static size_t s_dbg_len = 0;
static volatile bool s_dbg_play = false;

static size_t rbFree() {
    return (tail - head - 1 + RING_BYTES) % RING_BYTES;
}
static size_t rbUsed() {
    return (head - tail + RING_BYTES) % RING_BYTES;
}

static volatile uint32_t s_tts_out_ms = 0;
static std::atomic<uint32_t> s_face_error_ms{0};

static void showFaceError() {
    const uint32_t now = millis();
    s_face_error_ms.store(now ? now : UINT32_MAX, std::memory_order_relaxed);
}

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
        // Пишемо в канал Audio, без I2SClass begin/end — той після 3–4 фраз вбиває I2S0.
        size_t w = audio.i2sWriteRaw(p + off, n - off, 80);
        if (!w) {
            if (++spins > 80) {
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

static void i2sWriteSilenceChunks(uint8_t n) {
    static uint8_t stereo[CHUNK_BYTES * 2];
    memset(stereo, 0, sizeof(stereo));
    for (uint8_t i = 0; i < n; i++) {
        i2sWriteAll(stereo, sizeof(stereo));
    }
}

static void spkWrite(uint8_t* buf, size_t n, bool from_pcm) {
    AirPlayAudioGuard guard;
    if (!s_owns_spk) return;
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
    AirPlayAudioGuard guard;
    if (s_owns_spk) {
        return true;
    }
    if (airplay_owns_speaker()) return false;
    s_taking = true;
    s_need_speaker = false;
    delay(8);
    if (strcmp(g_audio_source, "wifi") == 0) {
        audio.setVolume(0);
        if (audio.isRunning()) {
            audio.stopSong();
        }
        delay(20);
    }
    free_uart0_from_i2s_pins();
    if (!audio.i2sSetSampleRateHz(RATE)) {
        ALOGLN(F("[AI] I2S0 rate 24k fail"));
        s_taking = false;
        return false;
    }
    s_spk_stereo = true;
    ampMuteHw(true);
    s_owns_spk = true;
    s_taking = false;
    s_take_n++;
    ALOG("[AI] I2S0 PCM 24k Audio n=%u heap=%u\n", (unsigned)s_take_n, (unsigned)ESP.getFreeHeap());
    return true;
}

static void releaseSpeakerFromLoop() {
    AirPlayAudioGuard guard;
    if (!s_owns_spk && !s_taking) {
        return;
    }
    ampMuteHw(true);
    i2sWriteSilenceChunks(12);
    s_owns_spk = false;
    s_taking = false;
    s_need_speaker = false;
    if (!audio.i2sSetSampleRateHz(44100)) {
        ALOGLN(F("[AI] I2S0 rate 44k fail"));
    }
    apply_output_volume();
    if (radioState.state && strcmp(g_audio_source, "wifi") == 0) {
        reconnect = station_url_for_current();
    }
    s_resume_radio = false;
    ALOG("[AI] I2S0 back to radio n=%u heap=%u\n", (unsigned)s_take_n, (unsigned)ESP.getFreeHeap());
}

static void requestGiveback() {
    s_giveback = true;
}

static void noteProgress() {
    s_progress_ms = millis();
}

static uint32_t s_sock_wait_ms;

static void forceRecover(const char* why, bool show_error = false) {
    const bool background_event = s_event_reply || s_drop_event_reply || s_pending_event.load() != 0;
    const bool user_active = pttHeld || s_ptt_armed || convState == ST_RECORDING;
    if (bender_face_show_error(show_error, background_event, user_active)) {
        showFaceError();
    }
    if (background_event) {
        s_pending_event.store(0);
        s_event_reply = false;
        s_drop_event_reply = true;
    }
    ALOG("[AI] recover %s heap=%u\n", why ? why : "?", (unsigned)ESP.getFreeHeap());
    waitingACK = false;
    responsePending = false;
    sessionArmed = false;
    respPlaybackPending = false;
    convState = ST_IDLE;
    stateSinceMs = 0;
    s_sock_wait_ms = 0;
    s_need_commit = false;
    speaking = false;
    s_tts_out_ms = 0;
    s_last_pcm_ms = 0;
    s_dbg_play = false;
    s_dbg_len = 0;
    portENTER_CRITICAL(&mux);
    head = tail = 0;
    portEXIT_CRITICAL(&mux);
    requestGiveback();
}

static void queueVoiceCmd(const char* name, int station) {
    if (!name || !name[0]) {
        return;
    }
    strncpy(s_voice_cmd, name, sizeof(s_voice_cmd) - 1);
    s_voice_cmd[sizeof(s_voice_cmd) - 1] = 0;
    s_voice_station = station;
    s_voice_pending = true;
    ALOG("[AI] cmd %s station=%d\n", s_voice_cmd, station);
}

static bool applyVoiceCmdState() {
    if (!s_voice_pending) {
        return false;
    }
    s_voice_pending = false;
    const char* n = s_voice_cmd;
    const int st = s_voice_station;
    if (!strcmp(n, "radio.off")) {
        radio_voice_set_state(false, -1);
    } else if (!strcmp(n, "radio.on")) {
        radio_voice_set_state(true, st);
    } else if (!strcmp(n, "radio.station")) {
        radio_voice_set_state(true, st);
    } else if (!strcmp(n, "radio.next")) {
        int i = (int)radioState.station + 1;
        const int nst = (int)radio_station_count();
        if (nst > 0 && i >= nst) {
            i = 0;
        }
        radio_voice_set_state(true, i);
    } else if (!strcmp(n, "radio.prev")) {
        int i = (int)radioState.station - 1;
        const int nst = (int)radio_station_count();
        if (i < 0) {
            i = nst > 0 ? nst - 1 : 0;
        }
        radio_voice_set_state(true, i);
    } else {
        ALOG("[AI] cmd unknown %s\n", n);
        s_voice_cmd[0] = 0;
        s_voice_station = -1;
        return false;
    }
    s_voice_cmd[0] = 0;
    s_voice_station = -1;
    return true;
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
    s_sock_wait_ms = 0;
    recMs = 0;
    recPeak = 0;
    recClip = 0;
    recSamples = 0;
    serverCommitted = false;
    commitWhenReady = false;
    preN = 0;
    preHead = 0;
    s_preN_at_arm = 0;
    s_dbg_play = false;
    s_dbg_len = 0;
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
    wantOnline = false;
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

constexpr size_t APPEND_CHUNKS = 10;
constexpr size_t APPEND_BYTES = APPEND_CHUNKS * CHUNK_BYTES;

static void sendMicAppendBytes(const uint8_t* pcm, size_t nbytes) {
    if (!pcm || !nbytes) {
        return;
    }
    static char b64[(((APPEND_BYTES + 2) / 3) * 4) + 8];
    static char json[80 + sizeof(b64)];
    size_t out = 0;
    if (mbedtls_base64_encode((unsigned char*)b64, sizeof(b64), &out, pcm, nbytes) != 0) {
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

static void dbgCaptureClear() {
    s_dbg_len = 0;
    s_dbg_play = false;
}

static void dbgCaptureAppend(const uint8_t* pcm, size_t nbytes) {
    if (!s_ai_debug || !s_dbg_pcm || !pcm || !nbytes) {
        return;
    }
    size_t room = (s_dbg_cap > s_dbg_len) ? (s_dbg_cap - s_dbg_len) : 0;
    if (nbytes > room) {
        nbytes = room;
    }
    if (!nbytes) {
        return;
    }
    memcpy(s_dbg_pcm + s_dbg_len, pcm, nbytes);
    s_dbg_len += nbytes;
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
    static uint8_t pack[APPEND_BYTES];
    uint8_t n = 0;
    while (preN && n < max_n) {
        uint8_t got = 0;
        while (preN && got < APPEND_CHUNKS && n < max_n) {
            const uint16_t idx = preHead % PREBUF_N;
            memcpy(pack + (size_t)got * CHUNK_BYTES, prebuf + (size_t)idx * CHUNK_BYTES, CHUNK_BYTES);
            preHead = (preHead + 1) % PREBUF_N;
            preN--;
            got++;
            n++;
        }
        sendMicAppendBytes(pack, (size_t)got * CHUNK_BYTES);
        dbgCaptureAppend(pack, (size_t)got * CHUNK_BYTES);
    }
}

static void tryPttCommit();

static void pttStartCapture() {
    sessionArmed = true;
    serverCommitted = false;
    commitWhenReady = false;
    micResetSmooth();
    dbgCaptureClear();
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
        if (!s_sock_wait_ms) {
            s_sock_wait_ms = millis();
        }
        ALOGLN(F("[PTT] wait session"));
        return;
    }
    const bool ok = (recMs >= PTT_MIN_MS) && (recPeak >= MIC_PEAK_MIN);
    convState = ST_WAIT_RESP;
    sessionArmed = false;
    pttHeld = false;
    stateSinceMs = millis();
    noteProgress();
    if (serverCommitted && ok) {
        JsonDocument r;
        r["type"] = "response.create";
        if (providerIsLocal()) r["current_station"] = (int)radioState.station;
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
        showFaceError();  // Too short/quiet: acknowledge the rejected recording visually.
        convState = ST_IDLE;
        requestHangup();
    }
}

static void onSessionReadyConv() {
    if (convState != ST_RECORDING && !s_ptt_armed) {
        return;
    }
    if (!pttHeld || commitWhenReady) {
        while (preN) {
            drainPrebufToWs(40);
            wsClient.poll();
        }
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
    if (n != pcm16_whole_bytes(n)) {
        forceRecover("partial PCM16 sample", true);
        requestHangup();
        return;
    }
    if (!s_owns_spk) {
        s_need_speaker = true;
    }
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        ALOG("[AI] pcm in %u\n", (unsigned)n);
    }
    size_t idx = 0;
    uint8_t waits = 0;
    while (idx < n) {
        portENTER_CRITICAL(&mux);
        size_t freeb = rbFree();
        size_t chunk = pcm16_whole_bytes(minOf(freeb, n - idx));
        if (chunk) {
            size_t first = minOf(chunk, RING_BYTES - head);
            memcpy(ring + head, pcm + idx, first);
            memcpy(ring, pcm + idx + first, chunk - first);
            head = (head + chunk) % RING_BYTES;
            idx += chunk;
        }
        portEXIT_CRITICAL(&mux);
        if (idx < n) {
            if (++waits > 40) {
                ALOG("[AI] ring full — drop %u whole-sample bytes\n", (unsigned)(n - idx));
                break;
            }
            vTaskDelay(1);
        }
    }
    speaking = true;
    s_last_pcm_ms = millis();
    noteProgress();
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
    if (strstr(payload, "input_audio_transcription")) {
        return;
    }
    if (isAudioDeltaMsg(payload)) {
        if (s_drop_event_reply) return;
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
    if (s_drop_event_reply && (!strcmp(t, "response.created") || !strcmp(t, "response.output_audio.done") ||
                              !strcmp(t, "response.audio.done") || !strcmp(t, "response.done"))) {
        if (!strcmp(t, "response.done")) s_drop_event_reply = false;
        return;
    }
    if (strcmp(t, "session.created") == 0) {
        JsonDocument u;
        u["type"] = "session.update";
        JsonObject s = u["session"].to<JsonObject>();
        if (providerIsLocal()) s["current_station"] = (int)radioState.station;
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
        JsonArray stArr = s["stations"].to<JsonArray>();
        const uint8_t nst = radio_station_count();
        for (uint8_t i = 0; i < nst; i++) {
            JsonObject o = stArr.add<JsonObject>();
            o["id"] = i;
            o["name"] = radio_station_name(i);
        }
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
        noteProgress();
        if (convState != ST_RECORDING) {
            waitingACK = false;
            JsonDocument r;
            r["type"] = "response.create";
            if (providerIsLocal()) r["current_station"] = (int)radioState.station;
            wsSend(r);
        }
    } else if (!strcmp(t, "response.created")) {
        // Не тримати I2S тишею на весь STT/LLM — інакше радіо «мертве» до відповіді.
        noteProgress();
        if (convState == ST_WAIT_RESP) {
            stateSinceMs = millis();
        }
    } else if (!strcmp(t, "response.output_audio.done") || !strcmp(t, "response.audio.done")) {
        speaking = false;
        noteProgress();
    } else if (!strcmp(t, "response.done")) {
        s_event_reply = false;
        speaking = false;
        waitingACK = false;
        responsePending = false;
        respPlaybackPending = true;
        convState = ST_IDLE;
        noteProgress();
        ALOGLN(F("[PTT] response.done"));
    } else if (!strcmp(t, "pong")) {
        // keepalive під час STT/LLM — не подовжує wait-timeout
    } else if (!strcmp(t, "error")) {
        const char* msg = j["error"]["message"] | "";
        ALOG("[WSS] error: %s\n", msg);
        forceRecover("ws error", true);
    } else if (!strcmp(t, "device.profile") && providerIsLocal()) {
        uint32_t lo = 0, hi = 0;
        for (JsonVariant id : j["favorites"].as<JsonArray>()) {
            const int i = id.as<int>();
            if (i >= 0 && i < 32) lo |= uint32_t(1) << i;
            else if (i >= 32 && i < 64) hi |= uint32_t(1) << (i - 32);
        }
        s_favorites_lo.store(lo);
        s_favorites_hi.store(hi);
        s_event_voice_enabled.store(j["event_voice"] | true);
    } else if (!strcmp(t, "device.command")) {
        const char* name = j["name"] | "";
        const int st = j["args"]["station"] | -1;
        queueVoiceCmd(name, st);
    }
}

static void onEvent(websockets::WebsocketsEvent e, String) {
    if (e == websockets::WebsocketsEvent::ConnectionOpened) {
        wsReady = true;
        ALOGLN(F("[WSS] opened"));
    } else if (e == websockets::WebsocketsEvent::ConnectionClosed) {
        // Closing an idle/prewarmed socket is normal, not a failed conversation.
        const bool interrupted = convState != ST_IDLE || pttHeld || waitingACK || responsePending || speaking;
        wsReady = false;
        sessionReady = false;
        ALOGLN(F("[WSS] closed"));
        forceRecover("ws closed", interrupted);
        s_event_reply = s_drop_event_reply = false;
        s_pending_event.store(0);
    } else if (e == websockets::WebsocketsEvent::GotPing) {
        wsClient.pong();
    }
}

static void speakerTask(void*) {
    static uint8_t buf[CHUNK_BYTES];
    bool primed = false;
    uint32_t quietSince = 0;
    uint8_t starve = 0;
    for (;;) {
        if (!s_owns_spk || s_taking) {
            primed = false;
            quietSince = 0;
            s_tts_out_ms = 0;
            vTaskDelay(s_ai_awake ? 10 : 80);
            continue;
        }
        if (s_demo == 1 || s_demo == 2 || convState == ST_RECORDING || pttHeld) {
            // Як у mic demo: під час запису не писати в I2S і тримати mute.
            // Інакше тиша/unmute дає тріск у колонку і він лізе в мікрофон.
            primed = false;
            quietSince = 0;
            ampMuteHw(true);
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }
        portENTER_CRITICAL(&mux);
        size_t avail = rbUsed();
        portEXIT_CRITICAL(&mux);
        if (!primed) {
            const bool gotWholeReply = respPlaybackPending && avail >= 2;
            const size_t minStart = CHUNK_BYTES * 16;
            if (avail < minStart && !gotWholeReply) {
                ampMuteHw(true);
                vTaskDelay(1);
                continue;
            }
            ampMuteHw(false);
            i2sWriteSilenceChunks(16);
            primed = true;
            quietSince = 0;
        }
        if (avail < CHUNK_BYTES) {
            if (avail >= 2) {
                portENTER_CRITICAL(&mux);
                size_t take = pcm16_whole_bytes(minOf(rbUsed(), (size_t)CHUNK_BYTES));
                size_t first = minOf(take, RING_BYTES - tail);
                memcpy(buf, ring + tail, first);
                if (first < take) {
                    memcpy(buf + first, ring, take - first);
                }
                tail = (tail + take) % RING_BYTES;
                portEXIT_CRITICAL(&mux);
                if (!take) {
                    vTaskDelay(1);
                    continue;
                }
                memset(buf + take, 0, CHUNK_BYTES - take);
                spkWrite(buf, CHUNK_BYTES, true);
                continue;
            }
            if (speaking) {
                if (s_last_pcm_ms && (millis() - s_last_pcm_ms > 800)) {
                    speaking = false;
                    starve = 0;
                } else if (++starve < 10) {
                    vTaskDelay(2 / portTICK_PERIOD_MS);
                    continue;
                } else {
                    starve = 0;
                    spkWriteSilence();
                    continue;
                }
            }
            if (!quietSince) {
                quietSince = millis();
            } else if (millis() - quietSince > 350) {
                ampMuteHw(true);
                quietSince = 0;
                primed = false;
                const bool still_wait = (convState == ST_WAIT_RESP) || responsePending || waitingACK;
                const bool pcm_fresh = s_last_pcm_ms && (millis() - s_last_pcm_ms < 700);
                if (!still_wait && !pcm_fresh &&
                    (respPlaybackPending || convState == ST_IDLE)) {
                    respPlaybackPending = false;
                    requestGiveback();
                }
            }
            spkWriteSilence();
            continue;
        }
        quietSince = 0;
        starve = 0;
        portENTER_CRITICAL(&mux);
        // A recovery/new recording may have reset the ring after the snapshot.
        // Advancing tail on an empty ring would manufacture almost a full ring
        // of stale audio and keep playing noise on subsequent responses.
        if (rbUsed() < CHUNK_BYTES) {
            portEXIT_CRITICAL(&mux);
            vTaskDelay(1);
            continue;
        }
        size_t first = minOf((size_t)CHUNK_BYTES, RING_BYTES - tail);
        memcpy(buf, ring + tail, first);
        if (first < CHUNK_BYTES) {
            memcpy(buf + first, ring, CHUNK_BYTES - first);
        }
        tail = (tail + CHUNK_BYTES) % RING_BYTES;
        portEXIT_CRITICAL(&mux);
        spkWrite(buf, CHUNK_BYTES, true);
        vTaskDelay(0);
    }
}

static void wsTask(void*) {
    static uint8_t mic16[CHUNK_BYTES];
    uint32_t lastPing = millis();
    uint32_t backoff = 0;
    for (;;) {
        if (s_demo == 1 && s_demo_pcm && s_demo_cap >= CHUNK_BYTES) {
            ampMuteHw(true);
            micDump();
            size_t filled = 0;
            int32_t pk = 0;
            uint64_t acc = 0;
            uint32_t ns = 0;
            const uint32_t t0 = millis();
            s_demo = 2;
            ALOGLN(F("[MIC] demo REC 3s — говори"));
            while (filled + CHUNK_BYTES <= s_demo_cap) {
                int32_t chunkPk = 0;
                if (!readMicChunk16(mic16, &chunkPk, false)) {
                    vTaskDelay(1);
                    continue;
                }
                memcpy(s_demo_pcm + filled, mic16, CHUNK_BYTES);
                filled += CHUNK_BYTES;
                if (chunkPk > pk) {
                    pk = chunkPk;
                }
                const int16_t* s = (const int16_t*)mic16;
                for (size_t i = 0; i < CHUNK; i++) {
                    int32_t a = s[i] < 0 ? -s[i] : s[i];
                    acc += (uint32_t)a;
                    ns++;
                }
            }
            const uint32_t rms = ns ? (uint32_t)(acc / ns) : 0;
            ALOG("[MIC] demo peak=%d rms=%u ms=%u\n", (int)pk, (unsigned)rms, (unsigned)(millis() - t0));
            pushPcmBytes(s_demo_pcm, filled);
            speaking = false;
            s_demo = 3;
            ALOGLN(F("[MIC] demo PLAY"));
        }
        if (s_demo) {
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }
        if (!s_ai_awake && !s_demo && !wantOnline) {
            backoff = 0;
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }
        if (!wsReady) {
            if (!wantOnline) {
                backoff = 0;
                vTaskDelay(100 / portTICK_PERIOD_MS);
                continue;
            }
            if (backoff) {
                vTaskDelay(backoff / portTICK_PERIOD_MS);
            }
            ALOG("[WSS] connecting… heap=%u\n", (unsigned)ESP.getFreeHeap());
            sessionReady = false;
            wsClient.close();
            const String url = wsUrl();
            if (wsUrlUsesTls(url)) {
                wsClient.setInsecure();
            }
            if (!wsClient.connect(url)) {
                wsClient.close();
                ALOG("[WSS] connect fail %s heap=%u\n", url.c_str(),
                     (unsigned)ESP.getFreeHeap());
                backoff = minOfU32(backoff ? backoff * 2 : 500u, 8000u);
                if (!pttHeld && (s_owns_spk || convState != ST_IDLE || s_taking)) {
                    forceRecover("connect fail", true);
                }
            } else {
                backoff = 0;
            }
            continue;
        }
        backoff = 0;
        const uint32_t pendingEvent = s_pending_event.load();
        if (pendingEvent && (pttHeld || s_ptt_armed || convState != ST_IDLE ||
                             uint32_t(millis() - s_pending_event_ms.load()) > 4000u)) {
            s_pending_event.store(0);
        } else if (pendingEvent && (!bender_event_radio_allows_voice(
                                       static_cast<BenderEvent>(pendingEvent & 0xff), radioState.state) ||
                                   radioState.vol <= 0 || !s_event_voice_enabled.load() ||
                                   strcmp(g_audio_source, "wifi") != 0 || airplay_owns_speaker())) {
            s_pending_event.store(0);
        } else if (pendingEvent && sessionReady && !bender_ai_busy()) {
            s_pending_event.store(0);
            JsonDocument event;
            event["type"] = "device.event";
            event["name"] = bender_event_name(static_cast<BenderEvent>(pendingEvent & 0xff));
            event["station"] = int(pendingEvent >> 8) - 1;
            s_event_reply = true;
            responsePending = true;
            convState = ST_WAIT_RESP;
            stateSinceMs = millis();
            noteProgress();
            wsSend(event);
        }
        const bool capturing = s_ptt_armed || (convState == ST_RECORDING && pttHeld);
        if (!capturing) {
            wsClient.poll();
            if (millis() - lastPing > 30000) {
                wsClient.ping();
                lastPing = millis();
            }
        } else if (recMs && (recMs % 200u) == 0) {
            wsClient.poll();
        }

        int32_t chunkPeak = 0;
        if (s_need_mic_clear) {
            s_need_mic_clear = false;
            if (sessionReady) {
                JsonDocument cl;
                cl["type"] = "input_audio_buffer.clear";
                wsSend(cl);
            }
        }
        const bool freeze_rec = (convState == ST_RECORDING) && s_need_commit;
        const bool keep_rec = capturing && !freeze_rec;
        if (capturing || s_ptt_armed) {
            if (readMicChunk16(mic16, &chunkPeak, keep_rec)) {
                if (keep_rec) {
                    prePush(mic16);
                    recMs += 10;
                    if (recMs >= MAX_RECORD_MS) {
                        ALOGLN(F("[PTT] max rec"));
                        s_need_commit = true;
                    }
                } else if (!freeze_rec) {
                    prePush(mic16);
                    preTrimIdle();
                }
            }
        }
        if (s_need_commit && convState == ST_RECORDING) {
            s_need_commit = false;
            // Idle до натискання (~0.8 с) + клац кнопки. Різати лише 30 мс з голови
            // не чіпало клац — він сидів після преролу, replay починався з нього.
            uint16_t drop_head = (uint16_t)s_preN_at_arm + (uint16_t)PTT_DROP_HEAD;
            s_preN_at_arm = 0;
            if (preN > 40) {
                while (drop_head && preN > 28) {
                    preHead = (preHead + 1) % PREBUF_N;
                    preN--;
                    drop_head--;
                }
                uint8_t drop_tail = PTT_DROP_TAIL;
                while (drop_tail && preN > 24) {
                    preN--;
                    drop_tail--;
                }
            }
            const uint32_t queued_ms = (uint32_t)preN * 10u;
            const uint32_t t0 = millis();
            while (preN) {
                drainPrebufToWs(40);
                wsClient.poll();
            }
            ALOG("[PTT] flush %u ms audio in %u ms\n", (unsigned)queued_ms, (unsigned)(millis() - t0));
            tryPttCommit();
        }

        const uint32_t waitLim = providerIsLocal() ? 45000u : 20000u;
        if (convState == ST_WAIT_RESP && s_progress_ms && (millis() - s_progress_ms > waitLim)) {
            ALOGLN(F("[PTT] timeout"));
            forceRecover("wait timeout", true);
            requestHangup();
        }
        if (s_sock_wait_ms && !sessionReady && !pttHeld &&
            (uint32_t)(millis() - s_sock_wait_ms) > 5000u) {
            ALOGLN(F("[PTT] no socket — radio back"));
            forceRecover("no socket", true);
        }

        if (respPlaybackPending && rbUsed() == 0 && !speaking) {
            s_dbg_play = false;
            respPlaybackPending = false;
            convState = ST_IDLE;
            requestHangup();
        }

        const bool busy = convState == ST_RECORDING || convState == ST_WAIT_RESP || waitingACK ||
                          responsePending || speaking || (respPlaybackPending && rbUsed() > 0) ||
                          s_dbg_play;
        if (hangupPending && wantOnline && !busy && !pttHeld) {
            doHangup();
        }
        if (!capturing) {
            vTaskDelay(1);
        }
    }
}

static void micSleep() {
    if (!s_mic_on) {
        return;
    }
    i2sMic.end();
    s_mic_on = false;
    ALOGLN(F("[AI] mic sleep"));
}

static bool initMic() {
    if (s_mic_on) {
        return true;
    }
    static bool s_mic_port = false;
    if (!s_mic_port) {
        if (!i2sMic.setPort(I2S_NUM_1)) {
            ALOGLN(F("[AI] mic setPort fail"));
            return false;
        }
        i2sMic.setPins(RadioConfig::micBclkPin, RadioConfig::micWsPin, -1, RadioConfig::micDinPin);
        s_mic_port = true;
    }
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
    s_mic_on = true;
    return true;
}

bool bender_ai_busy() {
    return s_demo || s_ptt_armed || s_taking || s_owns_spk || convState != ST_IDLE || speaking || waitingACK || responsePending ||
           (respPlaybackPending && rbUsed() > 0) || pttHeld || s_dbg_play;
}

bool bender_ai_favorite_station(int station) {
    if (station < 0 || station >= 64) return false;
    return station < 32 ? (s_favorites_lo.load() & (uint32_t(1) << station)) != 0
                        : (s_favorites_hi.load() & (uint32_t(1) << (station - 32))) != 0;
}

bool bender_ai_event_voice_enabled() { return s_event_voice_enabled.load(); }

bool bender_ai_event(BenderEvent event, int station) {
    AirPlayAudioGuard guard;
    if (!providerIsLocal() || !s_started || !s_event_voice_enabled.load() ||
        bender_ai_busy() || airplay_owns_speaker() || s_ptt_armed || WiFi.status() != WL_CONNECTED ||
        s_drop_event_reply || event == BenderEvent::None || event == BenderEvent::NetworkLost) return false;
    // Prepare the socket without turning on the microphone or taking I2S.
    s_pending_event_ms.store(millis());
    s_pending_event.store(uint32_t(event) | (uint32_t(station + 1) << 8));
    s_ai_awake = wantOnline = true;
    s_ai_last_live_ms = millis();
    return true;
}

bool bender_ai_recording() {
    return convState == ST_RECORDING;
}

bool bender_ai_owns_speaker() {
    return s_owns_spk || s_taking;
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

BenderFaceState bender_ai_face_state() {
    const uint32_t now = millis();
    const bool output_recent = s_owns_spk && bender_face_recent(now, s_tts_out_ms, 300u);
    return bender_face_resolve({
        bender_face_recent(now, s_face_error_ms.load(std::memory_order_relaxed), 2200u),
        convState == ST_RECORDING,
        !pttHeld || s_need_commit,
        output_recent,
        !s_event_reply && (convState == ST_WAIT_RESP || waitingACK || responsePending || s_need_speaker ||
            (respPlaybackPending && rbUsed() > 0)),
    });
}

void bender_ai_wake() {
    s_ai_awake = true;
    wantOnline = true;
    s_ai_last_live_ms = millis();
    (void)initMic();
}

void bender_ai_sleep() {
    if (pttHeld || s_demo || convState == ST_RECORDING || convState == ST_WAIT_RESP || speaking ||
        waitingACK || responsePending) {
        return;
    }
    s_ai_awake = false;
    wantOnline = false;
    hangupPending = false;
    sessionReady = false;
    if (wsReady) {
        wsClient.close();
    }
    wsReady = false;
    micSleep();
    ALOGLN(F("[AI] sleep"));
}

bool bender_ai_awake() {
    return s_ai_awake;
}

void bender_ai_ptt_arm() {
    s_pending_event.store(0);
    if (s_event_reply) {
        s_event_reply = false;
        s_drop_event_reply = true;
        forceRecover("user interrupts event");
    }
    if (s_demo || convState == ST_WAIT_RESP || waitingACK || responsePending || convState == ST_RECORDING) {
        return;
    }
    // Не рвати AirPlay на press: 1/2/3 кліки — пауза/трек, PTT підтверджується hold.
    bender_ai_wake();
    s_need_mic_clear = false;
    s_ptt_armed = true;
    s_preN_at_arm = preN;
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

void bender_ai_yield_radio() {
    s_pending_event.store(0);
    if (s_event_reply) {
        s_event_reply = false;
        s_drop_event_reply = true;
    }
    if (s_owns_spk || s_taking || convState != ST_IDLE) {
        forceRecover("radio play");
    }
}

void bender_ai_ptt_down() {
    if (s_demo) {
        ALOGLN(F("[PTT] wait answer"));
        return;
    }
    s_face_error_ms.store(0, std::memory_order_relaxed);
    if (!s_started || WiFi.status() != WL_CONNECTED || !s_mic_on) {
        showFaceError();
        return;
    }
    if (!wsReady) {
        const uint32_t t0 = millis();
        while (!wsReady && (uint32_t)(millis() - t0) < 1200u) {
            delay(10);
        }
        if (!wsReady) {
            ALOGLN(F("[PTT] no socket — radio stays"));
            showFaceError();
            return;
        }
    }
    const bool waiting = (convState == ST_WAIT_RESP || waitingACK || responsePending);
    if (waiting) {
        const bool recent_pcm = s_last_pcm_ms && (millis() - s_last_pcm_ms < 2500);
        if (recent_pcm) {
            ALOGLN(F("[PTT] wait answer"));
            return;
        }
        forceRecover("ptt barge");
    }
    s_resume_radio = radioState.state && strcmp(g_audio_source, "wifi") == 0;
    if (airplay_owns_speaker()) {
        airplay_interrupt();
    }
    if (!takeSpeaker()) {
        ALOGLN(F("[PTT] no speaker — TTS mute"));
        showFaceError();
    }
    pttHeld = true;
    wantOnline = true;
    hangupPending = false;
    if (speaking) {
        JsonDocument c;
        c["type"] = "response.cancel";
        wsSend(c);
    }
    // Start every recording on an empty, sample-aligned buffer, also when the
    // previous response already sent response.done but left a partial tail.
    speaking = false;
    ampMuteHw(true);
    s_tts_out_ms = 0;
    portENTER_CRITICAL(&mux);
    head = tail = 0;
    portEXIT_CRITICAL(&mux);
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
    if (s_need_speaker && !s_owns_spk && !s_giveback) {
        s_need_speaker = false;
        if (!takeSpeaker()) {
            ALOGLN(F("[AI] take speaker fail"));
            forceRecover("speaker busy/unavailable", true);
        }
    }
    const bool pcm_fresh = s_last_pcm_ms && (millis() - s_last_pcm_ms < 800);
    if (s_giveback && !pttHeld && convState != ST_RECORDING && convState != ST_WAIT_RESP &&
        !responsePending && !waitingACK && !s_dbg_play && rbUsed() == 0 && !pcm_fresh) {
        s_giveback = false;
        s_demo = 0;
        speaking = false;
        const bool had_cmd = applyVoiceCmdState();
        releaseSpeakerFromLoop();
        if (had_cmd) {
            radio_voice_after_speaker();
        }
    }
    if (s_ai_awake && !bender_ai_busy() && !s_ptt_armed &&
        (uint32_t)(millis() - s_ai_last_live_ms) > 8000u) {
        bender_ai_sleep();
    } else if (bender_ai_busy() || s_ptt_armed) {
        s_ai_last_live_ms = millis();
    }
}

void bender_ai_begin() {
    if (s_started) {
        return;
    }
    ring = (uint8_t*)ps_malloc(RING_BYTES);
    pcmDecode = (uint8_t*)ps_malloc(PCM_DECODE_BYTES);
    prebuf = (uint8_t*)ps_malloc(PREBUF_BYTES);
    s_demo_cap = (size_t)RATE * 2u * (size_t)RadioConfig::micDemoMs / 1000u;
    s_demo_pcm = (uint8_t*)ps_malloc(s_demo_cap);
    if (!ring || !pcmDecode || !prebuf) {
        ALOGLN(F("[AI] PSRAM alloc fail"));
        return;
    }
    if (!s_demo_pcm) {
        ALOGLN(F("[MIC] demo buf fail"));
        s_demo_cap = 0;
    }
    // До 15 с запису мікрофона для debug-прослушки після відповіді.
    s_dbg_cap = (size_t)RATE * 2u * (size_t)(MAX_RECORD_MS / 1000u);
    s_dbg_pcm = (uint8_t*)ps_malloc(s_dbg_cap);
    if (!s_dbg_pcm) {
        ALOGLN(F("[DBG] capture buf fail"));
        s_dbg_cap = 0;
    }
    // Debug playback is opt-in for the current session, including after an update
    // from firmware that persisted an enabled debug flag.
    s_ai_debug = false;
    if (nvsLoadAiDebug()) {
        nvsSaveAiDebug(false);
    }
    ALOG("[DBG] mode %s\n", s_ai_debug ? "ON" : "OFF");
    wsClient.setInsecure();
    if (providerIsLocal()) {
        const String basic = localBasicAuthHeader();
        if (basic.length()) {
            wsClient.addHeader("Authorization", basic);
        }
        ALOG("[WSS] local %s auth=basic (sleep until PTT)\n", wsUrl().c_str());
    } else {
        wsClient.addHeader("Authorization", String("Bearer ") + apiKey());
    }
    wsClient.onEvent(onEvent);
    wsClient.onMessage(onMessage);
    // WS вище за колонку: інакше I2S-write голодує poll() → сервер бачить client disconnected.
    xTaskCreatePinnedToCore(speakerTask, "ai_spk", 4096, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(wsTask, "ai_ws", 16384, nullptr, 5, nullptr, 1);
    s_started = true;
    ALOGLN(F("[AI] PTT hold=talk  8=sleep  9=restart"));
}

void bender_ai_set_debug(bool on) {
    s_ai_debug = on;
    if (!on) {
        dbgCaptureClear();
    }
    ALOG("[DBG] mode %s\n", on ? "ON" : "OFF");
}

bool bender_ai_debug() {
    return s_ai_debug;
}
