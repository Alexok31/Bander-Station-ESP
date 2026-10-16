#include "WebUi.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <cerrno>

#include "NvsConfig.h"
#include "RadioConfig.h"
#include "core0.h"
#include "WebUiPage.h"
#include "WebUiSaveScope.h"
#include "BenderAi.h"
#include "WakeRuntime.h"
#include "WakeDecision.h"
#include "WakeVoiceSettings.h"
#include "Reminders.h"
#include "AirPlay.h"
#include "battery.h"

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
        {"IP", htmlEscaped(server.client().localIP().toString())},
        {"CURRENT_STATION", htmlEscaped(radio_station_name((uint8_t)radioState.station))},
        {"RADIO_STATUS", play_mode_is_airplay() ? String(F("Сейчас выбран режим AirPlay")) :
            (radioState.state ? String(F("FM / Интернет-радио · включено")) : String(F("FM / Интернет-радио · на паузе")))},
        {"CUSTOM_COUNT", String(count)}, {"STATION_MAX", String(RadioConfig::customStationMaxCount)},
        {"STATIONS", htmlEscaped(stations)}, {"SSID", htmlEscaped(ssid)},
        {"AP_SSID", htmlEscaped(nvsEffectiveApSsid(w))}, {"AI_URL", htmlEscaped(aiUrl)},
        {"WAKE_CHECKED", nvsLoadWakeOnShake() ? "checked" : ""},
        {"TILT_EYES_CHECKED", nvsLoadTiltEyes() ? "checked" : ""},
        {"SHAKE", String(nvsLoadShakeThreshold())}, {"SHAKE_MIN", String(BenderBehavior::shakeMin)},
        {"SHAKE_MAX", String(BenderBehavior::shakeMax)}, {"SHAKE_STEP", String(BenderBehavior::shakeStep)},
        {"SHAKE_DEFAULT", String(BenderBehavior::shakeDefault)},
        {"CALM_OPTIONS", options}, {"MATRIX_CONTROLS", matrixControls()},
        {"SARCASM", String(character.values[0])}, {"SOCIABILITY", String(character.values[1])},
        {"CURIOSITY", String(character.values[2])}, {"STUBBORNNESS", String(character.values[3])},
        {"WARMTH", String(character.values[4])}, {"ROUGHNESS", String(character.values[5])},
        {"PROFANITY", String(character.values[6])},
        {"LLM_MODEL", BenderLlmModel::id(nvsLoadLlmModel())}
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
    if (scope.display) {
        if (server.hasArg("eye_motion_settings") && !nvsSaveTiltEyes(server.arg("tilt_eyes") == "1")) {
            server.send(500, "text/plain; charset=utf-8", "Не удалось сохранить реакцию глаз. Попробуй ещё раз.");
            return;
        }
        matrix_set_brightness_trim(trim, RadioConfig::matrixModuleCount, true);
    }
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

static void handleDebugModel() {
    wifi_touch_activity();
    web_send_close_connection();
    server.sendHeader("Cache-Control", "no-store");
    if (server.method() == HTTP_POST) {
        uint8_t model = BenderLlmModel::Auto;
        if (!server.hasArg("model") || !BenderLlmModel::parse(server.arg("model").c_str(), model)) {
            server.send(400, "text/plain; charset=utf-8", "Неизвестная модель.");
            return;
        }
        if (!nvsSaveLlmModel(model)) {
            server.send(500, "text/plain; charset=utf-8", "Не удалось сохранить модель.");
            return;
        }
    } else if (server.method() != HTTP_GET) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    server.send(200, "application/json", String("{\"model\":\"") +
                BenderLlmModel::id(nvsLoadLlmModel()) + "\"}");
}

static void sendJson(const JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    web_send_close_connection();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", body);
}

static void handleLiveStatus() {
    wifi_touch_activity();
    BenderAiDiagnostic ai{};
    bender_ai_diagnostic(ai);
    const auto wake = WakeRuntime::status();
    const bool wifi = WiFi.status() == WL_CONNECTED;
    const bool airplay = play_mode_is_airplay();
    JsonDocument doc;
    doc["uptime_seconds"] = millis() / 1000u;
    doc["heap"] = ESP.getFreeHeap();
    doc["wifi"] = wifi;
    doc["wifi_searching"] = wifiConnecting;
    doc["ap"] = apModeActive();
    doc["rssi"] = wifi ? WiFi.RSSI() : 0;
    doc["ip"] = wifi ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["mode"] = airplay ? "AirPlay" : strcmp(g_audio_source, "bt") == 0 ? "Bluetooth" : "FM";
    doc["music"] = airplay ? airplay_playing() : radioState.state;
    doc["volume"] = radioState.vol;
    doc["battery_ready"] = battery_gauge_ready();
    doc["battery_percent"] = battery_percent();
    doc["battery_mv"] = battery_millivolts();
    doc["charging"] = battery_is_charging();
    doc["eyes_awake"] = matrix_face_is_awake();
    const auto face = bender_ai_face_state();
    doc["face"] = face == BenderFaceState::Listening ? "listening" :
                  face == BenderFaceState::Thinking ? "thinking" :
                  face == BenderFaceState::Speaking ? "speaking" :
                  face == BenderFaceState::Error ? "error" :
                  face == BenderFaceState::Pleased ? "pleased" :
                  face == BenderFaceState::Curious ? "curious" :
                  face == BenderFaceState::Tired ? "tired" :
                  face == BenderFaceState::Annoyed ? "annoyed" : "idle";
    doc["ai_busy"] = bender_ai_busy();
    doc["ai_socket"] = ai.socketConnected;
    doc["ai_session"] = ai.sessionReady;
    doc["ai_connecting"] = ai.connecting;
    doc["ai_wait_seconds"] = ai.waitSeconds;
    doc["mic"] = ai.microphoneOn;
    doc["wake_enabled"] = wake.enabled && WakeRuntime::voiceEnabled();
    doc["wake_ready"] = wake.ready;
    doc["wake_listening"] = wake.listening;
    doc["wake_error"] = wake.error;
    doc["last_error"] = ai.lastError;
    doc["last_error_ago_seconds"] = ai.lastErrorAgoSeconds;
    sendJson(doc);
}

static void handleReminders() {
    wifi_touch_activity();
    if (server.method() == HTTP_POST) {
        const String action = server.arg("action");
        if (action == "add" || action == "add_daily") {
            uint16_t minutes = 0;
            String task = server.arg("text");
            task.trim();
            if (!task.length() ||
                task.length() >= Reminders::textBytes || task.indexOf('\n') >= 0 || task.indexOf('\r') >= 0) {
                saveError("Укажи текст до 160 байт."); return;
            }
            bool added = false;
            if (action == "add") {
                if (!BenderBehavior::parseUnsigned(server.arg("minutes").c_str(), minutes) ||
                    !minutes || minutes > Reminders::maxMinutes) {
                    saveError("Укажи время от 1 минуты до 24 часов."); return;
                }
                added = Reminders::add(minutes, task.c_str());
            } else {
                const String at = server.arg("time");
                if (at.length() != 5 || at[2] != ':' ||
                    !isDigit(at[0]) || !isDigit(at[1]) || !isDigit(at[3]) || !isDigit(at[4])) {
                    saveError("Укажи время в формате ЧЧ:ММ."); return;
                }
                const int hour = (at[0] - '0') * 10 + at[1] - '0';
                const int minute = (at[3] - '0') * 10 + at[4] - '0';
                if (hour > 23 || minute > 59) {
                    saveError("Некорректное время."); return;
                }
                added = Reminders::addDaily(uint8_t(hour), uint8_t(minute), task.c_str());
            }
            if (!added) {
                web_send_close_connection();
                server.send(409, "text/plain; charset=utf-8", "Не удалось сохранить. Проверь свободные места и память колонки."); return;
            }
        } else if (action == "cancel") {
            uint32_t id = 0;
            const String arg = server.arg("id");
            char* end = nullptr;
            errno = 0;
            const unsigned long parsed = strtoul(arg.c_str(), &end, 10);
            if (!arg.length() || arg.length() > 10 || errno || !end || *end || !parsed ||
                !Reminders::cancel(uint32_t(parsed))) {
                saveError("Напоминание не найдено."); return;
            }
        } else {
            saveError("Неизвестное действие."); return;
        }
    }
    Reminders::Item items[Reminders::capacity];
    const size_t count = Reminders::list(items, Reminders::capacity);
    JsonDocument doc;
    doc["capacity"] = Reminders::capacity;
    doc["clock_ready"] = Reminders::clockReady();
    JsonArray list = doc["items"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
        JsonObject row = list.add<JsonObject>();
        row["id"] = items[i].id;
        row["text"] = items[i].text;
        row["remaining_seconds"] = items[i].remainingSeconds;
        row["due"] = items[i].due;
        row["daily"] = items[i].daily;
        row["today"] = items[i].today;
        row["minute_of_day"] = items[i].minuteOfDay;
    }
    sendJson(doc);
}

static void handleLiquidCalibration() {
    wifi_touch_activity();
    web_send_close_connection();
    const String action = server.arg("action");
    if (action != "tare" && action != "reset") {
        server.send(400, "text/plain; charset=utf-8", "Обнови страницу настроек"); return;
    }
    if (const char* error = liquid_calibrate(action == "reset")) {
        server.send(409, "text/plain; charset=utf-8", error); return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
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

static void handleWakeDiagnostic() {
    wifi_touch_activity(); web_send_close_connection();
    server.sendHeader("Cache-Control", "no-store");
    if (server.method() == HTTP_POST) {
        if (server.header("X-Bender-Record") != "1") {
            server.send(403, "text/plain; charset=utf-8", "Открой настройки колонки заново."); return;
        }
        if (server.arg("action") == "settings") {
            uint16_t seconds=0;
            if(!BenderBehavior::parseUnsigned(server.arg("follow_up_seconds").c_str(),seconds) ||
               !WakeVoiceSettings::validFollowup(seconds)) {
                server.send(400,"text/plain; charset=utf-8","Время ожидания: от 0 до 30 секунд.");return;
            }
            if(!nvsSaveWakeFollowupSeconds(uint8_t(seconds))) {
                server.send(409,"text/plain; charset=utf-8","Не удалось сохранить время ожидания.");return;
            }
            WakeRuntime::setFollowupSeconds(uint8_t(seconds));
        } else {
        const String enabled = server.arg("enabled");
        if (enabled != "0" && enabled != "1") { server.send(400,"text/plain","Invalid enabled"); return; }
        const String mode = server.hasArg("mode") ? server.arg("mode") : "diagnostic";
        if (mode != "voice" && mode != "diagnostic") { server.send(400,"text/plain","Invalid mode"); return; }
        const bool previousEnabled = WakeRuntime::enabled();
        const bool previousVoice = WakeRuntime::voiceEnabled();
        if (const char* error = WakeRuntime::enable(enabled == "1", mode == "voice")) {
            server.send(409,"text/plain; charset=utf-8",error); return;
        }
        // Persist the user's voice choice; diagnostic-only mode stays temporary.
        if (!nvsSaveWakeVoiceEnabled(enabled == "1" && mode == "voice")) {
            WakeRuntime::enable(previousEnabled, previousVoice);
            server.send(409,"text/plain; charset=utf-8","Не удалось сохранить голосовой вызов."); return;
        }
        }
    }
    const auto s = WakeRuntime::status();
    JsonDocument doc;
    doc["enabled"]=s.enabled; doc["listening"]=s.listening; doc["ready"]=s.ready;
    doc["diagnostic"]=!WakeRuntime::voiceEnabled(); doc["model"]="v7"; doc["threshold"]=0.25;
    doc["voice_state"]=bender_ai_wake_voice_state();
    doc["follow_up"]=WakeRuntime::followupEnabled();
    doc["follow_up_seconds"]=WakeRuntime::followupSeconds();
    doc["decisions"]=s.decisions; doc["detections"]=s.detections;
    doc["dropped"]=s.dropped; doc["resets"]=s.resets; doc["last_hit_ms"]=s.lastHitMs;
    doc["score"]=s.score; doc["processing_us"]=s.processingUs;
    doc["peak_processing_us"]=s.peakProcessingUs; doc["arena_bytes"]=s.arenaBytes;
    doc["runtime"]=9; doc["decision_ms"]=WakeDecision::intervalMs;
    doc["frontend_us"]=s.frontendUs; doc["inference_us"]=s.inferenceUs;
    doc["overruns"]=s.overruns; doc["queue_high_water"]=s.queueHighWater;
    doc["stack_free_bytes"]=s.stackFreeBytes; doc["error"]=s.error;
    doc["heap"]=ESP.getFreeHeap(); doc["psram"]=ESP.getFreePsram();
    String json; serializeJson(doc,json); server.send(200,"application/json",json);
}

static void sendWakeSampleStatus(int code = 200) {
    const auto s = bender_ai_sample_status();
    String json = String("{\"state\":") + unsigned(s.state) + ",\"id\":" + s.id +
        ",\"ms\":" + s.elapsedMs + ",\"label\":\"" + WakeSample::labels[s.label] +
        "\",\"split\":\"" + WakeSample::splits[s.split] + "\"}";
    server.send(code, "application/json", json);
}

static void handleWakeSampleList() {
    wifi_touch_activity(); web_send_close_connection();
    server.sendHeader("Cache-Control", "no-store");
    WakeSample::Status items[WakeSample::poolCapacity];
    const size_t count = bender_ai_sample_list(items, WakeSample::poolCapacity);
    const bool active = WakeSample::active(bender_ai_sample_status().state);
    const size_t free = ESP.getFreePsram();
    size_t available = free > WakeSample::memoryReserve ?
        (free - WakeSample::memoryReserve) / (WakeSample::headerBytes + WakeSample::pcmBytes) : 0;
    const size_t slots = count + size_t(active) < WakeSample::poolCapacity ?
        WakeSample::poolCapacity - count - size_t(active) : 0;
    if (available > slots) available = slots;
    String json = String("{\"capacity\":") + unsigned(WakeSample::poolCapacity) +
        ",\"available\":" + unsigned(available) + ",\"volatile\":true,\"items\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i) json += ',';
        json += String("{\"id\":") + items[i].id + ",\"state\":3,\"ms\":4000,\"label\":\"" +
            WakeSample::labels[items[i].label] + "\",\"split\":\"" + WakeSample::splits[items[i].split] + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

static void handleWakeSample() {
    wifi_touch_activity();
    web_send_close_connection();
    server.sendHeader("Cache-Control", "no-store");
    if (server.method() == HTTP_GET) { sendWakeSampleStatus(); return; }
    // Browser requests from another origin cannot set this header without a
    // CORS preflight; no cross-origin microphone controls are exposed.
    if (server.header("X-Bender-Record") != "1") {
        server.send(403, "text/plain; charset=utf-8", "Открой настройки колонки заново."); return;
    }
    if (server.arg("action") == "start") {
        if (const char* error = bender_ai_sample_start(server.arg("label").c_str(), server.arg("split").c_str())) {
            server.send(409, "text/plain; charset=utf-8", error); return;
        }
        sendWakeSampleStatus(202);
    } else if (server.arg("action") == "cancel") {
        uint32_t id;
        if (!WakeSample::parseId(server.arg("id").c_str(), id) || !bender_ai_sample_cancel(id)) {
            server.send(409, "text/plain; charset=utf-8", "Эта запись уже недоступна. Обнови статус."); return;
        }
        sendWakeSampleStatus();
    } else if (server.arg("action") == "clear") {
        if (!bender_ai_sample_clear()) {
            server.send(409, "text/plain; charset=utf-8", "Дождись окончания записи или скачивания."); return;
        }
        sendWakeSampleStatus();
    } else server.send(400, "text/plain; charset=utf-8", "Неизвестное действие.");
}

static void handleWakeSampleAudio() {
    wifi_touch_activity();
    web_send_close_connection();
    server.sendHeader("Cache-Control", "no-store");
    uint32_t id;
    if (!WakeSample::parseId(server.arg("id").c_str(), id)) {
        server.send(400, "text/plain", "Invalid sample ID"); return;
    }
    size_t bytes = 0;
    const uint8_t* wav = bender_ai_sample_audio(id, bytes);
    if (!wav) { server.send(409, "text/plain; charset=utf-8", "Запись ещё не готова, удалена или сейчас скачивается."); return; }
    // Lease is held until the synchronous transfer ends. No large String/copy
    // in internal RAM and no audio lock across slow network writes.
    server.setContentLength(bytes);
    server.send(200, "audio/wav", "");
    auto client = server.client();
    const int socket = client.fd();
    const uint32_t started = millis();
    size_t sent = 0;
    while (sent < bytes && client.connected() && uint32_t(millis() - started) < 8000u) {
        const size_t remaining = bytes - sent;
        const int n = ::send(socket, wav + sent, remaining > 1460 ? 1460 : remaining, MSG_DONTWAIT);
        if (n > 0) sent += size_t(n);
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
        delay(1);
    }
    bender_ai_sample_audio_release();
    if (sent != bytes) client.stop();
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
        server.on("/liquid/calibration", HTTP_POST, handleLiquidCalibration);
        server.on("/character/preview", HTTP_POST, handleCharacterPreview);
        server.on("/character/preview", HTTP_GET, handleCharacterPreview);
        server.on("/debug/model", HTTP_GET, handleDebugModel);
        server.on("/debug/model", HTTP_POST, handleDebugModel);
        server.on("/live/status", HTTP_GET, handleLiveStatus);
        server.on("/reminders", HTTP_GET, handleReminders);
        server.on("/reminders", HTTP_POST, handleReminders);
        const char* recordHeaders[] = {"X-Bender-Record"};
        server.collectHeaders(recordHeaders, 1);
        server.on("/wakeword/sample", HTTP_GET, handleWakeSample);
        server.on("/wakeword/sample", HTTP_POST, handleWakeSample);
        server.on("/wakeword/audio.wav", HTTP_GET, handleWakeSampleAudio);
        server.on("/wakeword/samples", HTTP_GET, handleWakeSampleList);
        server.on("/wakeword/diagnostic", HTTP_GET, handleWakeDiagnostic);
        server.on("/wakeword/diagnostic", HTTP_POST, handleWakeDiagnostic);
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
