#include "BenderAi.h"

#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include <atomic>
#include <memory>
#include <esp_wifi.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#include "AirPlay.h"
#include "NvsConfig.h"
#include "RadioConfig.h"
#include "core0.h"
#include "pcm_analyzer.h"
#include "Pcm16.h"
#include "WakeRuntime.h"
#include "WakeVoice.h"
#include "PlaybackCompletion.h"
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
constexpr uint16_t PREBUF_N = 2000;
constexpr uint16_t PRE_IDLE_N = 80;
constexpr uint16_t WAKE_PREROLL_N = 320; // Keep late detections and the start of an immediate question.
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
// Owned only by wsTask. Recreate for each connection: ArduinoWebsockets upgrades
// ws -> wss in place but doesn't downgrade wss -> ws when the LAN comes back.
static std::unique_ptr<websockets::WebsocketsClient> wsClient;
static std::atomic<bool> s_ws_close_requested{false};
static std::atomic<bool> s_ws_cancel_requested{false};

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
static std::atomic<bool> s_mic_on{false};
static StaticSemaphore_t s_mic_mutex_storage;
static SemaphoreHandle_t s_mic_mutex = nullptr;
// Serialize reads with begin/end across PTT, sleep and diagnostic listening.
// A dedicated mic lock never blocks the AirPlay playback lock for a read.
struct MicGuard {
    MicGuard() { if (s_mic_mutex) xSemaphoreTakeRecursive(s_mic_mutex, portMAX_DELAY); }
    ~MicGuard() { if (s_mic_mutex) xSemaphoreGiveRecursive(s_mic_mutex); }
};
static volatile bool hangupPending = false;
static volatile bool sessionArmed = false;
static volatile bool pttHeld = false;
static volatile bool s_ptt_armed = false;
// Voice lifecycle is owned by wsTask. PTT callbacks ignore an active voice turn.
static std::atomic<uint8_t> s_wake_voice{0}; // 0 idle, 1 connecting, 2 recording, 3 answer
static std::atomic<uint32_t> s_wake_warmup{0};
static uint32_t s_wake_since = 0;
static WakeVoice::Endpoint s_wake_endpoint;
static bool s_wake_followup = false; // wsTask only; controls the no-question timeout.
static std::atomic<bool> s_wake_local_capture{false}, s_wake_recorded{false};
static std::atomic<bool> s_wake_prompt_pending{false};
static bool s_wake_invocation = false; // Sent with the first buffered utterance only.
static WakeVoice::Result s_wake_initial_result = WakeVoice::Continue;
static std::atomic<bool> s_dialogue_ending{false}; // Drain goodbye, then return to wake detection.
static uint32_t s_wake_no_speech_ms = 6000;
static std::atomic<uint32_t> s_favorites_lo{0}, s_favorites_hi{0};
static std::atomic<bool> s_event_voice_enabled{true};
static std::atomic<uint32_t> s_pending_event{0};
static std::atomic<uint32_t> s_pending_event_ms{0};
static std::atomic<uint8_t> s_preview_state{0};
static std::atomic<uint32_t> s_preview_id{0};
static uint32_t s_preview_queued_ms = 0;
static BenderCharacter::Settings s_preview_traits;
static char s_preview_question[BenderCharacter::questionMaxBytes + 1] = {};
static portMUX_TYPE s_preview_mux = portMUX_INITIALIZER_UNLOCKED;
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
static std::atomic<bool> s_pcm_in_flight{false};
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
static std::atomic<WakeSample::State> s_sample_state{WakeSample::Idle};
static std::atomic<bool> s_sample_cancel{false}, s_sample_leased{false};
static std::atomic<uint32_t> s_sample_bytes{0};
static uint8_t* s_sample_wav = nullptr;
static WakeSample::Pool s_sample_pool;
static_assert(WakeSample::pcmBytes % CHUNK_BYTES == 0 && WakeSample::rate == RATE,
              "Dataset buffer must contain whole microphone chunks at the native rate");
static uint32_t s_sample_id = 0, s_sample_queued_ms = 0;
static uint8_t s_sample_label = 0, s_sample_split = 0;
static bool sampleActive() { return WakeSample::active(s_sample_state.load()); }
static bool initMic();
static void micSleep();
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
    }
    ampMuteHw(false);
    i2sOutMono(buf, n);
    if (from_pcm && n >= 2) s_tts_out_ms = millis();
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

static void forceRecover(const char* why, bool show_error = false, bool keep_queued_preview = false) {
    if (s_wake_voice.load() != 0) {
        s_wake_voice.store(0);
        pttHeld = s_ptt_armed = false;
        s_ai_awake = wantOnline = false;
        s_ws_close_requested.store(true);
    }
    s_wake_local_capture.store(false);
    s_wake_recorded.store(false);
    s_wake_prompt_pending.store(false);
    s_wake_invocation=false;
    const uint8_t previewState = s_preview_state.load();
    if ((previewState == 1 && !keep_queued_preview) || previewState == 2 ||
        (previewState == 3 && show_error)) s_preview_state.store(5);
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
    s_wake_voice.store(0);
    s_wake_local_capture.store(false);
    s_wake_recorded.store(false);
    s_wake_prompt_pending.store(false);
    s_wake_invocation = false;
    s_dialogue_ending.store(false);
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
    if (s_wake_voice.load() == 3 && (s_wake_prompt_pending.load() || WakeRuntime::followupEnabled()) && !s_voice_pending && !s_dialogue_ending.load()) return;
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
    wsClient->close();
    requestGiveback();
}

static void micDump() {
    MicGuard guard;
    static uint8_t dump[1024];
    for (int i = 0; i < 8; i++) {
        i2sMic.readBytes((char*)dump, sizeof(dump));
    }
}

static bool readMicChunk16(uint8_t* mic16, int32_t* chunkPeak, bool countStats,
                           WakeSample::MicFilter* sampleFilter = nullptr, bool diagnostic = false) {
    MicGuard guard;
    if (!s_mic_on || (diagnostic && (s_ai_awake || wantOnline || sampleActive()))) return false;
    alignas(int32_t) static uint8_t micRaw[CHUNK * 4];
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
            if (sampleFilter) {
                // Preserve the raw headroom while removing DC, without the
                // additional PTT gain (currently x4). Never filter clipped PCM.
                s16[i] = sampleFilter->process(float(s32[i]) / float(uint32_t(1) << MIC_SHIFT));
                continue;
            }
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
            if (sampleFilter) {
                s16[i] = sampleFilter->process(float(s16[i]));
                continue;
            }
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
    wsClient->send(s);
}

static void wsSend(const JsonDocument& j) {
    String s;
    serializeJson(j, s);
    wsClient->send(s);
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

static bool flushRecording() {
    const uint32_t started = millis();
    while (preN) {
        if (!sessionReady || !wsReady || convState != ST_RECORDING ||
            (s_wake_voice.load() != 0 && !WakeRuntime::voiceEnabled()) ||
            uint32_t(millis()-started) >= 8000u) {
            preN = preHead = 0;
            pttHeld = s_ptt_armed = false;
            forceRecover("recording upload interrupted", true);
            requestHangup();
            return false;
        }
        drainPrebufToWs(40);
        wsClient->poll();
        vTaskDelay(1);
    }
    return sessionReady && wsReady && convState == ST_RECORDING;
}

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
        if (providerIsLocal()) {
            r["current_station"] = (int)radioState.station;
            r["wake_invocation"] = s_wake_invocation;
        }
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
        if (flushRecording()) tryPttCommit();
    }
}

static void startPttRecording();

static void cancelWakeVoice(const char* reason, bool error = false) {
    s_wake_followup = false;
    s_wake_voice.store(0);
    pttHeld = s_ptt_armed = false;
    preN = preHead = 0;
    forceRecover(reason, error);
    resetRecStats();
    s_ai_awake = wantOnline = false;
    s_ws_close_requested.store(true);
    micSleep();
    ALOG("[WakeVoice] cancel %s\n", reason);
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
    if (!strcmp(t, "character.preview.result")) {
        if (s_preview_state.load() == 2 && j["request_id"].as<uint32_t>() == s_preview_id.load()) {
            s_preview_state.store((j["ok"] | false) ? 3 : 5);
        }
    } else if (strcmp(t, "session.created") == 0) {
        JsonDocument u;
        u["type"] = "session.update";
        JsonObject s = u["session"].to<JsonObject>();
        if (providerIsLocal()) {
            s["current_station"] = (int)radioState.station;
            const auto character = nvsLoadCharacter();
            JsonObject traits = s["character"].to<JsonObject>();
            for (uint8_t i = 0; i < BenderCharacter::count; ++i)
                traits[BenderCharacter::keys[i]] = character.values[i];
        }
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
            if (providerIsLocal()) {
                r["current_station"] = (int)radioState.station;
                r["wake_invocation"] = s_wake_invocation;
            }
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
        if (providerIsLocal() && s_wake_invocation && (j["wake_only"] | false) &&
            s_wake_voice.load() == 3 && WakeRuntime::voiceEnabled()) {
            s_wake_prompt_pending.store(true);
            ALOGLN(F("[WakeVoice] name only — listen for follow-up silently"));
        }
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
        if (providerIsLocal() && !strcmp(name, "conversation.end")) {
            s_dialogue_ending.store(true);
            ALOGLN(F("[WakeVoice] goodbye — finish playback, no follow-up"));
            return;
        }
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
        // A queued preview hasn't been sent yet; a failed handshake may retry.
        forceRecover("ws closed", interrupted, true);
        s_event_reply = s_drop_event_reply = false;
        s_pending_event.store(0);
    } else if (e == websockets::WebsocketsEvent::GotPing) {
        wsClient->pong();
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
            { AirPlayAudioGuard guard; ampMuteHw(true); }
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
                if (take) s_pcm_in_flight.store(true);
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
                s_pcm_in_flight.store(false);
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
                // bender_ai_tick owns completion, including an empty unprimed
                // stream and response.done received after an underrun.
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
        s_pcm_in_flight.store(true);
        memcpy(buf, ring + tail, first);
        if (first < CHUNK_BYTES) {
            memcpy(buf + first, ring, CHUNK_BYTES - first);
        }
        tail = (tail + CHUNK_BYTES) % RING_BYTES;
        portEXIT_CRITICAL(&mux);
        spkWrite(buf, CHUNK_BYTES, true);
        s_pcm_in_flight.store(false);
        vTaskDelay(0);
    }
}

static BenderPlayback::State playbackState();

static void scaleWakeChunk(uint8_t* bytes) {
    auto* pcm = reinterpret_cast<int16_t*>(bytes);
    for (size_t i=0;i<CHUNK;++i) {
        const int32_t value=int32_t(pcm[i])*MIC_GAIN;
        pcm[i]=int16_t(value>32767?32767:(value<-32767?-32767:value));
    }
}

static void countWakeChunk(const uint8_t* bytes) {
    const auto* pcm=reinterpret_cast<const int16_t*>(bytes);
    for(size_t i=0;i<CHUNK;++i) {
        const int32_t magnitude=pcm[i]<0?-int32_t(pcm[i]):int32_t(pcm[i]);
        if(magnitude>recPeak) recPeak=magnitude;
        if(magnitude>=32767) ++recClip;
    }
    recSamples+=CHUNK;
    recMs+=10;
}

static void beginWakeInitialCapture() {
    s_wake_followup=false;
    s_wake_invocation=true;
    s_wake_no_speech_ms=6000;
    s_wake_warmup.store(0);
    s_wake_recorded.store(false);
    s_wake_prompt_pending.store(false);
    s_wake_voice.store(2);
    s_wake_since=millis();
    resetRecStats();
    s_wake_endpoint.reset();
    // Calibrate from older pre-roll; a cold-start buffer can already contain
    // speech throughout, so never classify its entire speech level as noise.
    float levels[WAKE_PREROLL_N];
    const uint16_t levelCount=preN<WAKE_PREROLL_N?preN:WAKE_PREROLL_N;
    for(uint16_t n=0;n<levelCount;++n) {
        const auto* pcm=prebuf+size_t((preHead+n)%PREBUF_N)*CHUNK_BYTES;
        levels[n]=WakeVoice::Endpoint::level(reinterpret_cast<const int16_t*>(pcm),CHUNK);
    }
    std::sort(levels,levels+levelCount);
    const float voiceRms=levelCount?levels[(levelCount-1)*95u/100u]:0;
    for(uint16_t n=0;n<preN && n<50;++n) {
        const auto* pcm=prebuf+size_t((preHead+n)%PREBUF_N)*CHUNK_BYTES;
        s_wake_endpoint.observeBackground(reinterpret_cast<const int16_t*>(pcm),CHUNK);
    }
    s_wake_endpoint.finishCalibration(voiceRms/4.f);
    s_wake_initial_result=WakeVoice::Continue;
    for(uint16_t n=0;n<preN;++n) {
        const auto* pcm=prebuf+size_t((preHead+n)%PREBUF_N)*CHUNK_BYTES;
        countWakeChunk(pcm);
        s_wake_initial_result=s_wake_endpoint.push(reinterpret_cast<const int16_t*>(pcm),CHUNK);
    }
    startPttRecording(); // Does not clear prebuf or drain the microphone.
    s_wake_local_capture.store(true);
    ALOG("[WakeVoice] local capture preroll=%ums — speak without waiting\n",unsigned(preN)*10u);
}

static WakeVoice::Result recordWakeInitialChunk(uint8_t* mic16) {
    scaleWakeChunk(mic16);
    prePush(mic16);
    countWakeChunk(mic16);
    const auto result=s_wake_endpoint.push(reinterpret_cast<const int16_t*>(mic16),CHUNK);
    // Never wrap away the start of a question at the recording limit.
    return preN>=PREBUF_N?WakeVoice::Finish:result;
}

static void wsTask(void*) {
    static uint8_t mic16[CHUNK_BYTES];
    WakeSample::MicFilter wakeFilter;
    bool wakeMic = false;
    uint32_t wakeWarmup = 0;
    uint32_t lastPing = millis();
    uint32_t backoff = 0;
    for (;;) {
        const uint8_t voice = s_wake_voice.load();
        if (voice == 1 || voice == 2) {
            const uint32_t limit = voice == 1 ? 10000u : s_wake_no_speech_ms+18000u;
            if (!WakeRuntime::voiceEnabled() || WiFi.status() != WL_CONNECTED ||
                uint32_t(millis()-s_wake_since) >= limit) {
                cancelWakeVoice("disabled/offline/timeout");
            }
        } else if (voice == 3) {
            if ((s_wake_prompt_pending.load() || WakeRuntime::followupEnabled()) && !s_voice_pending && !s_dialogue_ending.load() && sessionReady &&
                BenderPlayback::readyToRelease(playbackState())) {
                // Keep the same conversation and speaker between turns.
                respPlaybackPending = s_giveback = false;
                s_wake_followup = !s_wake_prompt_pending.exchange(false);
                s_wake_invocation = false;
                s_wake_since = millis();
                s_wake_voice.store(1);
                ALOG("[WakeVoice] follow-up — preparing %us listening window\n",unsigned(WakeRuntime::followupSeconds()));
            } else if (!bender_ai_busy() && !s_giveback) {
                s_wake_voice.store(0);
                bender_ai_sleep();
                ALOGLN(F("[WakeVoice] done — waiting for wake phrase"));
            }
        }
        const bool wakeListen = WakeRuntime::enabled() && WakeRuntime::status().ready &&
            !s_ai_awake && !wantOnline && !sampleActive() && !bender_ai_busy() && !s_demo;
        if (!wakeListen && wakeMic) {
            WakeRuntime::listening(false);
            AirPlayAudioGuard guard;
            { MicGuard micGuard; i2sMic.setTimeout(1000); }
            if (!s_ai_awake && !wantOnline && !sampleActive()) micSleep();
            wakeMic = false;
        }
        // Finish the first utterance before any blocking websocket connection.
        // This same task remains the sole microphone reader throughout handoff.
        if (s_wake_local_capture.load() && s_wake_voice.load()==2) {
            if (s_wake_initial_result==WakeVoice::Continue) {
                if (!readMicChunk16(mic16,nullptr,false,&wakeFilter)) {
                    cancelWakeVoice("initial capture microphone gap",true);continue;
                }
                s_wake_initial_result=recordWakeInitialChunk(mic16);
            }
            if(s_wake_initial_result==WakeVoice::Cancel) {
                cancelWakeVoice("initial capture no speech");continue;
            }
            if(s_wake_initial_result==WakeVoice::Finish) {
                s_wake_local_capture.store(false);
                s_wake_recorded.store(true);
                s_wake_voice.store(1);
                s_wake_since=millis();
                ALOG("[WakeVoice] utterance buffered %ums — connecting\n",unsigned(preN)*10u);
            }
            continue;
        }
        // This task is the only PCM reader. Dataset samples never enter prebuf,
        // response audio, or the websocket path (works with no network/server).
        if (s_sample_state.load() == WakeSample::Queued) {
            bool micOk = false;
            {
                AirPlayAudioGuard guard;
                if (!s_sample_cancel.load() && uint32_t(millis() - s_sample_queued_ms) < WakeSample::queueTimeoutMs)
                    micOk = initMic();
                if (micOk) {
                    i2sMic.setTimeout(100);
                }
            }
            const uint32_t started = millis();
            WakeSample::MicFilter sampleFilter;
            size_t warmed = 0;
            size_t filled = 0;
            while (micOk && filled < WakeSample::pcmBytes && !s_sample_cancel.load() &&
                   uint32_t(millis() - started) < WakeSample::captureTimeoutMs) {
                if (radioState.state || airplay_owns_speaker() || strcmp(g_audio_source, "wifi") != 0) break;
                if (readMicChunk16(mic16, nullptr, false, &sampleFilter)) {
                    if (warmed < WakeSample::rate * 2 * WakeSample::warmupMs / 1000) {
                        warmed += CHUNK_BYTES;
                        // Keep the UI in "preparing" until startup/filter settle.
                        if (warmed >= WakeSample::rate * 2 * WakeSample::warmupMs / 1000)
                            s_sample_state.store(WakeSample::Recording);
                        continue;
                    }
                    memcpy(s_sample_wav + WakeSample::headerBytes + filled, mic16, CHUNK_BYTES);
                    filled += CHUNK_BYTES;
                    s_sample_bytes.store(filled);
                } else break; // Never silently join audio across dropped microphone frames.
            }
            {
                AirPlayAudioGuard guard;
                i2sMic.setTimeout(1000);
                micSleep(); // active flag prevents the other task from closing I2S meanwhile
                preHead = preN = 0;
                const auto result = s_sample_cancel.load() ? WakeSample::Idle :
                    (micOk && filled == WakeSample::pcmBytes ? WakeSample::Ready : WakeSample::Failed);
                if (result == WakeSample::Ready) {
                    WakeSample::wavHeader(s_sample_wav);
                    s_sample_pool.find(s_sample_id)->ready = true;
                } else {
                    free(s_sample_pool.remove(s_sample_id));
                    s_sample_wav = nullptr;
                }
                s_sample_state.store(result);
                ALOG("[WakeSample] state=%u bytes=%u id=%u\n", unsigned(result), unsigned(filled), unsigned(s_sample_id));
            }
            continue;
        }
        if (s_ws_close_requested.exchange(false)) {
            if (wsClient) wsClient->close();
            wsReady = sessionReady = false;
        }
        if (s_ws_cancel_requested.exchange(false) && wsReady && wsClient) {
            wsSendRaw("{\"type\":\"response.cancel\"}");
        }
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
        if (s_preview_state.load() == 1 && uint32_t(millis() - s_preview_queued_ms) > 12000u)
            s_preview_state.store(5);
        if (!s_ai_awake && !s_demo && !wantOnline) {
            backoff = 0;
            if (wakeListen) {
                if (WakeRuntime::takeDetection()) {
                    AirPlayAudioGuard guard;
                    // Diagnostic mode never gets here. Don't retain a stale hit
                    // while offline, busy, or using the external Bluetooth path.
                    if (WakeRuntime::voiceEnabled() && !s_ai_awake && !wantOnline &&
                        WiFi.status() == WL_CONNECTED && !bender_ai_busy() &&
                        strcmp(g_audio_source, "wifi") == 0 && !s_giveback) {
                        s_face_error_ms.store(0, std::memory_order_relaxed);
                        s_preN_at_arm = 0;
                        wifi_touch_activity();
                        bender_ai_wake();
                        beginWakeInitialCapture();
                        if(!s_owns_spk) cancelWakeVoice("speaker unavailable",true);
                    } else {
                        ALOGLN(F("[WakeVoice] ignored: offline/busy/source"));
                    }
                    continue;
                }
                if (!wakeMic || !s_mic_on) {
                    WakeRuntime::listening(false);
                    AirPlayAudioGuard guard;
                    if (s_ai_awake || wantOnline || sampleActive()) continue;
                    if (!initMic()) { WakeRuntime::micError(); continue; }
                    { MicGuard micGuard; i2sMic.setTimeout(100); }
                    wakeFilter = WakeSample::MicFilter{};
                    preN=preHead=0;
                    wakeWarmup = 0; wakeMic = true;
                }
                if (readMicChunk16(mic16, nullptr, false, &wakeFilter, true)) {
                    if (s_ai_awake || wantOnline || sampleActive() || !WakeRuntime::enabled()) continue;
                    if (wakeWarmup < WakeSample::rate * WakeSample::warmupMs / 1000) {
                        wakeWarmup += CHUNK;
                    } else {
                        WakeRuntime::listening(true);
                        WakeRuntime::submit(reinterpret_cast<const int16_t*>(mic16), CHUNK);
                        if(WakeRuntime::voiceEnabled()) {
                            // Detector receives its original PCM; only the ASR buffer is amplified.
                            scaleWakeChunk(mic16);
                            prePush(mic16);
                            while(preN>WAKE_PREROLL_N) {preHead=(preHead+1)%PREBUF_N;--preN;}
                        }
                    }
                } else {
                    WakeRuntime::listening(false);
                    WakeRuntime::gap();
                    // Also discard startup after a timeout/reopen; never stitch
                    // an incomplete phrase to unrelated microphone samples.
                    wakeFilter = WakeSample::MicFilter{}; wakeWarmup = 0;
                    preN=preHead=0;
                    vTaskDelay(1);
                }
                continue;
            }
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
            if (wsClient) wsClient->close();
            wsClient.reset(); // Drop TLS state, buffers and old headers before reconnecting.
            wsClient = std::make_unique<websockets::WebsocketsClient>();
            if (providerIsLocal()) {
                const String basic = localBasicAuthHeader();
                if (basic.length()) wsClient->addHeader("Authorization", basic);
            } else {
                wsClient->addHeader("Authorization", String("Bearer ") + apiKey());
            }
            wsClient->onEvent(onEvent);
            wsClient->onMessage(onMessage);
            const String url = wsUrl();
            ALOG("[WSS] fresh transport %s\n", wsUrlUsesTls(url) ? "TLS" : "TCP");
            if (wsUrlUsesTls(url)) {
                wsClient->setInsecure();
            }
            if (!wsClient->connect(url)) {
                wsClient->close();
                ALOG("[WSS] connect fail %s heap=%u\n", url.c_str(),
                     (unsigned)ESP.getFreeHeap());
                backoff = minOfU32(backoff ? backoff * 2 : 500u, 8000u);
                if (!pttHeld && (s_owns_spk || convState != ST_IDLE || s_taking)) {
                    forceRecover("connect fail", true);
                }
                if (s_wake_voice.load() == 1) cancelWakeVoice("connect failed", true);
            } else {
                backoff = 0;
            }
            continue;
        }
        backoff = 0;
        if (s_wake_voice.load() == 1 && sessionReady) {
            if (!WakeRuntime::voiceEnabled()) { cancelWakeVoice("disabled"); continue; }
            if(s_wake_recorded.load()) {
                wsSendRaw("{\"type\":\"input_audio_buffer.clear\"}");
                if(!flushRecording()) continue;
                if(s_wake_voice.load()!=1 || convState!=ST_RECORDING) continue;
                s_wake_recorded.store(false);
                s_wake_voice.store(3);
                tryPttCommit();
                continue;
            }
            s_wake_invocation=false;
            s_wake_no_speech_ms=s_wake_followup?uint32_t(WakeRuntime::followupSeconds())*1000u:6000u;
            if(!s_wake_no_speech_ms) {cancelWakeVoice("follow-up disabled");continue;}
            s_wake_endpoint.reset(s_wake_no_speech_ms);
            s_wake_warmup.store(75); // 250 ms settle, then 500 ms background calibration.
            preN = preHead = s_preN_at_arm = 0;
            resetRecStats();
            s_wake_since = millis();
            s_wake_voice.store(2);
            startPttRecording();
            if (!s_owns_spk) { cancelWakeVoice("speaker unavailable", true); continue; }
            wsSendRaw("{\"type\":\"input_audio_buffer.clear\"}");
            micDump();
        }
        if (s_preview_state.load() == 1) {
            if (bender_ai_busy() || airplay_owns_speaker() || strcmp(g_audio_source, "wifi") != 0 || radioState.vol <= 0) {
                s_preview_state.store(5);
            } else if (sessionReady) {
                JsonDocument preview;
                preview["type"] = "character.preview";
                preview["request_id"] = s_preview_id.load();
                BenderCharacter::Settings traits;
                char question[BenderCharacter::questionMaxBytes + 1];
                portENTER_CRITICAL(&s_preview_mux);
                traits = s_preview_traits;
                memcpy(question, s_preview_question, sizeof(question));
                portEXIT_CRITICAL(&s_preview_mux);
                preview["question"] = question;
                JsonObject values = preview["character"].to<JsonObject>();
                for (uint8_t i = 0; i < BenderCharacter::count; ++i) values[BenderCharacter::keys[i]] = traits.values[i];
                s_preview_state.store(2);
                s_event_reply = true;
                s_drop_event_reply = false;
                responsePending = true;
                convState = ST_WAIT_RESP;
                stateSinceMs = millis();
                noteProgress();
                wsSend(preview);
            }
        }
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
            wsClient->poll();
            if (millis() - lastPing > 30000) {
                wsClient->ping();
                lastPing = millis();
            }
        } else if (recMs && (recMs % 200u) == 0) {
            wsClient->poll();
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
                    if (s_wake_voice.load() == 2 && s_wake_warmup.load()) {
                        const auto remaining=s_wake_warmup.load();
                        if(remaining<=50) s_wake_endpoint.observeBackground(reinterpret_cast<const int16_t*>(mic16),CHUNK);
                        if (remaining == 1) {
                            s_wake_endpoint.finishCalibration();
                            resetRecStats();
                            s_wake_warmup.store(0);
                            ALOG("[WakeVoice] listening ready noise=%u threshold=%u followup=%u — speak your question\n",
                                 unsigned(s_wake_endpoint.noise()),unsigned(s_wake_endpoint.threshold()),unsigned(s_wake_followup));
                        } else s_wake_warmup.store(remaining-1);
                        continue;
                    }
                    prePush(mic16);
                    recMs += 10;
                    if (s_wake_voice.load() == 2) {
                        const auto endpoint = s_wake_endpoint.push(reinterpret_cast<const int16_t*>(mic16), CHUNK);
                        if (!s_wake_endpoint.heardSpeech()) {preTrimIdle();recMs=uint32_t(preN)*10;}
                        if ((s_wake_endpoint.elapsedMs() % 1000u) == 0) {
                            ALOG("[WakeVoice] level rms=%u threshold=%u quiet=%ums voiced=%ums clips=%u/%u\n",
                                 unsigned(s_wake_endpoint.rms()), unsigned(s_wake_endpoint.threshold()),
                                 unsigned(s_wake_endpoint.quietMs()), unsigned(s_wake_endpoint.voicedMs()),
                                 unsigned(recClip), unsigned(recSamples));
                        }
                        if (endpoint == WakeVoice::Cancel) {
                            cancelWakeVoice("no question");
                            continue;
                        }
                        if (endpoint == WakeVoice::Finish) {
                            ALOG("[WakeVoice] question end voiced=%ums recorded=%ums\n",
                                 unsigned(s_wake_endpoint.voicedMs()), unsigned(recMs));
                            pttHeld = false;
                            s_need_commit = true;
                        }
                    }
                    if (s_wake_voice.load() != 2 && recMs >= MAX_RECORD_MS) {
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
            const bool voiceRecording = s_wake_voice.load() == 2;
            // Idle до натискання (~0.8 с) + клац кнопки. Різати лише 30 мс з голови
            // не чіпало клац — він сидів після преролу, replay починався з нього.
            uint16_t drop_head = voiceRecording ? 0 : (uint16_t)s_preN_at_arm + (uint16_t)PTT_DROP_HEAD;
            s_preN_at_arm = 0;
            if (preN > 40) {
                while (drop_head && preN > 28) {
                    preHead = (preHead + 1) % PREBUF_N;
                    preN--;
                    drop_head--;
                }
                uint8_t drop_tail = voiceRecording ? 0 : PTT_DROP_TAIL;
                while (drop_tail && preN > 24) {
                    preN--;
                    drop_tail--;
                }
            }
            const uint32_t queued_ms = (uint32_t)preN * 10u;
            const uint32_t t0 = millis();
            if (!flushRecording()) continue;
            ALOG("[PTT] flush %u ms audio in %u ms\n", (unsigned)queued_ms, (unsigned)(millis() - t0));
            // A poll during flush can disconnect/recover; never resurrect it.
            if (voiceRecording && s_wake_voice.load() == 2 && convState == ST_RECORDING)
                s_wake_voice.store(3);
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
    MicGuard guard;
    WakeRuntime::listening(false);
    if (!s_mic_on) {
        return;
    }
    i2sMic.end();
    s_mic_on = false;
    ALOGLN(F("[AI] mic sleep"));
}

static bool initMic() {
    MicGuard guard;
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
    if (s_wake_voice.load() == 1) return true;
    return sampleActive() || s_demo || s_ptt_armed || s_taking || s_owns_spk || convState != ST_IDLE || speaking || waitingACK || responsePending ||
           (respPlaybackPending && rbUsed() > 0) || pttHeld || s_dbg_play;
}

bool bender_ai_favorite_station(int station) {
    if (station < 0 || station >= 64) return false;
    return station < 32 ? (s_favorites_lo.load() & (uint32_t(1) << station)) != 0
                        : (s_favorites_hi.load() & (uint32_t(1) << (station - 32))) != 0;
}

bool bender_ai_event_voice_enabled() { return s_event_voice_enabled.load(); }

const char* bender_ai_sample_start(const char* label, const char* split) {
    AirPlayAudioGuard guard;
    const int li = WakeSample::labelIndex(label), si = WakeSample::splitIndex(split);
    if (li < 0 || si < 0) return "Выбери фразу и набор примеров.";
    if (!s_started) return "Микрофон недоступен: перезапусти колонку.";
    if (bender_ai_busy() || s_ai_awake || wantOnline || wsReady || s_preview_state.load() == 1 ||
        s_drop_event_reply || s_sample_leased.load())
        return "Бендер занят. Дождись завершения и повтори.";
    if (radioState.state || airplay_owns_speaker() || strcmp(g_audio_source, "wifi") != 0)
        return "Поставь музыку на паузу, заверши AirPlay и выбери режим FM.";
    if (s_sample_pool.count() >= WakeSample::poolCapacity)
        return "Пул заполнен: 30 записей. Скачай набор и удали ненужные примеры.";
    const size_t bytes = WakeSample::headerBytes + WakeSample::pcmBytes;
    if (ESP.getFreePsram() < bytes + WakeSample::memoryReserve)
        return "Достигнут резерв памяти. Скачай набор и освободи место в пуле.";
    auto* wav = (uint8_t*)ps_malloc(bytes);
    if (!wav) return "Недостаточно непрерывной памяти для записи. Скачай набор и освободи место.";
    s_sample_label = uint8_t(li); s_sample_split = uint8_t(si);
    if (s_sample_id == 0) s_sample_id = esp_random(); // Avoid matching a stale tab after reboot.
    do { if (++s_sample_id == 0) ++s_sample_id; } while (s_sample_pool.find(s_sample_id));
    if (!s_sample_pool.add(s_sample_id, uint8_t(li), uint8_t(si), wav)) {
        free(wav); return "Не удалось добавить запись в пул.";
    }
    s_sample_wav = wav;
    s_sample_bytes.store(0); s_sample_cancel.store(false);
    s_sample_queued_ms = millis();
    s_pending_event.store(0);
    s_ai_awake = wantOnline = false;
    s_ws_close_requested.store(true);
    s_sample_state.store(WakeSample::Queued);
    return nullptr;
}

WakeSample::Status bender_ai_sample_status() {
    AirPlayAudioGuard guard;
    return {s_sample_state.load(), s_sample_id, s_sample_bytes.load() * 1000u / (WakeSample::rate * 2u),
            s_sample_label, s_sample_split};
}

bool bender_ai_sample_cancel(uint32_t id) {
    AirPlayAudioGuard guard;
    if (s_sample_leased.load()) return false;
    if (id == s_sample_id && sampleActive()) { s_sample_cancel.store(true); return true; }
    auto* wav = s_sample_pool.remove(id);
    if (!wav) return false;
    free(wav);
    if (id == s_sample_id) {
        s_sample_state.store(WakeSample::Idle); s_sample_wav = nullptr;
        s_sample_bytes.store(0);
    }
    return true;
}

const uint8_t* bender_ai_sample_audio(uint32_t id, size_t& size) {
    AirPlayAudioGuard guard;
    size = 0;
    const auto* entry = s_sample_pool.find(id);
    if (!entry || !entry->ready || s_sample_leased.exchange(true)) return nullptr;
    size = WakeSample::headerBytes + WakeSample::pcmBytes;
    return entry->wav;
}

void bender_ai_sample_audio_release() { s_sample_leased.store(false); }

size_t bender_ai_sample_list(WakeSample::Status* out, size_t capacity) {
    AirPlayAudioGuard guard;
    return s_sample_pool.list(out, capacity);
}

bool bender_ai_sample_clear() {
    AirPlayAudioGuard guard;
    if (sampleActive() || s_sample_leased.load()) return false;
    WakeSample::Status items[WakeSample::poolCapacity];
    const size_t count = s_sample_pool.list(items, WakeSample::poolCapacity);
    for (size_t i = 0; i < count; ++i) free(s_sample_pool.remove(items[i].id));
    s_sample_wav = nullptr; s_sample_bytes.store(0); s_sample_state.store(WakeSample::Idle);
    return true;
}

const char* bender_ai_preview_character(const BenderCharacter::Settings& settings, const char* question) {
    AirPlayAudioGuard guard;
    if (!providerIsLocal() || !s_started) return "Прослушивание доступно через локальный AI сервер.";
    if (!BenderCharacter::valid(settings)) return "Недопустимые настройки характера.";
    if (!BenderCharacter::validQuestion(question)) return "Напиши тестовый вопрос: от 1 до 200 символов.";
    if (WiFi.status() != WL_CONNECTED) return "Бендер не подключён к Wi-Fi.";
    if (radioState.vol <= 0) return "Сначала прибавь громкость Бендера.";
    if (airplay_owns_speaker() || strcmp(g_audio_source, "wifi") != 0)
        return "Заверши AirPlay или Bluetooth и переключи Бендера в режим FM.";
    const uint8_t state = s_preview_state.load();
    if ((state >= 1 && state <= 3) || bender_ai_busy() || s_drop_event_reply)
        return "Бендер сейчас занят. Попробуй после ответа.";
    s_pending_event.store(0);
    portENTER_CRITICAL(&s_preview_mux);
    s_preview_traits = settings;
    strcpy(s_preview_question, question); // Length checked before copying into the queue.
    s_preview_queued_ms = millis();
    portEXIT_CRITICAL(&s_preview_mux);
    s_preview_id.fetch_add(1);
    s_ai_last_live_ms = millis();
    s_ai_awake = wantOnline = true; // No microphone capture for a web preview.
    s_preview_state.store(1);
    return nullptr;
}

uint8_t bender_ai_preview_status() {
    const uint8_t state = s_preview_state.load();
    return state == 2 && bender_ai_tts_playing() ? 3 : state;
}

bool bender_ai_event(BenderEvent event, int station) {
    AirPlayAudioGuard guard;
    if (!providerIsLocal() || !s_started || !s_event_voice_enabled.load() || s_preview_state.load() == 1 ||
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
    if (s_sample_state.load() == WakeSample::Recording) return BenderFaceState::Listening;
    if (s_wake_recorded.load()) return BenderFaceState::Thinking;
    if (s_wake_voice.load() == 1 || (s_wake_voice.load() == 2 && s_wake_warmup.load()))
        return BenderFaceState::Curious;
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
    AirPlayAudioGuard guard;
    if (sampleActive()) return;
    WakeRuntime::listening(false);
    s_ai_awake = true;
    wantOnline = true;
    s_ai_last_live_ms = millis();
    (void)initMic();
}

void bender_ai_sleep() {
    AirPlayAudioGuard guard;
    if (sampleActive()) return;
    if (s_wake_voice.load() == 1) return;
    if (s_preview_state.load() == 1) s_preview_state.store(5);
    if (pttHeld || s_demo || convState == ST_RECORDING || convState == ST_WAIT_RESP || speaking ||
        waitingACK || responsePending) {
        return;
    }
    s_ai_awake = false;
    wantOnline = false;
    hangupPending = false;
    sessionReady = false;
    // Closing here races the network task's poll/connect. Let its owner close it.
    s_ws_close_requested.store(true);
    micSleep();
    ALOGLN(F("[AI] sleep"));
}

bool bender_ai_awake() {
    return s_ai_awake;
}

uint8_t bender_ai_wake_voice_state() {
    if(s_wake_recorded.load()) return 3; // Already heard the question; waiting for transport/ASR.
    const uint8_t state = s_wake_voice.load();
    return state == 2 && s_wake_warmup.load() ? 1 : state;
}

void bender_ai_ptt_arm() {
    if (s_wake_voice.load()) return;
    AirPlayAudioGuard guard;
    if (sampleActive()) { s_sample_cancel.store(true); return; }
    if (s_preview_state.load() >= 1 && s_preview_state.load() <= 3) s_preview_state.store(5);
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
    if (sampleActive()) return;
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
    if (s_preview_state.load() >= 1 && s_preview_state.load() <= 3) s_preview_state.store(5);
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
    if (s_wake_voice.load()) return;
    if (sampleActive()) { s_sample_cancel.store(true); return; }
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
    startPttRecording();
}

static void startPttRecording() {
    if(s_wake_voice.load()==0) s_wake_invocation=false;
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
        s_ws_cancel_requested.store(true);
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
    if (s_wake_voice.load()) return;
    if (sampleActive()) return;
    pttHeld = false;
    s_ptt_armed = false;
    if (convState == ST_RECORDING) {
        ALOGLN(F("[PTT] release"));
        s_need_commit = true;
    }
}

static BenderPlayback::State playbackState() {
    // Queue removal and the in-flight flag are one operation in speakerTask.
    // Read them together so the final dequeued block cannot look like silence.
    portENTER_CRITICAL(&mux);
    const BenderPlayback::State state{
        s_owns_spk, s_taking, convState == ST_IDLE && !pttHeld && !s_ptt_armed && s_wake_voice.load()!=1,
        speaking, responsePending || waitingACK || s_need_speaker,
        bool(s_demo == 1 || s_demo == 2 || s_dbg_play), s_pcm_in_flight.load(), rbUsed(),
        millis(), s_last_pcm_ms, s_tts_out_ms};
    portEXIT_CRITICAL(&mux);
    return state;
}

void bender_ai_tick() {
    // Independent of priming: never leave I2S owned forever after the final
    // buffer drained while response.done / the input freshness timer was pending.
    const bool keepForFollowup = s_wake_voice.load() == 3 && (s_wake_prompt_pending.load() || WakeRuntime::followupEnabled()) &&
        !s_voice_pending && !s_dialogue_ending.load() && sessionReady;
    if (!keepForFollowup && BenderPlayback::readyToRelease(playbackState())) {
        respPlaybackPending = false;
        requestGiveback();
    }
    if (s_preview_state.load() == 3 && !bender_ai_busy()) s_preview_state.store(4);
    if (s_need_speaker && !s_owns_spk && !s_giveback) {
        s_need_speaker = false;
        if (!takeSpeaker()) {
            ALOGLN(F("[AI] take speaker fail"));
            forceRecover("speaker busy/unavailable", true);
        }
    }
    const auto playback = playbackState();
    const bool pcm_fresh = s_last_pcm_ms && (millis() - s_last_pcm_ms < 800);
    if (!keepForFollowup && s_giveback && s_wake_voice.load()!=1 && !pttHeld && convState != ST_RECORDING && convState != ST_WAIT_RESP &&
        !responsePending && !waitingACK && !s_dbg_play && !playback.inFlight &&
        playback.queuedBytes == 0 && !pcm_fresh &&
        (!s_tts_out_ms || uint32_t(millis()-s_tts_out_ms) >= 350u)) {
        s_giveback = false;
        s_demo = 0;
        speaking = false;
        const bool had_cmd = applyVoiceCmdState();
        releaseSpeakerFromLoop();
        if (had_cmd) {
            radio_voice_after_speaker();
        }
    }
    if (s_dialogue_ending.load() && !bender_ai_busy() && !s_giveback && !s_ptt_armed) {
        s_wake_voice.store(0);
        bender_ai_sleep();
        s_dialogue_ending.store(false);
        ALOGLN(F("[WakeVoice] conversation ended — waiting for wake phrase"));
    }
    if (s_ai_awake && !bender_ai_busy() && !s_ptt_armed && s_preview_state.load() != 1 &&
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
    WakeRuntime::setFollowupSeconds(nvsLoadWakeFollowupSeconds());
    if (!s_mic_mutex) s_mic_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mic_mutex_storage);
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
    if (providerIsLocal()) {
        ALOG("[WSS] local %s auth=basic (sleep until PTT)\n", wsUrl().c_str());
    }
    // WS вище за колонку: інакше I2S-write голодує poll() → сервер бачить client disconnected.
    xTaskCreatePinnedToCore(speakerTask, "ai_spk", 4096, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(wsTask, "ai_ws", 16384, nullptr, 5, nullptr, 1);
    s_started = true;
    if (nvsLoadWakeVoiceEnabled()) {
        if (const char* error = WakeRuntime::enable(true, true)) {
            ALOG("[Wake] restore failed: %s\n", error);
        } else {
            ALOGLN(F("[Wake] voice restored from settings"));
        }
    }
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
