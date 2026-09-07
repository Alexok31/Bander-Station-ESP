#include "AirPlay.h"
#include "AirPlayPriv.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_netif.h>
#include <mdns.h>

#include "BenderAi.h"
#include "RadioConfig.h"
#include "core0.h"

static constexpr int kRtspClients = 4;
static WiFiClient s_cli[kRtspClients];
static char s_acc[kRtspClients][3072];
static size_t s_accn[kRtspClients] = {0};
static uint32_t s_dead_ms[kRtspClients] = {0};
static int s_rep = 0;
static char s_sess_id[24] = "1";
static bool s_body_truncated = false;
static char s_dacp_id[28] = "";
static char s_active_remote[24] = "";
static IPAddress s_dacp_ip;
static uint16_t s_dacp_port = 0;
static volatile bool s_dacp_pending = false;
static volatile float s_dacp_db = 0;
static uint32_t s_dacp_quiet_ms = 0;
static uint32_t s_enc_guard_ms = 0;
static float s_enc_db = 0;

void airplay_rtsp_hangup() {
    for (int i = 0; i < kRtspClients; i++) {
        if (s_cli[i]) {
            s_cli[i].stop();
        }
        s_accn[i] = 0;
        s_dead_ms[i] = 0;
    }
    if (g_ap_cli) {
        g_ap_cli.stop();
    }
    s_dacp_id[0] = 0;
    s_active_remote[0] = 0;
    s_dacp_port = 0;
    s_dacp_pending = false;
    s_enc_guard_ms = 0;
}

bool airplay_rtsp_alive() {
    for (int i = 0; i < kRtspClients; i++) {
        if (s_cli[i] && s_cli[i].connected()) {
            return true;
        }
    }
    return false;
}

static const char* hdr_find(const char* req, const char* name) {
    const size_t nlen = strlen(name);
    const char* p = req;
    while (p && *p) {
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ') {
                ++p;
            }
            return p;
        }
        p = strchr(p, '\n');
        if (!p) {
            break;
        }
        ++p;
    }
    return nullptr;
}

static int hdr_int(const char* req, const char* name, int def = 0) {
    const char* v = hdr_find(req, name);
    return v ? atoi(v) : def;
}

static void hdr_copy_line(const char* v, char* out, size_t outn) {
    if (!out || outn == 0) {
        return;
    }
    out[0] = 0;
    if (!v) {
        return;
    }
    size_t i = 0;
    while (v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < outn) {
        out[i] = v[i];
        ++i;
    }
    out[i] = 0;
}

static bool sdp_copy(const char* body, const char* key, char* out, size_t outn) {
    if (!body || !out || outn == 0) {
        return false;
    }
    char pat[40];
    snprintf(pat, sizeof(pat), "a=%s:", key);
    const char* p = strstr(body, pat);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    ++p;
    size_t i = 0;
    while (*p && i + 1 < outn) {
        if (*p == '\r' || *p == '\n') {
            const char* n = p;
            while (*n == '\r' || *n == '\n') {
                ++n;
            }
            if (*n == ' ' || *n == '\t') {
                p = n + 1;
                continue;
            }
            break;
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

static void reply(int cseq, const char* extra, const char* body = "") {
    WiFiClient& c = s_cli[s_rep];
    if (!c) {
        return;
    }
    char apple[400] = "";
    const char* ch = hdr_find(s_acc[s_rep], "Apple-Challenge");
    if (ch) {
        char chs[128];
        hdr_copy_line(ch, chs, sizeof(chs));
        uint8_t raw[64];
        const size_t n = airplay_b64_decode(chs, raw, sizeof(raw));
        char b64[400];
        if (n && airplay_apple_response(raw, n, b64, sizeof(b64))) {
            snprintf(apple, sizeof(apple), "Apple-Response: %s\r\n", b64);
        }
    }
    const int blen = body ? (int)strlen(body) : 0;
    char sess[48] = "";
    if (g_ap.session) {
        snprintf(sess, sizeof(sess), "Session: %s;timeout=7200\r\n", s_sess_id);
    }
    c.printf(
        "RTSP/1.0 200 OK\r\n"
        "Server: AirTunes/105.1\r\n"
        "CSeq: %d\r\n"
        "%s"
        "%s"
        "%s"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        cseq, apple, sess, extra ? extra : "", blen, body ? body : "");
}

static void handle_announce(const char* body) {
    char fmtp[96] = "";
    if (sdp_copy(body, "fmtp", fmtp, sizeof(fmtp))) {
        sscanf(fmtp, "%d %d %d %d %d %d %d %d %d %d %d %d", &g_ap.fmtp[0], &g_ap.fmtp[1],
               &g_ap.fmtp[2], &g_ap.fmtp[3], &g_ap.fmtp[4], &g_ap.fmtp[5], &g_ap.fmtp[6],
               &g_ap.fmtp[7], &g_ap.fmtp[8], &g_ap.fmtp[9], &g_ap.fmtp[10], &g_ap.fmtp[11]);
    } else {
        g_ap.fmtp[1] = 352;
        g_ap.fmtp[3] = 16;
        g_ap.fmtp[4] = 40;
        g_ap.fmtp[5] = 10;
        g_ap.fmtp[6] = 14;
        g_ap.fmtp[7] = 2;
        g_ap.fmtp[11] = 44100;
    }
    if (g_ap.alac) {
        alac_free(g_ap.alac);
        g_ap.alac = nullptr;
    }
    const int bits = g_ap.fmtp[3] > 0 ? g_ap.fmtp[3] : 16;
    const int ch = g_ap.fmtp[7] > 0 ? g_ap.fmtp[7] : 2;
    g_ap.alac = alac_create(bits, ch);
    if (g_ap.alac) {
        g_ap.alac->setinfo_max_samples_per_frame = (uint32_t)(g_ap.fmtp[1] > 0 ? g_ap.fmtp[1] : 352);
        g_ap.alac->setinfo_7a = (uint8_t)g_ap.fmtp[2];
        g_ap.alac->setinfo_sample_size = (uint8_t)bits;
        g_ap.alac->setinfo_rice_historymult = (uint8_t)g_ap.fmtp[4];
        g_ap.alac->setinfo_rice_initialhistory = (uint8_t)g_ap.fmtp[5];
        g_ap.alac->setinfo_rice_kmodifier = (uint8_t)g_ap.fmtp[6];
        g_ap.alac->setinfo_7f = (uint8_t)ch;
        g_ap.alac->setinfo_80 = (uint16_t)g_ap.fmtp[8];
        g_ap.alac->setinfo_82 = (uint32_t)g_ap.fmtp[9];
        g_ap.alac->setinfo_86 = (uint32_t)g_ap.fmtp[10];
        g_ap.alac->setinfo_8a_rate = (uint32_t)(g_ap.fmtp[11] > 0 ? g_ap.fmtp[11] : 44100);
        alac_allocate_buffers(g_ap.alac);
    }

    g_ap.encrypted = false;
    char keyb[520] = "";
    char ivb[64] = "";
    const bool has_key = sdp_copy(body, "rsaaeskey", keyb, sizeof(keyb));
    const bool has_iv = sdp_copy(body, "aesiv", ivb, sizeof(ivb));
    if (has_key && has_iv) {
        uint8_t keyc[256];
        uint8_t iv[32];
        const size_t kn = airplay_b64_decode(keyb, keyc, sizeof(keyc));
        const size_t ivn = airplay_b64_decode(ivb, iv, sizeof(iv));
        uint8_t plain[256];
        size_t plen = 0;
        Serial.printf("[AirPlay] keyb64=%u kn=%u ivn=%u\n", (unsigned)strlen(keyb), (unsigned)kn,
                      (unsigned)ivn);
        if (kn == 256 && ivn >= 16 && airplay_rsa_oaep_decrypt(keyc, kn, plain, &plen)) {
            memcpy(g_ap.aes_key, plain, 16);
            memcpy(g_ap.aes_iv, iv, 16);
            g_ap.encrypted = true;
            airplay_aes_prepare();
        }
    }
    Serial.printf("[AirPlay] ANNOUNCE alac=%d enc=%d frame=%d ct=%.40s\n", g_ap.alac ? 1 : 0,
                  g_ap.encrypted ? 1 : 0, g_ap.fmtp[1], body);
}

static void handle_setup(const char* req) {
    char tr[192];
    hdr_copy_line(hdr_find(req, "Transport"), tr, sizeof(tr));
    int cport = 0, tport = 0;
    const char* p = strstr(tr, "control_port=");
    if (p) {
        cport = atoi(p + 13);
    }
    p = strstr(tr, "timing_port=");
    if (p) {
        tport = atoi(p + 12);
    }
    (void)cport;
    (void)tport;
    if (!airplay_take_speaker()) {
        Serial.println(F("[AirPlay] SETUP: speaker busy"));
    }
    airplay_rtp_start();
    g_ap.session = true;
    g_ap.last_rtp_ms = millis();
    const char* sid = hdr_find(req, "Session");
    if (sid) {
        hdr_copy_line(sid, s_sess_id, sizeof(s_sess_id));
        char* semi = strchr(s_sess_id, ';');
        if (semi) {
            *semi = 0;
        }
    } else {
        strncpy(s_sess_id, "1", sizeof(s_sess_id) - 1);
        s_sess_id[sizeof(s_sess_id) - 1] = 0;
    }
    char extra[240];
    snprintf(extra, sizeof(extra),
             "Audio-Jack-Status: connected;\r\n"
             "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;control_port=%u;"
             "timing_port=%u\r\n",
             (unsigned)g_ap.audio_port, (unsigned)g_ap.ctrl_port, (unsigned)g_ap.time_port);
    reply(hdr_int(req, "CSeq"), extra);
    Serial.println(F("[AirPlay] SETUP"));
}

static void note_dacp_headers(const char* req, WiFiClient& c) {
    const char* ar = hdr_find(req, "Active-Remote");
    const char* di = hdr_find(req, "DACP-ID");
    if (ar) {
        hdr_copy_line(ar, s_active_remote, sizeof(s_active_remote));
    }
    if (di) {
        char prev[28];
        strncpy(prev, s_dacp_id, sizeof(prev) - 1);
        prev[sizeof(prev) - 1] = 0;
        hdr_copy_line(di, s_dacp_id, sizeof(s_dacp_id));
        if (s_dacp_id[0] && strcmp(prev, s_dacp_id) != 0) {
            s_dacp_port = 0;
            Serial.printf("[AirPlay] DACP-ID %s remote=%s\n", s_dacp_id, s_active_remote);
        }
    }
    if (c) {
        s_dacp_ip = c.remoteIP();
    }
}

static bool dacp_resolve() {
    if (s_dacp_port || !s_dacp_id[0]) {
        return s_dacp_port != 0;
    }
    char inst[48];
    snprintf(inst, sizeof(inst), "iTunes_Ctrl_%s", s_dacp_id);
    mdns_result_t* res = nullptr;
    if (mdns_query_srv(inst, "_dacp", "_tcp", 280, &res) != ESP_OK || !res) {
        return false;
    }
    s_dacp_port = res->port;
    for (mdns_ip_addr_t* a = res->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            s_dacp_ip = IPAddress(a->addr.u_addr.ip4.addr);
            break;
        }
    }
    mdns_query_results_free(res);
    Serial.printf("[AirPlay] DACP %s:%u id=%s\n", s_dacp_ip.toString().c_str(),
                  (unsigned)s_dacp_port, s_dacp_id);
    return s_dacp_port != 0;
}

static void dacp_send_volume(float db) {
    if (!s_active_remote[0]) {
        Serial.println(F("[AirPlay] DACP: no Active-Remote"));
        return;
    }
    if (!dacp_resolve() && s_dacp_port == 0) {
        s_dacp_port = 3689;
    }
    if (!s_dacp_ip) {
        Serial.println(F("[AirPlay] DACP: no phone IP"));
        return;
    }
    WiFiClient c;
    c.setTimeout(300);
    if (!c.connect(s_dacp_ip, s_dacp_port, 300)) {
        Serial.printf("[AirPlay] DACP connect %s:%u fail\n", s_dacp_ip.toString().c_str(),
                      (unsigned)s_dacp_port);
        s_dacp_port = 0;
        return;
    }
    c.printf("GET /ctrl-int/1/setproperty?dmcp.device-volume=%.6f HTTP/1.1\r\n"
             "Host: %s:%u\r\n"
             "Active-Remote: %s\r\n"
             "\r\n",
             db, s_dacp_ip.toString().c_str(), (unsigned)s_dacp_port, s_active_remote);
    const uint32_t t0 = millis();
    while (c.connected() && (uint32_t)(millis() - t0) < 200u) {
        if (c.available()) {
            while (c.available()) {
                (void)c.read();
            }
            break;
        }
        delay(1);
    }
    c.stop();
}

void airplay_dacp_request(float db) {
    s_dacp_db = db;
    s_enc_db = db;
    s_enc_guard_ms = millis();
    s_dacp_quiet_ms = millis();
    s_dacp_pending = true;
}

void airplay_dacp_poll() {
    if (!s_dacp_pending || !g_ap.session) {
        return;
    }
    if ((uint32_t)(millis() - s_dacp_quiet_ms) < 90u) {
        return;
    }
    s_dacp_pending = false;
    dacp_send_volume(s_dacp_db);
}

static void apply_volume_db(float db) {
    Serial.printf("[AirPlay] volume %.1f dB\n", db);
    if (s_enc_guard_ms && (uint32_t)(millis() - s_enc_guard_ms) < 450u) {
        const float d = db - s_enc_db;
        if (d > 1.5f || d < -1.5f) {
            return;
        }
    }
    if (db <= -144.0f) {
        if (g_ap.phone_vol_seen) {
            g_ap.vol_gain = 0;
            if (radioState.vol != 0) {
                radioState.vol = 0;
                matrix_show_volume(0);
            }
        }
        return;
    }
    g_ap.phone_vol_seen = true;
    float g = powf(10.0f, db / 20.0f);
    if (g < 0.02f) {
        g = 0.02f;
    }
    if (g > 1.0f) {
        g = 1.0f;
    }
    g_ap.vol_gain = g;
    const int vmax = RadioConfig::ampVolumeUiMax > 0 ? RadioConfig::ampVolumeUiMax : 21;
    const int8_t nv = (int8_t)constrain((int)(g * (float)vmax + 0.5f), 0, vmax);
    if (nv != radioState.vol) {
        radioState.vol = nv;
        matrix_show_volume(nv);
    }
}

static bool bplist_volume_db(const uint8_t* b, size_t n, float* db) {
    if (!b || !db || n < 16 || memcmp(b, "bplist00", 8) != 0) {
        return false;
    }
    for (size_t i = 0; i + 14 < n; i++) {
        if (b[i] != 'v' || memcmp(b + i, "volume", 6) != 0) {
            continue;
        }
        for (size_t j = i + 6; j + 9 < n && j < i + 96; j++) {
            if (b[j] == 0x23) {
                uint64_t u = 0;
                for (int k = 0; k < 8; k++) {
                    u = (u << 8) | b[j + 1 + k];
                }
                double d = 0;
                memcpy(&d, &u, 8);
                *db = (float)d;
                return true;
            }
            if (b[j] == 0x22 && j + 5 < n) {
                uint32_t u = ((uint32_t)b[j + 1] << 24) | ((uint32_t)b[j + 2] << 16) |
                             ((uint32_t)b[j + 3] << 8) | (uint32_t)b[j + 4];
                float f = 0;
                memcpy(&f, &u, 4);
                *db = f;
                return true;
            }
        }
    }
    return false;
}

static void handle_volume(const char* body, size_t body_n, const char* req) {
    if (s_body_truncated) {
        return;
    }
    float db = 0;
    bool ok = false;
    const char* v = strcasestr(body ? body : "", "volume:");
    if (v) {
        db = atof(v + 7);
        ok = true;
    } else if (bplist_volume_db(reinterpret_cast<const uint8_t*>(body), body_n, &db)) {
        ok = true;
    }
    if (!ok) {
        char ct[48] = "";
        hdr_copy_line(hdr_find(req, "Content-Type"), ct, sizeof(ct));
        Serial.printf("[AirPlay] SET_PARAMETER ct=%.40s n=%u\n", ct, (unsigned)body_n);
        return;
    }
    apply_volume_db(db);
}

static void dispatch(char* req, size_t len) {
    char* blank = strstr(req, "\r\n\r\n");
    const char* body = blank ? blank + 4 : "";
    char method[16] = "";
    sscanf(req, "%15s", method);
    const int cseq = hdr_int(req, "CSeq");
    if (s_rep >= 0 && s_rep < kRtspClients) {
        note_dacp_headers(req, s_cli[s_rep]);
    }
    if (strcmp(method, "OPTIONS") != 0) {
        Serial.printf("[AirPlay] %s cseq=%d n=%u\n", method, cseq, (unsigned)len);
    }
    if (!strcmp(method, "OPTIONS")) {
        reply(cseq, "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, "
                    "GET_PARAMETER, SET_PARAMETER, POST\r\n");
    } else if (!strcmp(method, "ANNOUNCE")) {
        handle_announce(body);
        reply(cseq, "");
    } else if (!strcmp(method, "SETUP")) {
        handle_setup(req);
    } else if (!strcmp(method, "RECORD")) {
        g_ap.playing = true;
        g_ap.last_rtp_ms = millis();
        wifi_touch_activity();
        reply(cseq, "Audio-Latency: 31680\r\n");
        Serial.println(F("[AirPlay] RECORD"));
    } else if (!strcmp(method, "FLUSH") || !strcmp(method, "PAUSE")) {
        airplay_rtp_flush();
        g_ap.last_rtp_ms = millis();
        reply(cseq, "");
    } else if (!strcmp(method, "TEARDOWN")) {
        reply(cseq, "");
        airplay_interrupt();
        Serial.println(F("[AirPlay] TEARDOWN"));
    } else if (!strcmp(method, "SET_PARAMETER")) {
        const char* blank = strstr(req, "\r\n\r\n");
        const size_t body_n = blank ? (len > (size_t)(blank + 4 - req) ? len - (size_t)(blank + 4 - req) : 0) : 0;
        handle_volume(body, body_n, req);
        reply(cseq, "");
    } else if (!strcmp(method, "GET_PARAMETER")) {
        char vol[40];
        snprintf(vol, sizeof(vol), "volume: %.6f\r\n",
                 g_ap.vol_gain <= 0.0001f ? -144.0f : 20.0f * log10f(g_ap.vol_gain));
        reply(cseq, "Content-Type: text/parameters\r\n", vol);
    } else if (!strcmp(method, "POST")) {
        reply(cseq, "");
    } else {
        reply(cseq, "");
    }
}

static void drain_bytes(WiFiClient& c, size_t n) {
    uint8_t tmp[128];
    const uint32_t t0 = millis();
    while (n && c.connected() && (uint32_t)(millis() - t0) < 2500u) {
        if (!c.available()) {
            delay(1);
            continue;
        }
        const size_t chunk = n < sizeof(tmp) ? n : sizeof(tmp);
        const int r = c.read(tmp, chunk);
        if (r <= 0) {
            break;
        }
        n -= (size_t)r;
    }
}

static void poll_one(int i) {
    if (!s_cli[i]) {
        return;
    }
    while (s_cli[i].available() && s_accn[i] + 1 < sizeof(s_acc[i])) {
        s_acc[i][s_accn[i]++] = (char)s_cli[i].read();
    }
    s_acc[i][s_accn[i]] = 0;
    for (int guard = 0; guard < 6; guard++) {
        char* blank = strstr(s_acc[i], "\r\n\r\n");
        if (!blank) {
            if (s_accn[i] + 1 >= sizeof(s_acc[i])) {
                s_accn[i] = 0;
            }
            return;
        }
        int clen = hdr_int(s_acc[i], "Content-Length");
        if (clen < 0) {
            clen = 0;
        }
        const size_t header_n = (size_t)(blank - s_acc[i]) + 4;
        const size_t need = header_n + (size_t)clen;
        if (need + 1 > sizeof(s_acc[i])) {
            Serial.printf("[AirPlay] RTSP body %d — drain\n", clen);
            s_rep = i;
            s_body_truncated = true;
            dispatch(s_acc[i], s_accn[i]);
            s_body_truncated = false;
            const size_t have = s_accn[i] > header_n ? s_accn[i] - header_n : 0;
            if ((size_t)clen > have) {
                drain_bytes(s_cli[i], (size_t)clen - have);
            }
            s_accn[i] = 0;
            return;
        }
        if (s_accn[i] < need) {
            return;
        }
        s_rep = i;
        dispatch(s_acc[i], need);
        if (need < s_accn[i]) {
            memmove(s_acc[i], s_acc[i] + need, s_accn[i] - need);
            s_accn[i] -= need;
            s_acc[i][s_accn[i]] = 0;
        } else {
            s_accn[i] = 0;
            return;
        }
    }
}

void airplay_rtsp_poll() {
    for (int i = 0; i < kRtspClients; i++) {
        if (s_cli[i]) {
            const bool alive = s_cli[i].connected() || s_cli[i].available();
            if (alive) {
                s_dead_ms[i] = 0;
                continue;
            }
            if (!s_dead_ms[i]) {
                s_dead_ms[i] = millis();
                continue;
            }
            if ((uint32_t)(millis() - s_dead_ms[i]) < 20000u) {
                continue;
            }
            s_cli[i].stop();
            s_accn[i] = 0;
            s_dead_ms[i] = 0;
        }
        WiFiClient c = g_ap_rtsp.accept();
        if (c) {
            c.setNoDelay(true);
            c.setTimeout(400);
            s_cli[i] = c;
            s_accn[i] = 0;
            s_dead_ms[i] = 0;
            Serial.printf("[AirPlay] RTSP client%d %s\n", i, c.remoteIP().toString().c_str());
        }
    }
    for (int i = 0; i < kRtspClients; i++) {
        poll_one(i);
    }
}
