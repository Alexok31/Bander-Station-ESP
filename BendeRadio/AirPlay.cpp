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

#include "AirPlayPriv.h"
#include "BenderAi.h"
#include "RadioConfig.h"
#include "core0.h"

AirPlaySess g_ap;
WiFiServer g_ap_rtsp(RadioConfig::airplayRtspPort);
WiFiClient g_ap_cli;
WiFiUDP g_ap_audio;
WiFiUDP g_ap_ctrl;
WiFiUDP g_ap_time;
volatile bool g_ap_owns = false;
uint8_t g_ap_mac[6]{};

static TaskHandle_t s_rtp_task = nullptr;
static TaskHandle_t s_play_task = nullptr;
static TaskHandle_t s_rtsp_task = nullptr;
static bool s_begun = false;

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
static volatile bool s_play_armed = false;
static uint32_t s_under_n = 0;

static uint32_t ring_count() {
    return s_ring_w - s_ring_r;
}

static void ring_reset() {
    s_ring_w = 0;
    s_ring_r = 0;
    s_play_armed = false;
    s_pq_w = 0;
    s_pq_r = 0;
}

static void audio_recv_cb(void*, struct udp_pcb*, struct pbuf* p, const ip_addr_t*, u16_t) {
    if (!p) {
        return;
    }
    if (!s_pktq || (s_pq_w - s_pq_r) >= (uint32_t)kPktQ) {
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
    pbuf_free(p);
}

static int pktq_pop(uint8_t* out, int maxn) {
    if (!s_pktq || s_pq_w == s_pq_r) {
        return 0;
    }
    const ApUdpPkt* s = &s_pktq[s_pq_r % (uint32_t)kPktQ];
    int n = (int)s->n;
    if (n > maxn) {
        n = maxn;
    }
    memcpy(out, s->data, (size_t)n);
    s_pq_r++;
    return n;
}

static void audio_pcb_open() {
    if (s_audio_pcb) {
        return;
    }
    LOCK_TCPIP_CORE();
    s_audio_pcb = udp_new();
    if (s_audio_pcb) {
        udp_bind(s_audio_pcb, IP_ANY_TYPE, g_ap.audio_port);
        udp_recv(s_audio_pcb, audio_recv_cb, nullptr);
    }
    UNLOCK_TCPIP_CORE();
    if (!s_audio_pcb) {
        Serial.println(F("[AirPlay] UDP pcb fail"));
    }
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
    if (g_ap_owns) {
        return true;
    }
    if (bender_ai_owns_speaker()) {
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
    audio.i2sSetSampleRateHz(44100);
    airplay_hw_mute(false);
    g_ap_owns = true;
    bender_ai_sleep();
    Serial.printf("[AirPlay] I2S0 taken vol=%d\n", (int)radioState.vol);
    return true;
}

void airplay_release_speaker() {
    if (!g_ap_owns) {
        return;
    }
    airplay_hw_mute(true);
    g_ap_owns = false;
    apply_output_volume();
    if (radioState.state && strcmp(g_audio_source, "wifi") == 0) {
        reconnect = station_url_for_current();
    }
    Serial.println(F("[AirPlay] I2S0 back to radio"));
}

void airplay_write_pcm(int16_t* stereo, int frames) {
    if (!stereo || frames <= 0 || !g_ap_owns) {
        return;
    }
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
    const uint8_t* p = reinterpret_cast<const uint8_t*>(stereo);
    size_t left = (size_t)n * 2;
    size_t off = 0;
    uint8_t spins = 0;
    while (off < left) {
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

void airplay_rtp_flush() {
    ring_reset();
}

void airplay_session_clear() {
    g_ap.session = false;
    g_ap.playing = false;
    g_ap.encrypted = false;
    g_ap.vol_gain = 1.0f;
    g_ap.enc_touched = false;
    g_ap.phone_vol_seen = false;
    ring_reset();
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
    if (n <= 12) {
        return;
    }
    const uint8_t type = pkt[1] & 0x7f;
    if (type != 0x60) {
        return;
    }
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
        g_ap.last_rtp_ms = millis();
        g_ap.playing = true;
        ring_push(pcm, outn / 4);
    }
}

static void play_task(void*) {
    int16_t* pcm = (int16_t*)malloc((size_t)kApFrameBytes);
    if (!pcm) {
        Serial.println(F("[AirPlay] play alloc fail"));
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        if (!g_ap.session || !g_ap_owns) {
            s_play_armed = false;
            vTaskDelay(50);
            continue;
        }
        if (!s_play_armed) {
            if (ring_count() < (uint32_t)kApPrebuf) {
                vTaskDelay(1);
                continue;
            }
            airplay_hw_mute(false);
            s_play_armed = true;
        }
        if (ring_pop(pcm)) {
            airplay_write_pcm(pcm, kApFrameSamp);
            if (ring_count() < (uint32_t)(kApPrebuf / 2)) {
                vTaskDelay(1);
            }
            continue;
        }
        ++s_under_n;
        vTaskDelay(1);
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
    uint8_t* pkt = (uint8_t*)malloc(1600);
    uint8_t* sync = (uint8_t*)malloc(1600);
    uint8_t* dec = (uint8_t*)malloc(1600);
    int16_t* pcm = (int16_t*)malloc((size_t)kApFrameBytes);
    if (!pkt || !sync || !dec || !pcm) {
        Serial.println(F("[AirPlay] RTP alloc fail"));
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        if (!g_ap.session) {
            vTaskDelay(50);
            continue;
        }
        service_sync(sync);
        int got = 0;
        for (int i = 0; i < 48; i++) {
            const int n = pktq_pop(pkt, 1600);
            if (n <= 12) {
                break;
            }
            play_rtp_audio(pkt, n, dec, pcm);
            ++got;
            if ((i & 7) == 7) {
                service_sync(sync);
            }
        }
        int n = g_ap_ctrl.parsePacket();
        if (n > 16 && n <= 1600) {
            n = g_ap_ctrl.read(pkt, n);
            if ((pkt[1] & 0x7f) == 0x56) {
                play_rtp_audio(pkt + 4, n - 4, dec, pcm);
            }
        }
        if (got) {
            continue;
        }
        vTaskDelay(1);
    }
}

void airplay_rtp_start() {
    if (!s_ring) {
        const size_t nbytes = (size_t)kApRing * (size_t)kApFrameBytes;
        s_ring = (int16_t*)heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) {
            s_ring = (int16_t*)malloc(nbytes);
        }
        if (!s_ring) {
            Serial.println(F("[AirPlay] ring alloc fail"));
            return;
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
            return;
        }
        Serial.printf("[AirPlay] UDP q %u pkts\n", (unsigned)kPktQ);
    }
    ring_reset();
    audio_pcb_open();
    g_ap_ctrl.begin(g_ap.ctrl_port);
    g_ap_time.begin(g_ap.time_port);
    if (!s_rtp_task) {
        xTaskCreatePinnedToCore(rtp_task, "ap_rtp", 12288, nullptr, 5, &s_rtp_task, 1);
    }
    if (!s_play_task) {
        xTaskCreatePinnedToCore(play_task, "ap_play", 4096, nullptr, 6, &s_play_task, 1);
    }
}

void airplay_rtp_stop() {
    ring_reset();
    audio_pcb_close();
    g_ap_ctrl.stop();
    g_ap_time.stop();
}

static bool advertise() {
    if (g_ap.advertised || WiFi.status() != WL_CONNECTED) {
        return g_ap.advertised;
    }
    WiFi.macAddress(g_ap_mac);
    if (!MDNS.begin(RadioConfig::airplayName)) {
        Serial.println(F("[AirPlay] MDNS.begin fail"));
        return false;
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
        {"md", "0"},
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
            if (!g_ap.advertised) {
                advertise();
            }
            airplay_rtsp_poll();
            airplay_dacp_poll();
            if (g_ap.advertised && (uint32_t)(millis() - last_mdns) >= 20000u) {
                last_mdns = millis();
                mdns_refresh();
                mdns_service_txt_item_set("_raop", "_tcp", "vs", "105.1");
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
    airplay_session_clear();
    airplay_rtp_stop();
    airplay_rtsp_hangup();
    airplay_release_speaker();
}

bool airplay_owns_speaker() {
    return g_ap_owns;
}

bool airplay_playing() {
    return g_ap.playing && g_ap_owns;
}

void airplay_encoder_vol_changed() {
    g_ap.enc_touched = true;
    const int vmax = RadioConfig::ampVolumeUiMax > 0 ? RadioConfig::ampVolumeUiMax : 21;
    int vol = radioState.vol;
    if (vol < 0) {
        vol = 0;
    }
    if (vol > vmax) {
        vol = vmax;
    }
    g_ap.vol_gain = vol <= 0 ? 0.0f : (float)vol / (float)vmax;
    const float db = g_ap.vol_gain <= 0.0001f ? -144.0f : 20.0f * log10f(g_ap.vol_gain);
    airplay_dacp_request(db);
}
