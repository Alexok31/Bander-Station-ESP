#include "WebUi.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "NvsConfig.h"
#include "RadioConfig.h"
#include "core0.h"
#include "WebUiPage.h"
#include "WebUiSaveScope.h"
#include "BenderAi.h"

static WebServer server(80);
static DNSServer dnsServer;
static bool dnsRunning = false;
static bool handlersInstalled = false;
static volatile bool s_web_need_listen_reset = false;

static void web_send_close_connection() {
    server.sendHeader("Connection", "close");
}

static bool apModeActive() {
    const wifi_mode_t m = WiFi.getMode();
    return (m == WIFI_AP || m == WIFI_AP_STA);
}

static void sendRedirectRoot() {
    web_send_close_connection();
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
}

static void captiveProbeOk() {
    web_send_close_connection();
    server.send(200, "text/plain", "OK");
}

static void updateCaptivePortalState() {
    // Run captive DNS only while SoftAP is actually up.
    const bool apReady = apModeActive() && (WiFi.softAPIP() != IPAddress((uint32_t)0));
    if (apReady) {
        if (!dnsRunning) {
            dnsServer.start(53, "*", WiFi.softAPIP());
            dnsRunning = true;
        }
        return;
    }
    if (dnsRunning) {
        dnsServer.stop();
        dnsRunning = false;
    }
}

static void webUiOnWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)info;
    if (event != ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
        return;
    }
    if (!apModeActive()) {
        return;
    }
    s_web_need_listen_reset = true;
}

static void htmlAppendEscaped(String& html, const String& s) {
    for (unsigned i = 0; i < s.length(); i++) {
        const char c = s[i];
        if (c == '&') {
            html += F("&amp;");
        } else if (c == '"') {
            html += F("&quot;");
        } else if (c == '<') {
            html += F("&lt;");
        } else {
            html += c;
        }
    }
}

static String htmlEscaped(const String& value) {
    String out;
    out.reserve(value.length() + 16);
    htmlAppendEscaped(out, value);
    return out;
}

struct WebUiToken { const char* name; String value; };

static String matrixControls() {
    int8_t trim[RadioConfig::matrixModuleCount] = {};
    matrix_get_brightness_trim(trim, RadioConfig::matrixModuleCount);
    String html;
    html.reserve(3500);
    html += F("<div class=\"head-wrap\"><div class=\"head-row\">");
    const uint8_t order[] = {3, 4, 0, 1, 2};
    const char* names[] = {"Рот 1", "Рот 2", "Рот 3", "Левый глаз", "Правый глаз"};
    for (uint8_t n = 0; n < 5; ++n) {
        if (n == 2) html += F("</div><div class=\"head-row\">");
        const uint8_t i = order[n];
        html += F("<div class=\"mx\"><div class=\"mx-title\">");
        html += names[i];
        html += F("</div><input type=\"hidden\" name=\"mbr"); html += String(i);
        html += F("\" id=\"mbr"); html += String(i);
        html += F("\" value=\""); html += String(trim[i]);
        html += F("\"><div class=\"mx-value\" id=\"v"); html += String(i);
        html += F("\">"); html += String(trim[i]);
        html += F("</div><div class=\"mx-buttons\">");
        for (int delta : {-1, 1}) {
            html += F("<button type=\"button\" data-matrix=\""); html += String(i);
            html += F("\" data-delta=\""); html += String(delta);
            html += F("\" aria-label=\""); html += names[i];
            html += delta < 0 ? F(": темнее\">−</button>") : F(": ярче\">+</button>");
        }
        html += F("</div></div>");
    }
    html += F("</div></div>");
    return html;
}

static void sendPage() {
    wifi_touch_activity();
    WifiStored w;
    nvsLoadWifi(w);
    const bool online = WiFi.status() == WL_CONNECTED;
    String ssid = online ? WiFi.SSID() : (w.staSsid.length() ? w.staSsid : String(RadioConfig::wifiSsid));
    String aiUrl;
    nvsLoadAiWsUrl(aiUrl);
    String custom[RadioConfig::customStationMaxCount];
    uint8_t count = 0;
    nvsLoadCustomStations(custom, RadioConfig::customStationMaxCount, count);
    String stations;
    for (uint8_t i = 0; i < count; ++i) { stations += custom[i]; stations += '\n'; }
    String options;
    const uint8_t calm = nvsLoadCalmMinutes();
    for (auto minutes : BenderBehavior::calmOptions) {
        options += F("<option value=\""); options += String(minutes); options += '"';
        if (minutes == calm) options += F(" selected");
        options += '>';
        options += minutes ? String(minutes) + F(" мин") : String(F("Не засыпать"));
        options += F("</option>");
    }
    const auto character = nvsLoadCharacter();
    // Template values are escaped before insertion. Only our own controls contain markup.
    const WebUiToken tokens[] = {
        {"CONNECTION_CLASS", online ? "" : "offline"},
        {"CONNECTION_TEXT", online ? String(F("Wi-Fi подключён")) : String(F("Wi-Fi не подключён"))},
        {"IP", htmlEscaped(online ? WiFi.localIP().toString() : WiFi.softAPIP().toString())},
        {"CURRENT_STATION", htmlEscaped(radio_station_name((uint8_t)radioState.station))},
        {"RADIO_STATUS", play_mode_is_airplay() ? String(F("Сейчас выбран режим AirPlay")) :
            (radioState.state ? String(F("FM / Интернет-радио · включено")) : String(F("FM / Интернет-радио · на паузе")))},
        {"CUSTOM_COUNT", String(count)}, {"STATION_MAX", String(RadioConfig::customStationMaxCount)},
        {"STATIONS", htmlEscaped(stations)}, {"SSID", htmlEscaped(ssid)},
        {"AP_SSID", htmlEscaped(nvsEffectiveApSsid(w))}, {"AI_URL", htmlEscaped(aiUrl)},
        {"WAKE_CHECKED", nvsLoadWakeOnShake() ? "checked" : ""},
        {"SHAKE", String(nvsLoadShakeThreshold())}, {"SHAKE_MIN", String(BenderBehavior::shakeMin)},
        {"SHAKE_MAX", String(BenderBehavior::shakeMax)}, {"SHAKE_STEP", String(BenderBehavior::shakeStep)},
        {"SHAKE_DEFAULT", String(BenderBehavior::shakeDefault)},
        {"CALM_OPTIONS", options}, {"MATRIX_CONTROLS", matrixControls()},
        {"SARCASM", String(character.values[0])}, {"SOCIABILITY", String(character.values[1])},
        {"CURIOSITY", String(character.values[2])}, {"STUBBORNNESS", String(character.values[3])},
        {"WARMTH", String(character.values[4])}
    };
    web_send_close_connection();
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    // Stream from flash instead of allocating another full page on the ESP heap.
    // Scan the original template only: user text cannot introduce template tokens.
    const char* cursor = kWebUiPage;
    while (const char* start = strstr(cursor, "{{")) {
        if (start != cursor) server.sendContent_P(cursor, size_t(start - cursor));
        const char* end = strstr(start + 2, "}}");
        if (!end) break;
        for (const auto& token : tokens) {
            if (strlen(token.name) == size_t(end - start - 2) &&
                strncmp(start + 2, token.name, size_t(end - start - 2)) == 0) {
                // Empty content is the HTTP chunked terminator, not an empty field.
                if (token.value.length()) server.sendContent(token.value);
                break;
            }
        }
        cursor = end + 2;
    }
    if (*cursor) server.sendContent_P(cursor);
    server.sendContent("");
}

static void saveError(const char* message) {
    web_send_close_connection();
    server.send(400, "text/plain; charset=utf-8", message);
}

static void handleSave() {
    wifi_touch_activity();
    const String section = server.arg("section");
    const WebUiSaveScope scope(section.c_str());
    if (!scope.valid) { saveError("Неизвестный раздел настроек"); return; }

    // Validate every requested field before any NVS write. Other sections stay untouched.
    WifiStored current;
    String staSsid, staPass, apSsid, apPass, aiUrl;
    uint16_t shake = 0, calm = 0;
    int8_t trim[RadioConfig::matrixModuleCount] = {};
    String parsed[RadioConfig::customStationMaxCount];
    uint8_t count = 0;
    BenderCharacter::Settings character;
    if (scope.character) {
        for (uint8_t i = 0; i < BenderCharacter::count; ++i) {
            uint16_t value;
            const char* key = BenderCharacter::keys[i];
            if (!server.hasArg(key) || !BenderBehavior::parseUnsigned(server.arg(key).c_str(), value) || value > 100) {
                saveError("Каждая черта характера должна быть от 0 до 100"); return;
            }
            character.values[i] = uint8_t(value);
        }
    }
    if (scope.wifi) {
        nvsLoadWifi(current);
        staSsid = server.arg("sta_ssid"); staSsid.trim();
        if (!staSsid.length() || staSsid.length() > 32) { saveError("Укажи имя Wi-Fi сети (до 32 байт)"); return; }
        staPass = server.arg("sta_pass");
        if (!staPass.length()) staPass = current.staPass;
        apSsid = server.arg("ap_ssid"); apSsid.trim();
        if (!apSsid.length()) apSsid = nvsEffectiveApSsid(current);
        apPass = server.arg("ap_pass");
        if ((apPass.length() && apPass.length() < 8) || apPass.length() > 64 ||
            apSsid.length() > 32 || staPass.length() > 64) { saveError("Проверь длину имени сети и паролей. Пароль точки доступа: 8–64 символа."); return; }
        if (!apPass.length()) apPass = current.apPass;
    }
    if (scope.ai) {
        if (!server.hasArg("ai_ws")) { saveError("Не передан адрес AI сервера"); return; }
        aiUrl = server.arg("ai_ws");
        if (!nvsNormalizeAiWsUrl(aiUrl)) { saveError("Адрес AI сервера должен начинаться с ws:// или wss://"); return; }
    }
    if (scope.behavior) {
        if (!scope.legacy && (!server.hasArg("motion_settings") || !server.hasArg("shake_delta") || !server.hasArg("calm_minutes"))) {
            saveError("Не переданы настройки поведения"); return;
        }
        if ((server.hasArg("shake_delta") && (!BenderBehavior::parseUnsigned(server.arg("shake_delta").c_str(), shake) || !BenderBehavior::validShake(shake))) ||
            (server.hasArg("calm_minutes") && (!BenderBehavior::parseUnsigned(server.arg("calm_minutes").c_str(), calm) || !BenderBehavior::validCalm(calm)))) {
            saveError("Недопустимая чувствительность или время до покоя"); return;
        }
    }
    if (scope.display) {
        for (uint8_t i = 0; i < RadioConfig::matrixModuleCount; ++i) {
            const String name = String("mbr") + String(i);
            uint16_t value;
            if (!server.hasArg(name) || !BenderBehavior::parseUnsigned(server.arg(name).c_str(), value) || value > RadioConfig::matrixBrightnessTrimMax) {
                saveError("Яркость каждого модуля должна быть от 0 до 15"); return;
            }
            trim[i] = int8_t(value);
        }
    }
    if (scope.radio) {
        if (!server.hasArg("stations")) { saveError("Не передан список станций"); return; }
        const String list = server.arg("stations");
        int from = 0;
        while (from < list.length()) {
            const int nl = list.indexOf('\n', from);
            String line = nl < 0 ? list.substring(from) : list.substring(from, nl);
            line.trim();
            if (line.length()) {
                if (count >= RadioConfig::customStationMaxCount) { saveError("Слишком много станций"); return; }
                if (!line.startsWith("http://") && !line.startsWith("https://")) { saveError("Каждая станция должна быть ссылкой http:// или https://"); return; }
                parsed[count++] = line;
            }
            if (nl < 0) break;
            from = nl + 1;
        }
    }
    if (scope.character && !nvsSaveCharacter(character)) {
        server.send(500, "text/plain; charset=utf-8", "Не удалось сохранить характер. Попробуй ещё раз."); return;
    }
    if (scope.wifi) nvsSaveWifi(staSsid, staPass, apSsid, apPass);
    if (scope.radio) nvsSaveCustomStations(parsed, count);
    if (scope.ai) nvsSaveAiWsUrl(aiUrl);
    if (scope.behavior) {
        if (server.hasArg("motion_settings")) nvsSaveWakeOnShake(server.arg("wake_on_shake") == "1");
        if (server.hasArg("shake_delta")) nvsSaveShakeThreshold(shake);
        if (server.hasArg("calm_minutes")) nvsSaveCalmMinutes(uint8_t(calm));
    }
    if (scope.display) matrix_set_brightness_trim(trim, RadioConfig::matrixModuleCount, true);
    web_send_close_connection();
    String response = F("<!doctype html><html lang=ru><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Сохранено — Bender</title><body style='margin:0;background:#111a1b;color:#f1f4ed;font:16px system-ui;padding:10vh 24px'><main style='max-width:440px;margin:auto'><p style='color:#b6efd5;letter-spacing:2px'>BENDER STATION</p><h1>Запомнил.</h1><p>Настройки сохранены. Бендер перезапускается.</p><p>Если менял Wi-Fi, подключись к новой сети.</p><a style='color:#b6efd5' href='/#");
    response += scope.radio ? "radio" : (scope.character ? "character" : "settings");
    response += F("'>Вернуться к панели →</a></main><script>setTimeout(()=>location.href='/#");
    response += scope.radio ? "radio" : (scope.character ? "character" : "settings");
    response += F("',8000)</script></body></html>");
    server.send(200, "text/html; charset=utf-8", response);
    delay(300);
    ESP.restart();
}

static void handleCharacterPreview() {
    wifi_touch_activity();
    web_send_close_connection();
    server.sendHeader("Cache-Control", "no-store");
    if (server.method() == HTTP_GET) {
        server.send(200, "application/json", String("{\"state\":") + String(bender_ai_preview_status()) + "}");
        return;
    }
    BenderCharacter::Settings traits;
    for (uint8_t i = 0; i < BenderCharacter::count; ++i) {
        uint16_t value;
        const char* key = BenderCharacter::keys[i];
        if (!server.hasArg(key) || !BenderBehavior::parseUnsigned(server.arg(key).c_str(), value) || value > 100) {
            server.send(400, "text/plain; charset=utf-8", "Каждая черта должна быть от 0 до 100."); return;
        }
        traits.values[i] = uint8_t(value);
    }
    String question = server.hasArg("question") ? server.arg("question") : String(BenderCharacter::defaultQuestion);
    question.trim();
    if (question.length() != strlen(question.c_str()) || !BenderCharacter::validQuestion(question.c_str())) {
        server.send(400, "text/plain; charset=utf-8", "Напиши тестовый вопрос: от 1 до 200 символов."); return;
    }
    if (const char* error = bender_ai_preview_character(traits, question.c_str())) {
        server.send(409, "text/plain; charset=utf-8", error); return;
    }
    server.send(202, "application/json", "{\"state\":1}");
}

static void handleCalib() {
    wifi_touch_activity();
    if (server.method() != HTTP_POST) {
        web_send_close_connection();
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    int8_t trim[RadioConfig::matrixModuleCount] = {0, 0, 0, 0, 0};
    matrix_get_brightness_trim(trim, RadioConfig::matrixModuleCount);
    for (uint8_t i = 0; i < RadioConfig::matrixModuleCount; i++) {
        const String key = String("mbr") + String((int)i);
        if (!server.hasArg(key)) {
            continue;
        }
        String v = server.arg(key);
        v.trim();
        trim[i] = (int8_t)constrain(v.toInt(), (int)RadioConfig::matrixBrightnessTrimMin,
                                     (int)RadioConfig::matrixBrightnessTrimMax);
    }
    matrix_set_brightness_trim(trim, RadioConfig::matrixModuleCount, false);
    web_send_close_connection();
    server.send(200, "application/json", "{\"ok\":true}");
}

void webUiBegin() {
    if (!handlersInstalled) {
        server.on("/generate_204", HTTP_ANY, sendRedirectRoot);        // Android
        server.on("/gen_204", HTTP_ANY, sendRedirectRoot);             // Android
        server.on("/hotspot-detect.html", HTTP_ANY, sendRedirectRoot); // Apple
        server.on("/success.txt", HTTP_ANY, sendRedirectRoot);         // Apple
        server.on("/connecttest.txt", HTTP_ANY, sendRedirectRoot);     // Windows
        server.on("/redirect", HTTP_ANY, sendRedirectRoot);            // Windows
        server.on("/fwlink", HTTP_ANY, sendRedirectRoot);              // Windows
        server.on("/ncsi.txt", HTTP_ANY, captiveProbeOk);              // Windows fallback
        server.on("/", HTTP_GET, sendPage);
        server.on("/calib", HTTP_POST, handleCalib);
        server.on("/character/preview", HTTP_POST, handleCharacterPreview);
        server.on("/character/preview", HTTP_GET, handleCharacterPreview);
        server.on("/save", HTTP_POST, handleSave);
        server.onNotFound([]() {
            web_send_close_connection();
            server.send(404, "text/plain", "Not Found");
        });
        WiFi.onEvent(webUiOnWifiEvent);
        handlersInstalled = true;
    }
    server.begin();
    updateCaptivePortalState();
}

void webUiLoop() {
    if (s_web_need_listen_reset) {
        s_web_need_listen_reset = false;
        if (dnsRunning) {
            dnsServer.stop();
            dnsRunning = false;
        }
        server.close();  // esp32 2.x WebServer: no end(); close()/stop() drop listener
        delay(30);
        server.begin();
    }
    updateCaptivePortalState();
    if (dnsRunning) {
        dnsServer.processNextRequest();
    }
    server.handleClient();
}
