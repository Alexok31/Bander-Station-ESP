#include "AirPlay.h"
#include "AirPlayPriv.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
static char s_dacp_cmd[40] = "";
static volatile bool s_dacp_cmd_pend = false;

static char s_title[64] = "";
static char s_artist[48] = "";
static volatile uint32_t s_meta_serial = 1;
static volatile uint32_t s_dur_ms = 0;
static volatile uint32_t s_anchor_pos_ms = 0;
static volatile uint32_t s_anchor_wall_ms = 0;
static volatile bool s_clock_run = false;
static uint32_t s_rtp_start = 0;
static uint32_t s_rtp_end = 0;
static bool s_rtp_span = false;
static bool s_have_npt = false;
static uint32_t s_npt_ms = 0;
static bool s_have_ts = false;
static uint32_t s_last_ts = 0;
static uint32_t s_walk_cant = 0;
static bool s_walk_got_cant = false;
static uint8_t s_walk_caps = 0;
static bool s_walk_got_caps = false;
static volatile bool s_dacp_time_pend = false;
static uint32_t s_dacp_time_ms = 0;
static volatile bool s_seek_hold = false;

static uint32_t be32u(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t be_u32(const uint8_t* p, size_t n) {
    if (!p || n == 0) {
        return 0;
    }
    if (n > 8) {
        n = 8;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    if (v > 0xFFFFFFFFull) {
        v = 0xFFFFFFFFull;
    }
    return (uint32_t)v;
}

static void copy_txt(char* dst, size_t dstn, const uint8_t* s, size_t n) {
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < dstn; i++) {
        const uint8_t c = s[i];
        if (c < 32) {
            if ((c == '\t' || c == '\n' || c == '\r') && j && dst[j - 1] != ' ') {
                dst[j++] = ' ';
            }
            continue;
        }
        dst[j++] = (char)c;
    }
    while (j && dst[j - 1] == ' ') {
        --j;
    }
    dst[j] = 0;
}

static bool tag_eq(const uint8_t* p, const char* t) {
    return p[0] == (uint8_t)t[0] && p[1] == (uint8_t)t[1] && p[2] == (uint8_t)t[2] &&
           p[3] == (uint8_t)t[3];
}

static void snap_pos_ms(uint32_t pos, uint32_t dur);

static void apply_walk_times() {
    if (s_walk_got_cant && s_dur_ms > 1u) {
        uint32_t pos = 0;
        if (s_walk_cant < s_dur_ms) {
            pos = s_dur_ms - s_walk_cant;
        }
        snap_pos_ms(pos, s_dur_ms);
        s_seek_hold = false;
        if (s_walk_got_caps && (s_walk_caps == 3u || s_walk_caps == 2u)) {
            s_clock_run = false;
        } else {
            s_clock_run = true;
        }
    }
}

static void dmap_walk_reset_times() {
    s_walk_cant = 0;
    s_walk_got_cant = false;
    s_walk_caps = 0;
    s_walk_got_caps = false;
}

static bool fourcc_ok(const uint8_t* p) {
    for (int i = 0; i < 4; i++) {
        const char c = (char)p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

static void dmap_walk(const uint8_t* p, size_t n, int depth) {
    if (!p || depth > 8) {
        return;
    }
    size_t i = 0;
    while (i + 8 <= n) {
        const uint8_t* tag = p + i;
        const uint32_t len = be32u(p + i + 4);
        i += 8;
        if (len > n - i) {
            break;
        }
        const uint8_t* d = p + i;
        if (tag_eq(tag, "minm")) {
            copy_txt(s_title, sizeof(s_title), d, len);
        } else if (tag_eq(tag, "asar")) {
            copy_txt(s_artist, sizeof(s_artist), d, len);
        } else if (tag_eq(tag, "cann")) {
            copy_txt(s_title, sizeof(s_title), d, len);
        } else if (tag_eq(tag, "cana")) {
            copy_txt(s_artist, sizeof(s_artist), d, len);
        } else if (tag_eq(tag, "astm") && len > 0) {
            const uint32_t ms = be_u32(d, len);
            if (ms > 1u) {
                s_dur_ms = ms;
            }
        } else if (tag_eq(tag, "cast") && len > 0) {
            const uint32_t ms = be_u32(d, len);
            if (ms > 1u) {
                s_dur_ms = ms;
            }
        } else if (tag_eq(tag, "cant") && len > 0) {
            s_walk_cant = be_u32(d, len);
            s_walk_got_cant = true;
        } else if (tag_eq(tag, "caps") && len > 0) {
            s_walk_caps = (uint8_t)be_u32(d, len);
            s_walk_got_caps = true;
        } else if (len >= 8 && fourcc_ok(d)) {
            dmap_walk(d, len, depth + 1);
        }
        i += len;
    }
}

static uint32_t rtp_to_ms(uint32_t samples) {
    uint32_t sr = (uint32_t)g_ap.fmtp[11];
    if (sr < 8000u) {
        sr = 44100u;
    }
    return (uint32_t)((uint64_t)samples * 1000ull / (uint64_t)sr);
}

static uint32_t ms_to_rtp(uint32_t ms) {
    uint32_t sr = (uint32_t)g_ap.fmtp[11];
    if (sr < 8000u) {
        sr = 44100u;
    }
    return (uint32_t)((uint64_t)ms * (uint64_t)sr / 1000ull);
}

static void snap_pos_ms(uint32_t pos, uint32_t dur) {
    if (dur > 1u && pos >= dur) {
        pos = dur - 1u;
    }
    if (dur > 1u) {
        s_dur_ms = dur;
    }
    s_anchor_pos_ms = pos;
    s_anchor_wall_ms = millis();
}

static void ensure_span_from_ts(uint32_t ts) {
    if (s_rtp_span) {
        return;
    }
    const uint32_t pos_samp = s_have_npt ? ms_to_rtp(s_npt_ms) : 0u;
    s_rtp_start = ts - pos_samp;
    s_have_ts = true;
    s_last_ts = ts;
    if (s_dur_ms > 1u) {
        s_rtp_end = s_rtp_start + ms_to_rtp(s_dur_ms);
        s_rtp_span = true;
    }
}

static void snap_from_rtp(uint32_t rtp) {
    ensure_span_from_ts(rtp);
    const uint32_t dur =
        s_rtp_span ? rtp_to_ms(s_rtp_end - s_rtp_start) : (s_dur_ms > 1u ? s_dur_ms : 0u);
    const uint32_t pos = rtp_to_ms(rtp - s_rtp_start);
    snap_pos_ms(pos, dur);
}

static void apply_progress_rtp(uint32_t start, uint32_t curr, uint32_t end) {
    s_rtp_start = start;
    s_rtp_end = end;
    s_rtp_span = true;
    s_have_ts = true;
    s_last_ts = curr;
    snap_from_rtp(curr);
    Serial.printf("[AirPlay] progress %u/%u ms\n", (unsigned)s_anchor_pos_ms, (unsigned)s_dur_ms);
}

static void handle_progress_body(const char* body) {
    const char* p = strcasestr(body ? body : "", "progress:");
    if (!p) {
        p = strstr(body ? body : "", "progress");
        if (p) {
            p = strchr(p, ':');
        }
    }
    if (!p) {
        p = body;
    }
    if (*p == ':') {
        ++p;
    } else if (strncasecmp(p, "progress", 8) == 0) {
        p += 8;
        if (*p == ':') {
            ++p;
        }
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    unsigned long start = 0, curr = 0, end = 0;
    if (sscanf(p, "%lu/%lu/%lu", &start, &curr, &end) != 3) {
        return;
    }
    apply_progress_rtp((uint32_t)start, (uint32_t)curr, (uint32_t)end);
}

void airplay_meta_clear() {
    s_title[0] = 0;
    s_artist[0] = 0;
    s_dur_ms = 0;
    s_anchor_pos_ms = 0;
    s_anchor_wall_ms = millis();
    s_clock_run = false;
    s_rtp_start = 0;
    s_rtp_end = 0;
    s_rtp_span = false;
    s_have_npt = false;
    s_npt_ms = 0;
    s_have_ts = false;
    s_last_ts = 0;
    s_meta_serial++;
}

void airplay_meta_on_flush() {
    if (s_clock_run) {
        const uint32_t dur = s_dur_ms;
        uint64_t pos = (uint64_t)s_anchor_pos_ms + (uint64_t)(millis() - s_anchor_wall_ms);
        if (dur > 1u && pos >= dur) {
            pos = dur - 1u;
        }
        s_anchor_pos_ms = (uint32_t)pos;
        s_anchor_wall_ms = millis();
    }
    s_clock_run = false;
    s_seek_hold = true;
    s_dacp_time_pend = true;
}

void airplay_meta_on_play() {
    if (s_seek_hold) {
        return;
    }
    if (!s_clock_run) {
        s_anchor_wall_ms = millis();
    }
    s_clock_run = true;
}

void airplay_meta_seek_rtp(uint32_t rtptime) {
    snap_from_rtp(rtptime);
    s_clock_run = false;
}

void airplay_meta_on_rtp(uint32_t) {
    // RTP clock is not track position (seek does not jump timestamps).
}

void airplay_meta_set_npt_ms(uint32_t npt_ms) {
    s_npt_ms = npt_ms;
    s_have_npt = true;
    snap_pos_ms(npt_ms, s_dur_ms);
}

uint32_t airplay_track_duration_ms() {
    return s_dur_ms;
}

uint32_t airplay_track_position_ms() {
    const uint32_t dur = s_dur_ms;
    if (dur <= 1u) {
        return 0;
    }
    uint64_t pos = s_anchor_pos_ms;
    if (s_clock_run) {
        pos += (uint64_t)(millis() - s_anchor_wall_ms);
    }
    if (pos >= dur) {
        return dur - 1u;
    }
    return (uint32_t)pos;
}

uint32_t airplay_track_meta_serial() {
    return s_meta_serial;
}

static void utf8_to_matrix(const char* in, char* out, size_t outn) {
    if (!out || outn == 0) {
        return;
    }
    out[0] = 0;
    if (!in) {
        return;
    }
    size_t j = 0;
    auto put = [&](const char* s) {
        while (*s && j + 1 < outn) {
            out[j++] = *s++;
        }
    };
    for (size_t i = 0; in[i] && j + 1 < outn;) {
        const uint8_t c = (uint8_t)in[i];
        if (c < 0x80) {
            out[j++] = (char)c;
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && in[i + 1]) {
            const uint16_t u = (uint16_t)(((c & 0x1F) << 6) | ((uint8_t)in[i + 1] & 0x3F));
            i += 2;
            switch (u) {
                case 0x401:
                case 0x451:
                    put("E");
                    break;
                case 0x404:
                case 0x454:
                    put("Ye");
                    break;
                case 0x406:
                case 0x456:
                    put("I");
                    break;
                case 0x407:
                case 0x457:
                    put("Yi");
                    break;
                case 0x490:
                case 0x491:
                    put("G");
                    break;
                default: {
                    uint16_t base = u;
                    if (base >= 0x430 && base <= 0x44F) {
                        base = (uint16_t)(base - 0x20);
                    }
                    static const char* const ru[] = {"A",  "B", "V", "G", "D", "E", "Zh", "Z",
                                                     "I",  "Y", "K", "L", "M", "N", "O",  "P",
                                                     "R",  "S", "T", "U", "F", "H", "Ts", "Ch",
                                                     "Sh", "Sch", "", "Y", "", "E", "Yu", "Ya"};
                    if (base >= 0x410 && base <= 0x42F) {
                        put(ru[base - 0x410]);
                    }
                    break;
                }
            }
            continue;
        }
        i++;
    }
    out[j] = 0;
}

const char* airplay_track_scroll_cstr() {
    if (!s_title[0] && !s_artist[0]) {
        return "AIR";
    }
    static char raw[sizeof(s_title) + sizeof(s_artist) + 8];
    static char line[96];
    if (!s_artist[0]) {
        strncpy(raw, s_title, sizeof(raw) - 1);
    } else if (!s_title[0]) {
        strncpy(raw, s_artist, sizeof(raw) - 1);
    } else {
        snprintf(raw, sizeof(raw), "%s  |  %s", s_title, s_artist);
    }
    raw[sizeof(raw) - 1] = 0;
    utf8_to_matrix(raw, line, sizeof(line));
    if (!line[0]) {
        return raw;
    }
    return line;
}

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
    s_dacp_cmd_pend = false;
    s_dacp_time_pend = false;
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

static void reply(int cseq, const char* extra, const char* body = "", int code = 200) {
    WiFiClient& c = s_cli[s_rep];
    if (!c) {
        return;
    }
    const char* reason = "OK";
    if (code == 453) {
        reason = "Not Enough Bandwidth";
    } else if (code != 200) {
        reason = "Forbidden";
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
        "RTSP/1.0 %d %s\r\n"
        "Server: AirTunes/105.1\r\n"
        "CSeq: %d\r\n"
        "%s"
        "%s"
        "%s"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        code, reason, cseq, apple, sess, extra ? extra : "", blen, body ? body : "");
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
    if (!airplay_accepts()) {
        reply(hdr_int(req, "CSeq"), "", "", 453);
        Serial.println(F("[AirPlay] SETUP refused (radio mode)"));
        return;
    }
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
        reply(hdr_int(req, "CSeq"), "", "", 453);
        Serial.println(F("[AirPlay] SETUP: speaker busy"));
        return;
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

static void dacp_send_cmd(const char* cmd) {
    if (!cmd || !cmd[0]) {
        return;
    }
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
    c.printf("GET /ctrl-int/1/%s HTTP/1.1\r\n"
             "Host: %s:%u\r\n"
             "Active-Remote: %s\r\n"
             "\r\n",
             cmd, s_dacp_ip.toString().c_str(), (unsigned)s_dacp_port, s_active_remote);
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

static bool dacp_http_get(const char* path, uint8_t* out, size_t outmax, size_t* outn) {
    if (outn) {
        *outn = 0;
    }
    if (!path || !out || outmax < 16) {
        return false;
    }
    if (!s_active_remote[0]) {
        return false;
    }
    if (!dacp_resolve() && s_dacp_port == 0) {
        s_dacp_port = 3689;
    }
    if (!s_dacp_ip) {
        return false;
    }
    WiFiClient c;
    c.setTimeout(180);
    if (!c.connect(s_dacp_ip, s_dacp_port, 180)) {
        s_dacp_port = 0;
        return false;
    }
    c.printf("GET /ctrl-int/1/%s HTTP/1.1\r\n"
             "Host: %s:%u\r\n"
             "Active-Remote: %s\r\n"
             "\r\n",
             path, s_dacp_ip.toString().c_str(), (unsigned)s_dacp_port, s_active_remote);
    uint8_t buf[1400];
    size_t n = 0;
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 250u && n + 1 < sizeof(buf)) {
        while (c.available() && n + 1 < sizeof(buf)) {
            buf[n++] = (uint8_t)c.read();
        }
        buf[n] = 0;
        if (n >= 16) {
            char* blank = strstr(reinterpret_cast<char*>(buf), "\r\n\r\n");
            if (blank) {
                const size_t header_n = (size_t)(blank + 4 - reinterpret_cast<char*>(buf));
                int clen = 0;
                const char* cl = strcasestr(reinterpret_cast<char*>(buf), "Content-Length:");
                if (cl && cl < blank) {
                    clen = atoi(cl + 15);
                }
                if (clen < 0) {
                    clen = 0;
                }
                const size_t need = header_n + (size_t)clen;
                if (clen == 0 && n > header_n) {
                    break;
                }
                if (n >= need && need > header_n) {
                    n = need;
                    break;
                }
            }
        }
        delay(1);
    }
    c.stop();
    char* blank = strstr(reinterpret_cast<char*>(buf), "\r\n\r\n");
    if (!blank || n < 16) {
        return false;
    }
    const size_t header_n = (size_t)(blank + 4 - reinterpret_cast<char*>(buf));
    if (header_n >= n) {
        return false;
    }
    const size_t body_n = n - header_n;
    const size_t copy = body_n < outmax ? body_n : outmax;
    memcpy(out, buf + header_n, copy);
    if (outn) {
        *outn = copy;
    }
    return copy > 8;
}

static void dacp_fetch_playing_time() {
    uint8_t body[1024];
    size_t n = 0;
    if (!dacp_http_get("playstatusupdate?revision-number=1", body, sizeof(body), &n)) {
        if (!dacp_http_get("getproperty?properties=dacp.playingtime", body, sizeof(body), &n)) {
            static uint32_t s_fail_ms = 0;
            if (!s_fail_ms || (uint32_t)(millis() - s_fail_ms) > 8000u) {
                s_fail_ms = millis();
                Serial.println(F("[AirPlay] DACP time fail"));
            }
            return;
        }
    }
    const uint32_t old_pos = s_anchor_pos_ms;
    dmap_walk_reset_times();
    dmap_walk(body, n, 0);
    apply_walk_times();
    if (s_walk_got_cant && s_dur_ms > 1u && s_anchor_pos_ms != old_pos) {
        Serial.printf("[AirPlay] dacp time %u/%u\n", (unsigned)s_anchor_pos_ms, (unsigned)s_dur_ms);
    }
}

static void dacp_send_volume(float db) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "setproperty?dmcp.device-volume=%.6f", db);
    dacp_send_cmd(cmd);
}

void airplay_dacp_request(float db) {
    s_dacp_db = db;
    s_enc_db = db;
    s_enc_guard_ms = millis();
    s_dacp_quiet_ms = millis();
    s_dacp_pending = true;
}

void airplay_dacp_command(const char* cmd) {
    if (!cmd || !cmd[0]) {
        return;
    }
    if (!s_active_remote[0]) {
        Serial.println(F("[AirPlay] DACP: no Active-Remote"));
        return;
    }
    strncpy(s_dacp_cmd, cmd, sizeof(s_dacp_cmd) - 1);
    s_dacp_cmd[sizeof(s_dacp_cmd) - 1] = 0;
    s_dacp_cmd_pend = true;
    Serial.printf("[AirPlay] DACP %s\n", s_dacp_cmd);
}

void airplay_dacp_poll() {
    if (!s_active_remote[0]) {
        return;
    }
    if (s_dacp_pending) {
        if ((uint32_t)(millis() - s_dacp_quiet_ms) < 90u) {
            return;
        }
        s_dacp_pending = false;
        dacp_send_volume(s_dacp_db);
        return;
    }
    if (s_dacp_cmd_pend) {
        s_dacp_cmd_pend = false;
        dacp_send_cmd(s_dacp_cmd);
        return;
    }
    const bool due = s_dacp_time_pend ||
                     ((g_ap.session || g_ap_owns) && (uint32_t)(millis() - s_dacp_time_ms) >= 700u);
    if (due) {
        s_dacp_time_pend = false;
        s_dacp_time_ms = millis();
        dacp_fetch_playing_time();
    }
}

static void handle_dmap(const uint8_t* body, size_t n) {
    char old_t[sizeof(s_title)];
    char old_a[sizeof(s_artist)];
    const uint32_t old_dur = s_dur_ms;
    strncpy(old_t, s_title, sizeof(old_t) - 1);
    old_t[sizeof(old_t) - 1] = 0;
    strncpy(old_a, s_artist, sizeof(old_a) - 1);
    old_a[sizeof(old_a) - 1] = 0;
    dmap_walk_reset_times();
    dmap_walk(body, n, 0);
    apply_walk_times();
    if (strcmp(old_t, s_title) != 0 || strcmp(old_a, s_artist) != 0) {
        s_meta_serial++;
        Serial.printf("[AirPlay] track \"%s\" — %s\n", s_title[0] ? s_title : "-",
                      s_artist[0] ? s_artist : "-");
    }
    if (s_dur_ms != old_dur && s_dur_ms > 1u) {
        Serial.printf("[AirPlay] duration %u ms pos %u\n", (unsigned)s_dur_ms,
                      (unsigned)s_anchor_pos_ms);
    }
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
    g_ap.vol_gain = airplay_db_to_gain(db);
    const int8_t nv = (int8_t)airplay_db_to_ui(db);
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

static void scan_progress_bytes(const char* body, size_t n) {
    if (!body || n < 12) {
        return;
    }
    for (size_t i = 0; i + 12 < n; i++) {
        if (strncasecmp(body + i, "progress", 8) != 0) {
            continue;
        }
        const char* p = body + i + 8;
        const char* end = body + n;
        if (p < end && *p == ':') {
            ++p;
        }
        while (p < end && (*p == ' ' || *p == '\t' || *p == 0)) {
            ++p;
        }
        char tmp[72];
        size_t k = 0;
        while (p < end && k + 1 < sizeof(tmp) && *p != '\r' && *p != '\n' && *p != 0) {
            tmp[k++] = *p++;
        }
        tmp[k] = 0;
        handle_progress_body(tmp[0] ? tmp : body + i);
        return;
    }
}

static void handle_set_parameter(const char* body, size_t body_n, const char* req) {
    char ct[56] = "";
    hdr_copy_line(hdr_find(req, "Content-Type"), ct, sizeof(ct));
    if (strcasestr(ct, "image/")) {
        return;
    }
    if (body && body_n && !s_body_truncated) {
        scan_progress_bytes(body, body_n);
        if (strcasestr(ct, "dmap") || strcasestr(ct, "daap")) {
            handle_dmap(reinterpret_cast<const uint8_t*>(body), body_n);
            return;
        }
        if (strcasestr(body, "volume:") ||
            (body_n >= 8 && memcmp(body, "bplist00", 8) == 0)) {
            handle_volume(body, body_n, req);
        }
    }
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
        {
            const char* range = hdr_find(req, "Range");
            const char* npt = range ? strcasestr(range, "npt=") : nullptr;
            if (npt) {
                const float sec = strtof(npt + 4, nullptr);
                airplay_meta_set_npt_ms((uint32_t)(sec * 1000.f + 0.5f));
            }
            s_dacp_time_pend = true;
        }
        airplay_meta_on_play();
        reply(cseq, "Audio-Latency: 31680\r\n");
        Serial.println(F("[AirPlay] RECORD"));
    } else if (!strcmp(method, "FLUSH") || !strcmp(method, "PAUSE")) {
        airplay_rtp_flush();
        g_ap.last_rtp_ms = millis();
        wifi_touch_activity();
        reply(cseq, "");
    } else if (!strcmp(method, "TEARDOWN")) {
        reply(cseq, "");
        // Не вішати RTSP і не віддавати I2S: iPhone часто шле TEARDOWN на паузі/скіпі,
        // а наступний SETUP йде тим самим TCP. Інакше колонка «відвалюється».
        g_ap.playing = false;
        g_ap.session = false;
        airplay_rtp_stop();
        airplay_meta_on_flush();
        Serial.println(F("[AirPlay] TEARDOWN (keep RTSP)"));
    } else if (!strcmp(method, "SET_PARAMETER")) {
        const char* blank = strstr(req, "\r\n\r\n");
        const size_t body_n = blank ? (len > (size_t)(blank + 4 - req) ? len - (size_t)(blank + 4 - req) : 0) : 0;
        handle_set_parameter(body, body_n, req);
        reply(cseq, "");
    } else if (!strcmp(method, "GET_PARAMETER")) {
        char vol[40];
        snprintf(vol, sizeof(vol), "volume: %.6f\r\n", airplay_ui_to_db((int)radioState.vol));
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
