#include "NetworkManager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "esp_wifi.h"

#include "AppContext.h"
#include "Logging.h"
#include "ConfigStorage.h"

namespace {
constexpr uint8_t kDnsPort = 53;

void applyStationPowerProfile() {
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_15dBm);
}

void applyAccessPointPowerProfile() {
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);
}

bool connectWiFiSTA(const String& ssid, const String& pass, uint32_t timeoutMs = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  applyStationPowerProfile();
  LOGV("[WiFi] Connecting to '%s' ...\n", ssid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(200);
    if (kVerboseLogging) Serial.print('.');
  }
  if (kVerboseLogging) Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    LOGV("[WiFi] Connected. IP: %s RSSI:%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  LOGE_LN("[WiFi] Connect failed.");
  return false;
}

void startCaptivePortal() {
  auto& ctx = app();
  ctx.network.portalMode = true;

  WiFi.mode(WIFI_AP);
  applyAccessPointPowerProfile();
  IPAddress apIP(192, 168, 4, 1);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  String apSsid = String("ESP32CAM-") + ctx.device.id.substring(6);
  WiFi.softAP(apSsid.c_str());
  LOGV("[AP] SSID: %s  IP: %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  ctx.dnsServer.start(kDnsPort, "*", apIP);

  ctx.server.onNotFound([]() {
    auto& state = app();
    if (state.network.portalMode) {
      state.server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
      state.server.send(302, "text/plain", "");
      return;
    }
    state.server.send(404, "text/plain", "Not found");
  });

  ctx.server.on("/", HTTP_GET, []() {
    auto& state = app();
    String html =
      String(F("<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<style>body{font-family:sans-serif;margin:24px}label{display:block;margin:.6rem 0 .2rem}"
               "input,button{width:100%;padding:.6rem;font-size:16px}button{margin-top:1rem}</style>"
               "<h2>ESP32-CAM Kurulum</h2>")) +
      "<p>Device ID: <b>" + state.device.id + "</b></p>"
      "<form method='POST' action='/save'>"
      "<label>WiFi SSID</label><input name='ssid' required>"
      "<label>WiFi Password</label><input name='pass' type='password' required>"
      "<label>Backend URL (ex: http://192.168.1.10:8000)</label><input name='be' required>"
      "<label>Backend Token (Bearer)</label><input name='betok' type='password' required>"
      "<button type='submit'>Save & Reboot</button></form>"
      "<p style='margin-top:1rem;color:#666'>After reboot, local UI is disabled. Manage via your PC admin interface.</p>";
    state.server.send(200, "text/html", html);
  });

  ctx.server.on("/save", HTTP_POST, []() {
    auto& state = app();
    if (!state.server.hasArg("ssid") || !state.server.hasArg("pass") ||
        !state.server.hasArg("be") || !state.server.hasArg("betok")) {
      state.server.send(400, "text/plain", "Missing fields.");
      return;
    }
    state.network.ssid = state.server.arg("ssid");
    state.network.password = state.server.arg("pass");
    state.backend.baseUrl = state.server.arg("be");
    state.backend.token = state.server.arg("betok");
    if (state.backend.baseUrl.endsWith("/")) {
      state.backend.baseUrl.remove(state.backend.baseUrl.length() - 1);
    }
    state.upload.apiUrl = defaultUploadUrl(state.backend.baseUrl);
    state.upload.apiToken = state.backend.token;
    state.prefs.begin("cfg", false);
    state.prefs.putString("wifi_ssid", state.network.ssid);
    state.prefs.putString("wifi_pass", state.network.password);
    state.prefs.putString("be_url", state.backend.baseUrl);
    state.prefs.putString("be_tok", state.backend.token);
    state.prefs.putString("api_url", state.upload.apiUrl);
    state.prefs.putString("api_token", state.upload.apiToken);
    state.prefs.end();
    state.server.send(200, "text/html", "<meta charset='utf-8'>Saved. Rebooting...");
    delay(400);
    ESP.restart();
  });

  ctx.server.begin();
}
}  // namespace

void ensureWiFiOrPortal() {
  auto& ctx = app();
  bool missingCreds = ctx.network.ssid.isEmpty() || ctx.network.password.isEmpty() ||
                      ctx.backend.baseUrl.isEmpty() || ctx.backend.token.isEmpty();
  if (!missingCreds && connectWiFiSTA(ctx.network.ssid, ctx.network.password)) {
    ctx.network.portalMode = false;
    return;
  }
  startCaptivePortal();
}
