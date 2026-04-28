#include "WebUi.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "NvsConfig.h"
#include "RadioConfig.h"
#include "core0.h"

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

static void sendPage() {
    wifi_touch_activity();
    const bool staOk = (WiFi.status() == WL_CONNECTED);
    String ipSta = staOk ? WiFi.localIP().toString() : String("offline");

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
    html.reserve(3200);
    html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>Bender Station</title>"
              "<style>"
              ":root{--bg:#008EA0;--panel:#1A5354;--text:#ffffff;--accent:#FF6F00;--field:#d9e2ea;--fieldText:#12202a;}"
              "body{font-family:system-ui,sans-serif;max-width:560px;margin:0 auto;padding:20px 14px;background:linear-gradient(180deg,#42A9B6 0%,var(--bg) 56%,#1A5354 100%);color:var(--text);}"
              ".hero{display:flex;align-items:center;gap:10px;margin-bottom:10px;}"
              ".icon{font-size:28px;line-height:1;filter:drop-shadow(0 1px 0 #22313b);}"
              "h1,h2{margin:0 0 10px 0;text-transform:uppercase;color:#111111;letter-spacing:0.8px;}"
              "h2{margin-top:18px;font-size:18px;}"
              "label{display:block;margin:10px 0 4px;color:var(--text);font-size:14px;}"
              "input,textarea{width:100%;box-sizing:border-box;padding:10px;font-size:15px;border-radius:8px;border:1px solid #4c6170;background:var(--field);color:var(--fieldText);}"
              ".btn{margin-top:16px;padding:11px 16px;font-size:16px;font-weight:600;cursor:pointer;border:none;border-radius:10px;background:var(--accent);color:#1e1e1e;}"
              ".panel{background:rgba(24,38,49,0.28);padding:12px;border-radius:10px;border:1px solid rgba(255,255,255,0.15);}"
              ".muted{margin-top:8px;font-size:13px;opacity:.92;}"
              ".headWrap{display:flex;flex-direction:column;gap:8px;margin-top:6px;}"
              ".headRow{display:flex;justify-content:center;gap:8px;}"
              ".mx{width:92px;min-height:82px;background:rgba(8,20,28,.34);border:1px solid rgba(255,255,255,.22);border-radius:10px;padding:6px;display:flex;flex-direction:column;align-items:center;justify-content:space-between;cursor:pointer;}"
              ".mxTitle{font-size:11px;text-align:center;line-height:1.15;color:#111111;}"
              ".mxVal{font-weight:700;color:var(--accent);}"
              ".mxBtns{display:flex;gap:8px;}"
              ".mxBtn{border:none;border-radius:8px;min-width:46px;min-height:40px;padding:6px 12px;background:#d8e4eb;color:#13232b;font-weight:700;font-size:22px;line-height:1;touch-action:manipulation;}"
              "</style></head><body>");
    String apSsidShow = nvsEffectiveApSsid(w);

    html += F("<div class=\"hero\"><div class=\"icon\">🤖</div><h1>Bender Station</h1></div>");
    html += F("<div class=\"panel\">");
    html += F("СТАТУС WIFI: ");
    html += ipSta;
    if (staOk) {
        html += F(" / ");
        html += WiFi.SSID();
    }
    html += F("</div>");

    html += F("<form method=\"POST\" action=\"/save\">");
    html += F("<h2>Wi-Fi Дом</h2>");
    html += F("<label>Имя сети</label>");
    html += F("<input name=\"sta_ssid\" maxlength=\"32\" value=\"");
    html += staSsidShow;
    html += F("\" autocomplete=\"off\">");
    html += F("<label>Пароль</label>");
    html += F("<input name=\"sta_pass\" type=\"password\" maxlength=\"64\" placeholder=\"новый пароль\" autocomplete=\"new-password\">");

    html += F("<h2>Wi-Fi Bender</h2>");
    html += F("<label>Имя точки доступа</label>");
    html += F("<input name=\"ap_ssid\" maxlength=\"32\" value=\"");
    html += w.apSsid.length() ? w.apSsid : apSsidShow;
    html += F("\" autocomplete=\"off\">");
    html += F("<label>Пароль точки доступа</label>");
    html += F("<input name=\"ap_pass\" type=\"password\" maxlength=\"64\" placeholder=\"если нужно изменить\" autocomplete=\"new-password\">");

    String custom[RadioConfig::customStationMaxCount];
    uint8_t customCount = 0;
    nvsLoadCustomStations(custom, RadioConfig::customStationMaxCount, customCount);
    String stationsText;
    for (uint8_t i = 0; i < customCount; i++) {
        stationsText += custom[i];
        stationsText += '\n';
    }
    html += F("<h2>Интернет Радио</h2>");
    html += F("<label>Ссылки (одна строка = одна станция)</label>");
    html += F("<textarea name=\"stations\" rows=\"8\" style=\"width:100%;box-sizing:border-box;padding:8px;font-size:14px;\">");
    html += stationsText;
    html += F("</textarea>");
    html += F("<p class=\"muted\">Можно сохранить до ");
    html += String((int)RadioConfig::customStationMaxCount);
    html += F(" ссылок.</p>");

    int8_t trim[RadioConfig::matrixModuleCount] = {0, 0, 0, 0, 0};
    matrix_get_brightness_trim(trim, RadioConfig::matrixModuleCount);
    html += F("<h2>КАЛИБРОВКА МАТРИЦ</h2>");
    html += F("<div class=\"muted\">Клик по модулю: ярче. Кнопки -/+ : темнее/ярче.</div>");
    for (uint8_t i = 0; i < RadioConfig::matrixModuleCount; i++) {
        html += F("<input type=\"hidden\" id=\"mbr");
        html += String((int)i);
        html += F("\" name=\"mbr");
        html += String((int)i);
        html += F("\" value=\"");
        html += String((int)trim[i]);
        html += F("\">");
    }
    html += F("<div class=\"headWrap\">");
    html += F("<div class=\"headRow\">");
    html += F("<div class=\"mx\" onclick=\"adj(3,1)\"><div class=\"mxTitle\">ГЛАЗ ЛЕВЫЙ</div><div class=\"mxVal\" id=\"v3\"></div><div class=\"mxBtns\"><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,3,-1)\">-</button><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,3,1)\">+</button></div></div>");
    html += F("<div class=\"mx\" onclick=\"adj(4,1)\"><div class=\"mxTitle\">ГЛАЗ ПРАВЫЙ</div><div class=\"mxVal\" id=\"v4\"></div><div class=\"mxBtns\"><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,4,-1)\">-</button><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,4,1)\">+</button></div></div>");
    html += F("</div>");
    html += F("<div class=\"headRow\">");
    html += F("<div class=\"mx\" onclick=\"adj(0,1)\"><div class=\"mxTitle\">РОТ 1</div><div class=\"mxVal\" id=\"v0\"></div><div class=\"mxBtns\"><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,0,-1)\">-</button><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,0,1)\">+</button></div></div>");
    html += F("<div class=\"mx\" onclick=\"adj(1,1)\"><div class=\"mxTitle\">РОТ 2</div><div class=\"mxVal\" id=\"v1\"></div><div class=\"mxBtns\"><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,1,-1)\">-</button><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,1,1)\">+</button></div></div>");
    html += F("<div class=\"mx\" onclick=\"adj(2,1)\"><div class=\"mxTitle\">РОТ 3</div><div class=\"mxVal\" id=\"v2\"></div><div class=\"mxBtns\"><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,2,-1)\">-</button><button class=\"mxBtn\" type=\"button\" onclick=\"evtAdj(event,2,1)\">+</button></div></div>");
    html += F("</div>");
    html += F("</div>");
    html += F("<div class=\"mxBtns\" style=\"margin-top:8px;justify-content:flex-start;\">"
              "<button class=\"mxBtn\" type=\"button\" onclick=\"startCalib()\">НАЧАТЬ КАЛИБРОВКУ</button>"
              "</div>");

    html += F("<button class=\"btn\" type=\"submit\">СОХРАНИТЬ</button>");
    html += F("</form>"
              "<script>"
              "const MIN_B=0,MAX_B=15,CALIB_START=0;"
              "let calibTimer=0;"
              "function getV(i){const e=document.getElementById('mbr'+i);return parseInt(e.value||'0',10)||0;}"
              "function postCalib(){const b=[];for(let i=0;i<5;i++){b.push('mbr'+i+'='+encodeURIComponent(getV(i)));}fetch('/calib',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b.join('&')}).catch(()=>{});}"
              "function queueCalib(){if(calibTimer)clearTimeout(calibTimer);calibTimer=setTimeout(postCalib,120);}"
              "function setV(i,v,notify=true){if(v<MIN_B)v=MIN_B;if(v>MAX_B)v=MAX_B;document.getElementById('mbr'+i).value=v;const t=document.getElementById('v'+i);if(t)t.textContent=''+v;if(notify)queueCalib();}"
              "function adj(i,d){setV(i,getV(i)+d,true);}"
              "function evtAdj(ev,i,d){ev.stopPropagation();adj(i,d);}"
              "function startCalib(){for(let i=0;i<5;i++){setV(i,CALIB_START,false);}queueCalib();}"
              "for(let i=0;i<5;i++){setV(i,getV(i),false);}"
              "</script>"
              "</body></html>");

    web_send_close_connection();
    server.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
    wifi_touch_activity();
    if (server.method() != HTTP_POST) {
        web_send_close_connection();
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    String staSsid = server.arg("sta_ssid");
    staSsid.trim();
    if (staSsid.length() == 0) {
        web_send_close_connection();
        server.send(400, "text/plain", "SSID required");
        return;
    }
    String staPassIn = server.arg("sta_pass");
    String apSsidIn = server.arg("ap_ssid");
    apSsidIn.trim();
    String apPassIn = server.arg("ap_pass");
    String stationsIn = server.arg("stations");
    int8_t trim[RadioConfig::matrixModuleCount] = {0, 0, 0, 0, 0};
    for (uint8_t i = 0; i < RadioConfig::matrixModuleCount; i++) {
        String v = server.arg(String("mbr") + String((int)i));
        v.trim();
        trim[i] = (int8_t)constrain(v.toInt(), (int)RadioConfig::matrixBrightnessTrimMin,
                                     (int)RadioConfig::matrixBrightnessTrimMax);
    }

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
    String parsed[RadioConfig::customStationMaxCount];
    uint8_t parsedCount = 0;
    int from = 0;
    while (from < stationsIn.length() && parsedCount < RadioConfig::customStationMaxCount) {
        int nl = stationsIn.indexOf('\n', from);
        String line = (nl >= 0) ? stationsIn.substring(from, nl) : stationsIn.substring(from);
        line.replace("\r", "");
        line.trim();
        if (line.length() > 0) {
            parsed[parsedCount++] = line;
        }
        if (nl < 0) {
            break;
        }
        from = nl + 1;
    }
    nvsSaveCustomStations(parsed, parsedCount);
    matrix_set_brightness_trim(trim, RadioConfig::matrixModuleCount, true);

    web_send_close_connection();
    server.send(200, "text/html; charset=utf-8",
                 "<!DOCTYPE html><html><head><meta charset=utf-8></head><body>"
                 "<p>Сохранено. Перезагрузка…</p>"
                 "<script>setTimeout(function(){location.href='/';},3000);</script>"
                 "</body></html>");
    delay(300);
    ESP.restart();
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
