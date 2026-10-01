#include "AirPlay.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include <mdns.h>
#include <sys/time.h>
#include <freertos/semphr.h>

#include "AirPlayPriv.h"
#include "AirPlayPolicy.h"
#include "BenderAi.h"
#include "RadioConfig.h"
#include "core0.h"
#include "pcm_analyzer.h"

AirPlaySess g_ap;
WiFiServer g_ap_rtsp(RadioConfig::airplayRtspPort);
WiFiClient g_ap_cli;
WiFiUDP g_ap_audio;
WiFiUDP g_ap_ctrl;
WiFiUDP g_ap_time;
std::atomic<bool> g_ap_owns{false};
uint8_t g_ap_mac[6]{};

static TaskHandle_t s_rtp_task = nullptr;
static TaskHandle_t s_play_task = nullptr;
static TaskHandle_t s_rtsp_task = nullptr;
static bool s_begun = false;
static bool s_mdns_host = false;
static std::atomic<bool> s_ap_accept{false};

static SemaphoreHandle_t audio_mutex() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexStatic(&storage);
    return mutex;
}

void airplay_audio_lock() { xSemaphoreTakeRecursive(audio_mutex(), portMAX_DELAY); }
bool airplay_audio_try_lock() { return xSemaphoreTakeRecursive(audio_mutex(), 0) == pdTRUE; }
void airplay_audio_unlock() { xSemaphoreGiveRecursive(audio_mutex()); }

// 352 семпли ALAC ≈ 8 мс. Велике кільце — iPhone у lock реже Wi‑Fi пачками.
static constexpr int kApFrameSamp = 352;
static constexpr int kApFrameCh = 2;
static constexpr int kApRing = 180;
static constexpr int kApPrebuf = 90;
static constexpr int kApFrameBytes = kApFrameSamp * kApFrameCh * (int)sizeof(int16_t);
static constexpr int kPktQ = 96;
static constexpr int kPktMax = 1472;

struct ApUdpPkt {
    uint16_t n;
    uint8_t data[kPktMax];
};

static ApUdpPkt* s_pktq = nullptr;
static volatile uint32_t s_pq_w = 0;
static volatile uint32_t s_pq_r = 0;
static udp_pcb* s_audio_pcb = nullptr;

static int16_t* s_ring = nullptr;
static volatile uint32_t s_ring_w = 0;
static volatile uint32_t s_ring_r = 0;
static AirPlayPrebuffer s_prebuffer;
static AirPlayRtpOrder s_rtp_order;
static portMUX_TYPE s_pkt_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_under_n = 0;

static uint32_t ring_count() {
    return s_ring_w - s_ring_r;
}

static void ring_reset() {
    // Caller holds the audio mutex; the lwIP callback only uses s_pkt_mux.
    s_ring_w = 0;
    s_ring_r = 0;
    portENTER_CRITICAL(&s_pkt_mux);
    s_pq_w = 0;
    s_pq_r = 0;
    portEXIT_CRITICAL(&s_pkt_mux);
    s_prebuffer.reset();
    s_rtp_order.reset();
}

static void audio_recv_cb(void*, struct udp_pcb*, struct pbuf* p, const ip_addr_t*, u16_t) {
    if (!p) {
        return;
    }
    portENTER_CRITICAL(&s_pkt_mux);
    if (!s_pktq || !g_ap.session || p->tot_len > kPktMax ||
        (s_pq_w - s_pq_r) >= (uint32_t)kPktQ) {
        portEXIT_CRITICAL(&s_pkt_mux);
        pbuf_free(p);
        return;
    }
    ApUdpPkt* d = &s_pktq[s_pq_w % (uint32_t)kPktQ];
    u16_t n = p->tot_len;
    if (n > (u16_t)kPktMax) {
        n = (u16_t)kPktMax;
    }
    pbuf_copy_partial(p, d->data, n, 0);
    d->n = n;
    s_pq_w++;
    portEXIT_CRITICAL(&s_pkt_mux);
    pbuf_free(p);
}

static int pktq_pop(uint8_t* out, int maxn) {
    portENTER_CRITICAL(&s_pkt_mux);
    if (!s_pktq || s_pq_w == s_pq_r) {
        portEXIT_CRITICAL(&s_pkt_mux);
        return 0;
    }
    const ApUdpPkt* s = &s_pktq[s_pq_r % (uint32_t)kPktQ];
    int n = (int)s->n;
    if (n > maxn) {
        n = maxn;
    }
    memcpy(out, s->data, (size_t)n);
    s_pq_r++;
    portEXIT_CRITICAL(&s_pkt_mux);
    return n;
}

static bool audio_pcb_open() {
    if (s_audio_pcb) {
        return true;
    }
    LOCK_TCPIP_CORE();
    s_audio_pcb = udp_new();
    if (s_audio_pcb) {
        if (udp_bind(s_audio_pcb, IP_ANY_TYPE, g_ap.audio_port) == ERR_OK) {
            udp_recv(s_audio_pcb, audio_recv_cb, nullptr);
        } else {
            udp_remove(s_audio_pcb);
            s_audio_pcb = nullptr;
        }
    }
    UNLOCK_TCPIP_CORE();
    if (!s_audio_pcb) {
        Serial.println(F("[AirPlay] UDP pcb fail"));
    }
    return s_audio_pcb != nullptr;
}

static void audio_pcb_close() {
    if (!s_audio_pcb) {
        return;
    }
    LOCK_TCPIP_CORE();
    udp_remove(s_audio_pcb);
    s_audio_pcb = nullptr;
    UNLOCK_TCPIP_CORE();
}

static void ring_push(const int16_t* pcm, int frames) {
    if (!s_ring || !pcm || frames <= 0) {
        return;
    }
    if (ring_count() >= (uint32_t)kApRing) {
        return;
    }
    int16_t* dst = s_ring + (s_ring_w % (uint32_t)kApRing) * kApFrameSamp * kApFrameCh;
    const int n = frames * kApFrameCh;
    const int maxn = kApFrameSamp * kApFrameCh;
    if (n >= maxn) {
        memcpy(dst, pcm, (size_t)kApFrameBytes);
    } else {
        memcpy(dst, pcm, (size_t)n * sizeof(int16_t));
        memset(dst + n, 0, (size_t)(maxn - n) * sizeof(int16_t));
    }
    s_ring_w++;
}

static bool ring_pop(int16_t* pcm) {
    if (!s_ring || s_ring_w == s_ring_r) {
        return false;
    }
    const int16_t* src = s_ring + (s_ring_r % (uint32_t)kApRing) * kApFrameSamp * kApFrameCh;
    memcpy(pcm, src, (size_t)kApFrameBytes);
    s_ring_r++;
    return true;
}

void airplay_hw_mute(bool mute) {
    if (RadioConfig::ampMutePin == 255 || !RadioConfig::ampUseHardwareMute) {
        return;
    }
    pinMode(RadioConfig::ampMutePin, OUTPUT);
    const bool mute_high = RadioConfig::ampMuteWhenIdleHigh ? mute : !mute;
    digitalWrite(RadioConfig::ampMutePin, mute_high ? HIGH : LOW);
}

bool airplay_take_speaker() {
    AirPlayAudioGuard guard;
    if (g_ap_owns) {
        return true;
    }
    if (!airplay_accepts()) {
        return false;
    }
    if (bender_ai_busy() || bender_ai_owns_speaker() || airplay_rtsp_hangup_pending()) {
        return false;
    }
    if (strcmp(g_audio_source, "wifi") == 0) {
        audio.setVolume(0);
        if (audio.isRunning()) {
            audio.stopSong();
        }
        delay(20);
    }
    free_uart0_from_i2s_pins();
    (void)audio.reclaimI2SChannel();
    if (!audio.i2sSetSampleRateHz(44100)) return false;
    airplay_hw_mute(false);
    g_ap_owns = true;
    bender_ai_sleep();
    Serial.printf("[AirPlay] I2S0 taken vol=%d\n", (int)radioState.vol);
    return true;
}

void airplay_release_speaker() {
    AirPlayAudioGuard guard;
    if (!g_ap_owns) {
        return;
    }
    airplay_hw_mute(true);
    g_ap_owns = false;
    apply_output_volume();
    if (radioState.state && strcmp(g_audio_source, "wifi") == 0 && !play_mode_is_airplay()) {
        reconnect = station_url_for_current();
    }
    Serial.println(F("[AirPlay] I2S0 back to radio"));
}

static void i2s_feed(const uint8_t* p, size_t left) {
    const uint32_t started = millis();
    size_t off = 0;
    uint8_t spins = 0;
    while (off < left && uint32_t(millis() - started) < 80u) {
        const size_t w = audio.i2sWriteRaw(p + off, left - off, 40);
        if (!w) {
            if (++spins > 20) {
                break;
            }
            vTaskDelay(1);
            continue;
        }
        spins = 0;
        off += w;
    }
}

void airplay_write_pcm(int16_t* stereo, int frames) {
    if (!stereo || frames <= 0 || !g_ap_owns) {
        return;
    }
    pcm_analyzer_on_airplay_pcm16(stereo, (uint16_t)frames);
    float g = g_ap.vol_gain;
    if (g < 0) {
        g = 0;
    }
    if (g > 1) {
        g = 1;
    }
    const int scale = (int)(g * 256.0f + 0.5f);
    const int n = frames * 2;
    if (scale != 256) {
        for (int i = 0; i < n; i++) {
            int32_t x = ((int32_t)stereo[i] * scale) >> 8;
            if (x > 32767) {
                x = 32767;
            } else if (x < -32767) {
                x = -32767;
            }
            stereo[i] = (int16_t)x;
        }
    }
    i2s_feed(reinterpret_cast<const uint8_t*>(stereo), (size_t)n * 2);
}

void airplay_rtp_flush(bool hasBoundary, uint32_t timestamp) {
    AirPlayAudioGuard guard;
    ring_reset();
    s_rtp_order.reset(hasBoundary, timestamp);
    pcm_analyzer_reset();
    airplay_meta_on_flush();
    g_ap.playing = false;
    g_ap.paused = true;
}

void airplay_session_clear() {
    AirPlayAudioGuard guard;
    g_ap.session = false;
    g_ap.playing = false;
    g_ap.encrypted = false;
    g_ap.paused = false;
    g_ap.vol_gain = airplay_db_to_gain(airplay_ui_to_db((int)radioState.vol));
    g_ap.enc_touched = false;
    g_ap.phone_vol_seen = false;
    ring_reset();
    pcm_analyzer_reset();
    airplay_meta_clear();
    if (g_ap.alac) {
        alac_free(g_ap.alac);
        g_ap.alac = nullptr;
    }
}

static void ntp_write(uint8_t* p) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const uint32_t sec = (uint32_t)tv.tv_sec + 0x83AA7E80u;
    const uint32_t frac = (uint32_t)(((uint64_t)tv.tv_usec << 32) / 1000000ull);
    p[0] = (uint8_t)(sec >> 24);
    p[1] = (uint8_t)(sec >> 16);
    p[2] = (uint8_t)(sec >> 8);
    p[3] = (uint8_t)sec;
    p[4] = (uint8_t)(frac >> 24);
    p[5] = (uint8_t)(frac >> 16);
    p[6] = (uint8_t)(frac >> 8);
    p[7] = (uint8_t)frac;
}

static void reply_timing(WiFiUDP& udp, uint8_t* pkt, int n) {
    if (n < 32) {
        return;
    }
    memcpy(pkt + 8, pkt + 24, 8);
    ntp_write(pkt + 16);
    ntp_write(pkt + 24);
    pkt[1] = 0x53 | 0x80;
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(pkt, n);
    udp.endPacket();
}

static void play_rtp_audio(uint8_t* pkt, int n, uint8_t* dec, int16_t* pcm) {
    if (!g_ap.session) return;
    if (n <= 12) {
        return;
    }
    const uint8_t type = pkt[1] & 0x7f;
    if (type != 0x60) {
        return;
    }
    const uint32_t timestamp = (uint32_t(pkt[4]) << 24) | (uint32_t(pkt[5]) << 16) |
                               (uint32_t(pkt[6]) << 8) | uint32_t(pkt[7]);
    if (!s_rtp_order.accepts(timestamp)) return;
    const uint8_t* pay = pkt + 12;
    size_t pn = (size_t)n - 12;
    if ((pkt[0] & 0x10) && pn >= 4) {
        const uint16_t ext = ((uint16_t)pay[2] << 8) | pay[3];
        const size_t skip = 4 + (size_t)ext * 4;
        if (skip < pn) {
            pay += skip;
            pn -= skip;
        }
    }
    const uint8_t* src = pay;
    if (g_ap.encrypted && pn >= 16) {
        airplay_aes_decrypt(pay, pn, dec);
        src = dec;
    }
    if (!g_ap.alac) {
        return;
    }
    int outn = kApFrameSamp * 4;
    alac_decode_frame(g_ap.alac, const_cast<unsigned char*>(src), pcm, &outn);
    if (outn >= 4) {
        s_rtp_order.commit(timestamp);
        g_ap.last_rtp_ms = millis();
        g_ap.paused = false;  // Some senders resume with RTP without another RECORD.
        g_ap.playing = true;
        airplay_meta_on_play();
        ring_push(pcm, outn / 4);
    }
}

static void play_task(void*) {
    static int16_t pcm[kApFrameSamp * kApFrameCh];
    while (true) {
        bool active = false;
        {
            AirPlayAudioGuard guard;
            active = g_ap_owns;
            if (!active) {
                s_prebuffer.reset();
            } else if (g_ap.session && !g_ap.paused &&
                       s_prebuffer.ready(ring_count(), kApPrebuf) && ring_pop(pcm)) {
                // Silence the amplifier at zero without waiting for queued DMA audio.
                airplay_hw_mute(g_ap.vol_gain.load() <= 0.0f);
                airplay_write_pcm(pcm, kApFrameSamp);
            } else {
                ++s_under_n;
                memset(pcm, 0, (size_t)kApFrameBytes);
                i2s_feed(reinterpret_cast<const uint8_t*>(pcm), (size_t)kApFrameBytes);
            }
        }
        // Let the lower-priority RTSP task acquire the lifecycle mutex between frames.
        vTaskDelay(active ? 1 : pdMS_TO_TICKS(20));
    }
}

static void service_sync(uint8_t* pkt) {
    for (int i = 0; i < 4; i++) {
        int n = g_ap_time.parsePacket();
        if (n < 32 || n > 1600) {
            break;
        }
        n = g_ap_time.read(pkt, n);
        if ((pkt[1] & 0x7f) == 0x52) {
            reply_timing(g_ap_time, pkt, n);
        }
    }
    for (int i = 0; i < 4; i++) {
        int n = g_ap_ctrl.parsePacket();
        if (n <= 4 || n > 1600) {
            break;
        }
        n = g_ap_ctrl.read(pkt, n);
        const uint8_t t = pkt[1] & 0x7f;
        if (t == 0x52) {
            reply_timing(g_ap_ctrl, pkt, n);
        }
    }
}

static void rtp_task(void*) {
    static uint8_t pkt[1600], sync[1600], dec[1600];
    static int16_t pcm[kApFrameSamp * kApFrameCh];
    while (true) {
        if (!g_ap.session) {
            vTaskDelay(50);
            continue;
        }
        int got = 0;
        for (int i = 0; i < 48; i++) {
            AirPlayAudioGuard guard;
            if (!g_ap.session) break;
            if ((i & 7) == 0) service_sync(sync);
            const int n = pktq_pop(pkt, 1600);
            if (n <= 12) {
                break;
            }
            play_rtp_audio(pkt, n, dec, pcm);
            ++got;
        }
        {
            AirPlayAudioGuard guard;
            if (g_ap.session) {
                int n = g_ap_ctrl.parsePacket();
                if (n > 16 && n <= 1600) {
                    n = g_ap_ctrl.read(pkt, n);
                    if (n > 16 && (pkt[1] & 0x7f) == 0x56) {
                        play_rtp_audio(pkt + 4, n - 4, dec, pcm);
                    }
                }
            }
        }
        (void)got;
        vTaskDelay(1);
    }
}

bool airplay_rtp_start() {
    AirPlayAudioGuard guard;
    if (!s_ring) {
        const size_t nbytes = (size_t)kApRing * (size_t)kApFrameBytes;
        s_ring = (int16_t*)heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) {
            s_ring = (int16_t*)malloc(nbytes);
        }
        if (!s_ring) {
            Serial.println(F("[AirPlay] ring alloc fail"));
            return false;
        }
        Serial.printf("[AirPlay] ring %u frames %u KB\n", (unsigned)kApRing, (unsigned)(nbytes / 1024));
    }
    if (!s_pktq) {
        s_pktq = (ApUdpPkt*)heap_caps_malloc((size_t)kPktQ * sizeof(ApUdpPkt),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pktq) {
            s_pktq = (ApUdpPkt*)malloc((size_t)kPktQ * sizeof(ApUdpPkt));
        }
        if (!s_pktq) {
            Serial.println(F("[AirPlay] pktq alloc fail"));
            return false;
        }
        Serial.printf("[AirPlay] UDP q %u pkts\n", (unsigned)kPktQ);
    }
    ring_reset();
    if (!audio_pcb_open() || !g_ap_ctrl.begin(g_ap.ctrl_port) || !g_ap_time.begin(g_ap.time_port))
        return false;
    if (!s_rtp_task) {
        if (xTaskCreatePinnedToCore(rtp_task, "ap_rtp", 12288, nullptr, 5, &s_rtp_task, 1) != pdPASS)
            return false;
    }
    if (!s_play_task) {
        if (xTaskCreatePinnedToCore(play_task, "ap_play", 4096, nullptr, 6, &s_play_task, 1) != pdPASS)
            return false;
    }
    return true;
}

void airplay_rtp_stop() {
    AirPlayAudioGuard guard;
    g_ap.session = false;
    g_ap.playing = false;
    g_ap.paused = false;
    ring_reset();
    pcm_analyzer_reset();
    audio_pcb_close();
    g_ap_ctrl.stop();
    g_ap_time.stop();
}

static bool advertise() {
    AirPlayAudioGuard guard;
    if (g_ap.advertised || !s_ap_accept || WiFi.status() != WL_CONNECTED) {
        return g_ap.advertised;
    }
    WiFi.macAddress(g_ap_mac);
    if (!s_mdns_host) {
        if (!MDNS.begin(RadioConfig::airplayName)) {
            Serial.println(F("[AirPlay] MDNS.begin fail"));
            return false;
        }
        s_mdns_host = true;
    }
    char inst[48];
    snprintf(inst, sizeof(inst), "%02X%02X%02X%02X%02X%02X@%s", g_ap_mac[0], g_ap_mac[1],
             g_ap_mac[2], g_ap_mac[3], g_ap_mac[4], g_ap_mac[5], RadioConfig::airplayName);
    static mdns_txt_item_t txt[] = {
        {"txtvers", "1"},
        {"ch", "2"},
        {"cn", "0,1"},
        {"et", "0,1"},
        {"sv", "false"},
        {"da", "true"},
        {"sr", "44100"},
        {"ss", "16"},
        {"pw", "false"},
        {"vn", "3"},
        {"tp", "UDP"},
        {"md", "0,1,2"},
        {"vs", "105.1"},
        {"am", "AirPort4,107"},
        {"ek", "1"},
    };
    if (mdns_service_add(inst, "_raop", "_tcp", RadioConfig::airplayRtspPort, txt,
                         sizeof(txt) / sizeof(txt[0])) != ESP_OK) {
        Serial.println(F("[AirPlay] mDNS _raop fail"));
        return false;
    }
    g_ap.advertised = true;
    Serial.printf("[AirPlay] %s  %s:%u\n", inst, WiFi.localIP().toString().c_str(),
                  (unsigned)RadioConfig::airplayRtspPort);
    return true;
}

static void unadvertise() {
    AirPlayAudioGuard guard;
    if (!g_ap.advertised) {
        return;
    }
    mdns_service_remove("_raop", "_tcp");
    g_ap.advertised = false;
    Serial.println(F("[AirPlay] mDNS off"));
}

static void mdns_refresh() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        mdns_netif_action(netif, MDNS_EVENT_ANNOUNCE_IP4);
    }
}

static void rtsp_task(void*) {
    uint32_t last_mdns = 0;
    while (true) {
        if (g_ap.ready) {
            if (s_ap_accept && !g_ap.advertised) {
                advertise();
            }
            airplay_rtsp_poll();
            airplay_dacp_poll();
            if (g_ap.advertised && (uint32_t)(millis() - last_mdns) >= 20000u) {
                last_mdns = millis();
                mdns_refresh();
                mdns_service_txt_item_set("_raop", "_tcp", "vs", "105.1");
                mdns_service_txt_item_set("_raop", "_tcp", "md", "0,1,2");
            }
        }
        vTaskDelay(g_ap.session ? 3 : 40);
    }
}

void airplay_begin() {
    if (s_begun || !RadioConfig::airplayEnable) {
        return;
    }
    s_begun = true;
    if (!airplay_crypto_begin()) {
        Serial.println(F("[AirPlay] crypto fail — advertising anyway"));
    }
    g_ap_rtsp.begin();
    g_ap.ready = true;
    if (!s_rtsp_task) {
        xTaskCreatePinnedToCore(rtsp_task, "ap_rtsp", 6144, nullptr, 4, &s_rtsp_task, 1);
    }
    Serial.println(F("[AirPlay] RTSP listen"));
}

void airplay_tick() {
}

void airplay_interrupt() {
    AirPlayAudioGuard guard;
    // Socket buffers belong to the RTSP task; never close/mutate them from core0.
    airplay_rtsp_request_hangup();
    airplay_rtp_stop();
    airplay_session_clear();
    airplay_release_speaker();
}

bool airplay_accepts() {
    return s_ap_accept && RadioConfig::airplayEnable;
}

void airplay_set_accept(bool on) {
    AirPlayAudioGuard guard;
    s_ap_accept = on && RadioConfig::airplayEnable;
    if (!s_ap_accept) {
        airplay_interrupt();
        unadvertise();
    }
}

bool airplay_owns_speaker() {
    return g_ap_owns;
}

bool airplay_playing() {
    return g_ap.playing && g_ap_owns;
}

static int airplay_vmax() {
    return RadioConfig::ampVolumeUiMax > 0 ? RadioConfig::ampVolumeUiMax : 21;
}

int airplay_db_to_ui(float db) {
    const int vmax = airplay_vmax();
    if (db <= -144.0f) {
        return 0;
    }
    if (db >= 0.0f) {
        return vmax;
    }
    const float t = (db + 30.0f) / 30.0f;
    int v = (int)(t * (float)vmax + 0.5f);
    if (v < 0) {
        v = 0;
    }
    if (v > vmax) {
        v = vmax;
    }
    return v;
}

float airplay_ui_to_db(int vol) {
    const int vmax = airplay_vmax();
    if (vol <= 0) {
        return -144.0f;
    }
    if (vol >= vmax) {
        return 0.0f;
    }
    return -30.0f + 30.0f * ((float)vol / (float)vmax);
}

float airplay_db_to_gain(float db) {
    if (db <= -144.0f) {
        return 0.0f;
    }
    float g = powf(10.0f, db / 20.0f);
    if (g < 0.02f) {
        g = 0.02f;
    }
    if (g > 1.0f) {
        g = 1.0f;
    }
    return g;
}

void airplay_encoder_vol_changed() {
    int vol = radioState.vol;
    const int vmax = airplay_vmax();
    if (vol < 0) {
        vol = 0;
    }
    if (vol > vmax) {
        vol = vmax;
    }
    const float db = airplay_ui_to_db(vol);
    airplay_dacp_request(db);
}
