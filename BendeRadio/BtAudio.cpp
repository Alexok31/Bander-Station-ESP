#include "BtAudio.h"

#include "RadioConfig.h"
#include "pcm_analyzer.h"

#include <HardwareSerial.h>
#include <cstdio>
#include <cstring>

// BK8000L: AT+XX + CRLF @ 9600 8N1. Аудио — аналог на CD4052 X1/Y1 (см. AudioMux).

static HardwareSerial s_bt_uart(1);

static bool g_sink_running = false;
static bool g_link_connected = false;
static bool g_is_playing = false;
static bool g_uart_ready = false;
static int8_t s_vol_applied = -1;

static uint32_t s_track_duration_ms = 0;
static uint32_t s_anchor_pos_ms = 0;
static uint32_t s_anchor_wall_ms = 0;
static uint32_t s_last_reconnect_ms = 0;
static uint8_t s_cc_attempts = 0;

static char s_title[96];
static char s_artist[96];
static volatile uint32_t s_meta_serial = 0;
static volatile uint8_t s_remote_ui_req = 0;

static char s_rx_line[96];
static uint8_t s_rx_len = 0;

static void meta_bump() {
    s_meta_serial++;
}

static void meta_clear() {
    s_title[0] = '\0';
    s_artist[0] = '\0';
    s_track_duration_ms = 0;
    s_anchor_pos_ms = 0;
    s_anchor_wall_ms = millis();
    meta_bump();
}

static void copy_meta_field(char* dst, size_t cap, const char* src) {
    if (cap == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void bt_debug(const char* msg) {
    if (RadioConfig::debugBtUartSerial) {
        Serial.println(msg);
    }
}

static void bt_send_cmd(const char* cmd) {
    if (!g_uart_ready || cmd == nullptr) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "AT+%s\r\n", cmd);
    s_bt_uart.print(buf);
    if (RadioConfig::debugBtUartSerial) {
        Serial.print(F("[BT] >> "));
        Serial.print(buf);
    }
}

static void trim_rx_line(char* s) {
    if (s == nullptr) {
        return;
    }
    char* w = s;
    while (*w == ' ' || *w == '\t') {
        w++;
    }
    if (w != s) {
        memmove(s, w, strlen(w) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static bool line_is(const char* line, const char* token) {
    return line != nullptr && token != nullptr && strcmp(line, token) == 0;
}

static void bt_handle_event_line(char* line) {
    trim_rx_line(line);
    if (line[0] == '\0') {
        return;
    }
    if (RadioConfig::debugBtUartSerial) {
        Serial.print(F("[BT] << "));
        Serial.println(line);
    }

    if (line_is(line, "ON")) {
        bt_debug("module ON");
        return;
    }
    if (line_is(line, "II") || line_is(line, "M1")) {
        bt_audio_set_link_connected(true);
        return;
    }
    if (line_is(line, "IA") || line_is(line, "M0")) {
        bt_audio_set_link_connected(false);
        return;
    }
    if (line_is(line, "MB")) {
        if (!g_link_connected) {
            bt_audio_set_link_connected(true);
        }
        bt_audio_set_playing(true);
        return;
    }
    if (line_is(line, "MA")) {
        bt_audio_set_playing(false);
        return;
    }
}

static void bt_uart_push_char(char c) {
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        if (s_rx_len > 0) {
            s_rx_line[s_rx_len] = '\0';
            bt_handle_event_line(s_rx_line);
            s_rx_len = 0;
        }
        return;
    }
    if (s_rx_len + 1 >= sizeof(s_rx_line)) {
        s_rx_len = 0;
        return;
    }
    s_rx_line[s_rx_len++] = c;
}

static void bt_uart_poll() {
    while (g_uart_ready && s_bt_uart.available() > 0) {
        bt_uart_push_char((char)s_bt_uart.read());
    }
}

static void bt_uart_begin() {
    if (RadioConfig::bk8000UartRxPin == 255 || RadioConfig::bk8000UartTxPin == 255) {
        g_uart_ready = false;
        return;
    }
    s_bt_uart.end();
    s_bt_uart.begin(RadioConfig::bk8000UartBaud, SERIAL_8N1, RadioConfig::bk8000UartRxPin,
                    RadioConfig::bk8000UartTxPin);
    g_uart_ready = true;
    s_rx_len = 0;
    delay(RadioConfig::bk8000UartBootMs);
}

static void bt_uart_end() {
    if (g_uart_ready) {
        s_bt_uart.end();
    }
    g_uart_ready = false;
    s_rx_len = 0;
}

static void bt_module_power_set(bool on) {
    if (RadioConfig::btModulePowerEnablePin == 255) {
        return;
    }
    pinMode(RadioConfig::btModulePowerEnablePin, OUTPUT);
    const bool level = on ? RadioConfig::btModulePowerActiveHigh
                          : !RadioConfig::btModulePowerActiveHigh;
    digitalWrite(RadioConfig::btModulePowerEnablePin, level ? HIGH : LOW);
}

static void bt_audio_shutdown_impl(bool send_at_powerdown) {
    if (g_uart_ready && send_at_powerdown && RadioConfig::btModuleAtPowerDownOnWifi) {
        bt_send_cmd("CD");
        bt_send_cmd("CP");
        delay(80);
        while (s_bt_uart.available() > 0) {
            (void)s_bt_uart.read();
        }
    }
    bt_uart_end();
    bt_module_power_set(false);

    g_sink_running = false;
    g_link_connected = false;
    g_is_playing = false;
    s_vol_applied = -1;
    s_cc_attempts = 0;
    meta_clear();
    s_remote_ui_req = 0;
    pcm_analyzer_reset();
}

void bt_audio_set_link_connected(bool connected) {
    if (g_link_connected == connected) {
        return;
    }
    g_link_connected = connected;
    if (!connected) {
        g_is_playing = false;
        meta_clear();
        s_remote_ui_req = 0;
        s_vol_applied = -1;
        pcm_analyzer_reset();
    } else {
        s_cc_attempts = 0;
        s_last_reconnect_ms = millis();
    }
}

void bt_audio_set_playing(bool playing) {
    if (g_is_playing == playing) {
        return;
    }
    g_is_playing = playing;
    s_anchor_wall_ms = millis();
    if (playing) {
        s_remote_ui_req = 2;
    } else {
        s_remote_ui_req = 1;
    }
}

void bt_audio_set_track_meta(const char* title, const char* artist) {
    char t[sizeof(s_title)];
    char a[sizeof(s_artist)];
    copy_meta_field(t, sizeof(t), title);
    copy_meta_field(a, sizeof(a), artist);
    if (strcmp(t, s_title) != 0 || strcmp(a, s_artist) != 0) {
        strncpy(s_title, t, sizeof(s_title) - 1);
        strncpy(s_artist, a, sizeof(s_artist) - 1);
        meta_bump();
    }
}

void bt_audio_set_track_times_ms(uint32_t position_ms, uint32_t duration_ms) {
    s_track_duration_ms = duration_ms;
    s_anchor_pos_ms = position_ms;
    s_anchor_wall_ms = millis();
}

void bt_audio_queue_remote_ui(uint8_t req) {
    if (req <= 2u) {
        s_remote_ui_req = req;
    }
}

void bt_audio_shutdown_for_wifi_mode() {
    bt_audio_shutdown_impl(true);
}

void bt_audio_start_sink() {
    if (g_sink_running) {
        return;
    }
    bt_module_power_set(true);
    if (RadioConfig::btModulePowerEnablePin != 255) {
        delay(RadioConfig::btModulePowerOnDelayMs);
    }
    g_sink_running = true;
    g_link_connected = false;
    g_is_playing = false;
    s_vol_applied = -1;
    meta_clear();
    s_remote_ui_req = 0;
    s_last_reconnect_ms = millis();
    s_cc_attempts = 0;

    bt_uart_begin();
    bt_send_cmd("CC");
    s_cc_attempts = 1;
}

void bt_audio_stop_sink() {
    bt_audio_shutdown_impl(true);
}

void bt_audio_forget_paired_devices() {
    if (!g_sink_running) {
        bt_audio_start_sink();
    }
    bt_send_cmd("CD");
    bt_send_cmd("CZ");
    g_link_connected = false;
    g_is_playing = false;
    s_vol_applied = -1;
    meta_clear();
    s_remote_ui_req = 0;
    pcm_analyzer_reset();
    delay(120);
    bt_send_cmd("CA");
    s_last_reconnect_ms = millis();
}

bool bt_audio_is_sink_running() {
    return g_sink_running;
}

bool bt_audio_needs_pairing_ui() {
    if (!RadioConfig::btShowPairingSearchEyes) {
        return false;
    }
    return g_sink_running && !g_link_connected;
}

void bt_audio_tick() {
    if (!g_sink_running) {
        return;
    }
    bt_uart_poll();

    if (g_link_connected) {
        return;
    }

    uint32_t interval = RadioConfig::bk8000ReconnectIntervalMs;
    if (s_cc_attempts >= RadioConfig::bk8000ReconnectMaxAttempts) {
        interval = 60000;
    }
    const uint32_t now = millis();
    if ((uint32_t)(now - s_last_reconnect_ms) < interval) {
        return;
    }
    s_last_reconnect_ms = now;
    if (s_cc_attempts < 255) {
        s_cc_attempts++;
    }
    bt_send_cmd("CC");
}

void bt_audio_volume_apply(bool audio_on, int8_t vol_ui) {
    if (!g_sink_running || !g_uart_ready) {
        return;
    }
    if (!audio_on) {
        s_vol_applied = -1;
        return;
    }
    int target = constrain((int)vol_ui, 0, 21);
    if (s_vol_applied < 0) {
        s_vol_applied = target;
        return;
    }
    while (s_vol_applied < target) {
        bt_send_cmd("CK");
        s_vol_applied++;
    }
    while (s_vol_applied > target) {
        bt_send_cmd("CL");
        s_vol_applied--;
    }
}

void bt_audio_avrcp_pause() {
    if (!g_link_connected) {
        return;
    }
    bt_send_cmd("MA");
    g_is_playing = false;
    s_anchor_wall_ms = millis();
}

void bt_audio_avrcp_play() {
    if (!g_link_connected) {
        return;
    }
    bt_send_cmd("MA");
    g_is_playing = true;
    s_anchor_wall_ms = millis();
}

void bt_audio_avrcp_next() {
    bt_send_cmd("MF");
}

void bt_audio_avrcp_previous() {
    bt_send_cmd("MH");
}

uint32_t bt_audio_track_duration_ms() {
    return s_track_duration_ms;
}

void bt_audio_poll_track_position() {
    // MV по UART иногда даёт щелчок в аналоге — метаданные позже с BK8000L.
}

uint32_t bt_audio_track_meta_serial() {
    return s_meta_serial;
}

uint8_t bt_audio_take_remote_ui_request() {
    const uint8_t v = s_remote_ui_req;
    s_remote_ui_req = 0;
    return v;
}

const char* bt_audio_track_scroll_cstr() {
    if (s_title[0] == '\0' && s_artist[0] == '\0') {
        return g_link_connected ? "BT" : "BT pair";
    }
    if (s_artist[0] == '\0') {
        return s_title;
    }
    if (s_title[0] == '\0') {
        return s_artist;
    }
    static char line[sizeof(s_title) + sizeof(s_artist) + 8];
    snprintf(line, sizeof(line), "%s  |  %s", s_title, s_artist);
    line[sizeof(line) - 1] = '\0';
    return line;
}

uint32_t bt_audio_track_position_ms() {
    const uint32_t dur = s_track_duration_ms;
    if (dur <= 1u) {
        return 0;
    }
    if (!g_is_playing) {
        uint32_t p = s_anchor_pos_ms;
        return (p >= dur) ? (dur - 1u) : p;
    }
    const uint32_t now = millis();
    uint64_t pos = (uint64_t)s_anchor_pos_ms + (uint64_t)(now - s_anchor_wall_ms);
    if (pos >= dur) {
        return dur - 1u;
    }
    return (uint32_t)pos;
}
