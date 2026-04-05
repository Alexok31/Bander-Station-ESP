#include "WebUi.h"

#include <WebServer.h>
#include <WiFi.h>

#include "NvsConfig.h"
#include "RadioConfig.h"

static WebServer server(80);

static void sendPage() {
    const bool staOk = (WiFi.status() == WL_CONNECTED);
    String ipSta = staOk ? WiFi.localIP().toString() : String("—");
    String rssi = staOk ? String(WiFi.RSSI()) : String("—");

    WifiStored w;
    nvsLoadWifi(w);
    String staSsidShow;
    if (staOk) {
        staSsidShow = WiFi.SSID();
    } else if (w.staSsid.length()) {
        staSsidShow = w.staSsid;
    } else {
        staSsidShow = RadioConfig::wifiSsid;
    }

    String html;
    html.reserve(2048);
    html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>BendeRadio</title>"
              "<style>"
              "body{font-family:system-ui,sans-serif;max-width:520px;margin:16px auto;padding:0 12px;}"
              "label{display:block;margin:12px 0 4px;color:#333;}"
              "input{width:100%;box-sizing:border-box;padding:8px;font-size:16px;}"
              ".btn{margin-top:16px;padding:10px 16px;font-size:16px;cursor:pointer;}"
              "code{background:#eee;padding:2px 6px;border-radius:4px;}"
              ".box{background:#f6f6f6;padding:12px;border-radius:8px;margin:12px 0;}"
              "</style></head><body>");
    String apSsidShow = nvsEffectiveApSsid(w);

    html += F("<h1>BendeRadio</h1>");
    html += F("<p>Веб по Wi‑Fi. Точка доступа: <code>");
    html += apSsidShow;
    html += F("</code> — откройте <code>http://192.168.4.1</code>. "
              "Если ESP в домашней сети — эта же страница по IP STA (см. ниже). "
              "Имя и пароль AP сохраняются во flash (NVS) и не сбрасываются при перезагрузке.</p>");

    html += F("<div class=\"box\"><b>Статус</b><br>");
    html += F("Точка доступа ESP: <code>http://");
    html += WiFi.softAPIP().toString();
    html += F("</code><br>");
    html += F("Домашняя сеть (STA): ");
    html += ipSta;
    if (staOk) {
        html += F(" (RSSI ");
        html += rssi;
        html += F(" dBm)");
    }
    html += F("</div>");

    html += F("<form method=\"POST\" action=\"/save\">");
    html += F("<h2>Wi‑Fi (домашняя сеть)</h2>");
    html += F("<label>Имя сети (SSID)</label>");
    html += F("<input name=\"sta_ssid\" maxlength=\"32\" value=\"");
    html += staSsidShow;
    html += F("\" autocomplete=\"off\">");
    html += F("<label>Пароль</label>");
    html += F("<input name=\"sta_pass\" type=\"password\" maxlength=\"64\" placeholder=\"пароль STA\" autocomplete=\"new-password\">");

    html += F("<h2>Точка доступа для настройки (сохраняется в NVS)</h2>");
    html += F("<label>Имя сети AP (SSID)</label>");
    html += F("<input name=\"ap_ssid\" maxlength=\"32\" value=\"");
    html += w.apSsid.length() ? w.apSsid : apSsidShow;
    html += F("\" autocomplete=\"off\">");
    html += F("<label>Пароль AP (мин. 8 символов WPA2)</label>");
    html += F("<input name=\"ap_pass\" type=\"password\" maxlength=\"64\" placeholder=\"пусто = не менять\" autocomplete=\"new-password\">");
    html += F("<p style=\"font-size:14px;color:#555;\">Пустые поля паролей не затирают уже сохранённые пароли STA/AP. "
              "Новый пароль AP — только если ввести 8+ символов.</p>");

    html += F("<button class=\"btn\" type=\"submit\">Сохранить и перезагрузить</button>");
    html += F("</form></body></html>");

    server.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    String staSsid = server.arg("sta_ssid");
    staSsid.trim();
    if (staSsid.length() == 0) {
        server.send(400, "text/plain", "SSID required");
        return;
    }
    String staPassIn = server.arg("sta_pass");
    String apSsidIn = server.arg("ap_ssid");
    apSsidIn.trim();
    String apPassIn = server.arg("ap_pass");

    WifiStored cur;
    nvsLoadWifi(cur);

    String staPassToStore = staPassIn.length() ? staPassIn : cur.staPass;

    String apSsidToStore = apSsidIn.length() ? apSsidIn : nvsEffectiveApSsid(cur);
    if (apSsidToStore.length() == 0) {
        apSsidToStore = RadioConfig::apSsid;
    }

    String apPassToStore = cur.apPass;
    if (apPassIn.length() >= 8) {
        apPassToStore = apPassIn;
    }

    nvsSaveWifi(staSsid, staPassToStore, apSsidToStore, apPassToStore);

    server.send(200, "text/html; charset=utf-8",
                 "<!DOCTYPE html><html><head><meta charset=utf-8></head><body>"
                 "<p>Сохранено. Перезагрузка…</p>"
                 "<script>setTimeout(function(){location.href='/';},3000);</script>"
                 "</body></html>");
    delay(300);
    ESP.restart();
}

void webUiBegin() {
    server.on("/", HTTP_GET, sendPage);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
}

void webUiLoop() {
    server.handleClient();
}
