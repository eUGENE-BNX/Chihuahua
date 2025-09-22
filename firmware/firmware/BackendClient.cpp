#include "BackendClient.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>

#include "AppContext.h"
#include "CameraController.h"
#include "ConfigStorage.h"
#include "Logging.h"
#include "esp_camera.h"

namespace {
constexpr const char* kRegisterPath = "/api/register";
constexpr const char* kConfigPath = "/api/config";

String joinUrl(const String& base, const char* path) {
  if (base.length() == 0) return String(path ? path : "");
  if (!path || path[0] == '\0') return base;
  if (base.endsWith("/")) {
    if (path[0] == '/') return base + (path + 1);
    return base + String(path);
  }
  if (path[0] == '/') return base + String(path);
  return base + "/" + String(path);
}

String jsonGetString(const String& body, const char* key) {
  String needle = "\"" + String(key) + "\"";
  int i = body.indexOf(needle);
  if (i < 0) return "";
  i = body.indexOf(':', i + needle.length());
  if (i < 0) return "";
  int q1 = body.indexOf('\"', i + 1);
  if (q1 < 0) return "";
  int q2 = body.indexOf('\"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

long jsonGetInt(const String& body, const char* key, long defv = LONG_MIN) {
  String needle = "\"" + String(key) + "\"";
  int i = body.indexOf(needle);
  if (i < 0) return defv;
  i = body.indexOf(':', i + needle.length());
  if (i < 0) return defv;
  int j = i + 1;
  while (j < body.length() && (body[j] == ' ' || body[j] == '\t')) j++;
  int s = j;
  while (j < body.length() && ((body[j] >= '0' && body[j] <= '9') || body[j] == '-')) j++;
  if (s == j) return defv;
  return body.substring(s, j).toInt();
}

bool jsonGetBool(const String& body, const char* key, bool defVal) {
  String needle = "\"" + String(key) + "\"";
  int i = body.indexOf(needle);
  if (i < 0) return defVal;
  i = body.indexOf(':', i + needle.length());
  if (i < 0) return defVal;
  int j = i + 1;
  while (j < body.length() && (body[j] == ' ' || body[j] == '\t')) j++;
  if (body.startsWith("true", j)) return true;
  if (body.startsWith("false", j)) return false;
  return defVal;
}
}  // namespace

void registerWithBackend() {
  auto& ctx = app();
  if (ctx.backend.baseUrl.isEmpty() || ctx.backend.token.isEmpty() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  String url = joinUrl(ctx.backend.baseUrl, kRegisterPath);
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) {
    LOGE_LN("[BE] http.begin failed (register)");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + ctx.backend.token);

  String payload = String("{") +
    "\"deviceId\":\"" + ctx.device.id + "\"," +
    "\"uniqueId\":\"" + ctx.device.id + "\"," +
    "\"fw\":\"1.0.0\"," +
    "\"ip\":\"" + WiFi.localIP().toString() + "\"," +
    "\"rssi\":" + String(WiFi.RSSI()) + "," +
    "\"model\":\"ESP32-CAM-OV2640\"," +
    "\"chipModel\":\"" + String(ESP.getChipModel()) + "\"," +
    "\"chipRev\":" + String(ESP.getChipRevision()) + "," +
    "\"cores\":" + String(ESP.getChipCores()) + "," +
    "\"psram\":" + (psramFound() ? String("true") : String("false")) + "," +
    "\"flashSize\":" + String(ESP.getFlashChipSize()) + "," +
    "\"sdk\":\"" + String(ESP.getSdkVersion()) + "\"" +
  "}";

  http.setTimeout(15000);
  int code = http.POST((uint8_t*)payload.c_str(), payload.length());
  LOGV("[BE] register POST => %d\n", code);
  http.end();
}

bool fetchConfigFromBackend() {
  auto& ctx = app();
  if (ctx.backend.baseUrl.isEmpty() || ctx.backend.token.isEmpty() || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  String url = joinUrl(ctx.backend.baseUrl, kConfigPath);
  url += "?deviceId=" + ctx.device.id + "&rev=" + String(ctx.backend.revision);

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) {
    LOGE_LN("[BE] http.begin failed (config)");
    return false;
  }
  http.addHeader("Authorization", "Bearer " + ctx.backend.token);
  http.setTimeout(15000);

  int code = http.GET();
  if (code <= 0) {
    LOGE("[BE] GET config err: %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  long rev = jsonGetInt(body, "rev", LONG_MIN);

  String fsKey = jsonGetString(body, "framesize");
  long q = jsonGetInt(body, "jpegQuality", ctx.camera.jpegQualityTarget);
  long interval = jsonGetInt(body, "uploadIntervalSec", ctx.upload.intervalSec);
  String newUploadUrl = jsonGetString(body, "uploadUrl");
  String newUploadTok = jsonGetString(body, "uploadToken");
  bool newAuto = jsonGetBool(body, "autoUpload", ctx.upload.autoUpload);
  bool newLowLight = jsonGetBool(body, "lowLightBoost", ctx.lowLight.boostEnabled);

  if (fsKey.length()) {
    ctx.camera.frameSizeTarget = framesizeFromKey(fsKey);
    ctx.camera.frameSizeKeyTarget = fsKey;
  }
  if (q < 5) q = 5;
  if (q > 63) q = 63;
  ctx.camera.jpegQualityTarget = static_cast<int>(q);

  if (interval < 1) interval = 1;
  if (interval > 3600) interval = 3600;
  ctx.upload.intervalSec = static_cast<uint32_t>(interval);

  if (newUploadUrl.length()) ctx.upload.apiUrl = newUploadUrl;
  else if (ctx.upload.apiUrl.isEmpty()) ctx.upload.apiUrl = defaultUploadUrl(ctx.backend.baseUrl);
  if (newUploadTok.length()) ctx.upload.apiToken = newUploadTok;
  ctx.upload.autoUpload = newAuto;

  if (ctx.lowLight.boostEnabled != newLowLight) {
    ctx.lowLight.boostEnabled = newLowLight;
    resetLowLightState();
  }

  auto& target = ctx.camera.target;
  target.whitebal = jsonGetBool(body, "whitebal", target.whitebal);
  target.wbMode = static_cast<int>(jsonGetInt(body, "wbMode", target.wbMode));
  target.hmirror = jsonGetBool(body, "hmirror", target.hmirror);
  target.vflip = jsonGetBool(body, "vflip", target.vflip);
  target.brightness = static_cast<int>(jsonGetInt(body, "brightness", target.brightness));
  target.contrast = static_cast<int>(jsonGetInt(body, "contrast", target.contrast));
  target.saturation = static_cast<int>(jsonGetInt(body, "saturation", target.saturation));
  target.sharpness = static_cast<int>(jsonGetInt(body, "sharpness", target.sharpness));
  target.awbGain = jsonGetBool(body, "awbGain", target.awbGain);
  target.gainCtrl = jsonGetBool(body, "gainCtrl", target.gainCtrl);
  target.exposureCtrl = jsonGetBool(body, "exposureCtrl", target.exposureCtrl);
  long gc = jsonGetInt(body, "gainceiling", LONG_MIN);
  if (gc != LONG_MIN && gc >= 0) target.gainceilingIndex = static_cast<uint8_t>(gc);
  target.aeLevel = static_cast<int>(jsonGetInt(body, "aeLevel", target.aeLevel));
  target.lensCorr = jsonGetBool(body, "lensCorr", target.lensCorr);
  target.rawGma = jsonGetBool(body, "rawGma", target.rawGma);
  target.bpcEnabled = jsonGetBool(body, "bpc", target.bpcEnabled);
  target.wpcEnabled = jsonGetBool(body, "wpc", target.wpcEnabled);
  target.dcwEnabled = jsonGetBool(body, "dcw", target.dcwEnabled);
  target.colorbarEnabled = jsonGetBool(body, "colorbar", target.colorbarEnabled);
  target.specialEffect = static_cast<int>(jsonGetInt(body, "specialEffect", target.specialEffect));

  applyConfigIfNeeded();

  if (rev != LONG_MIN) {
    ctx.backend.revision = static_cast<uint32_t>(rev);
  }
  savePrefs();

  LOGV("[CFG] auto=%d int=%lus fs=%s q=%d url=%s toklen=%u\n",
       ctx.upload.autoUpload ? 1 : 0,
       static_cast<unsigned long>(ctx.upload.intervalSec),
       labelFromFramesize(ctx.camera.frameSizeTarget),
       ctx.camera.jpegQualityTarget,
       ctx.upload.apiUrl.c_str(),
       static_cast<unsigned>(ctx.upload.apiToken.length()));
  LOGV("[CFG] awb=%d wbMode=%d hmir=%d vflip=%d bri=%d con=%d sat=%d\n",
       target.whitebal ? 1 : 0,
       target.wbMode,
       target.hmirror ? 1 : 0,
       target.vflip ? 1 : 0,
       target.brightness,
       target.contrast,
       target.saturation);
  return true;
}

void testUploadConnectivity() {
  auto& ctx = app();
  if (!kVerboseLogging) return;
  if (ctx.upload.apiUrl.isEmpty() || WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, ctx.upload.apiUrl)) {
    LOGV_LN("[TEST] begin fail");
    return;
  }
  if (ctx.upload.apiToken.length()) {
    http.addHeader("Authorization", "Bearer " + ctx.upload.apiToken);
  }
  http.setTimeout(8000);
  int code = http.GET();
  LOGV("[TEST] GET %s => %d\n", ctx.upload.apiUrl.c_str(), code);
  http.end();
}

bool uploadFrameToApi(const uint8_t* data, size_t len) {
  auto& ctx = app();
  if (ctx.upload.apiUrl.isEmpty()) {
    ctx.http.lastError = "No API URL";
    ctx.http.lastStatus = 0;
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    ctx.http.lastError = "No WiFi";
    ctx.http.lastStatus = 0;
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, ctx.upload.apiUrl)) {
    ctx.http.lastError = "http.begin()";
    ctx.http.lastStatus = 0;
    return false;
  }

  http.addHeader("Content-Type", "image/jpeg", true);
  http.addHeader("X-Device-ID", ctx.device.id);
  http.addHeader("X-Frame-Size", ctx.camera.lastUsedFrameSizeKey);
  http.addHeader("X-JPEG-Quality", String(ctx.camera.jpegQuality));
  char fname[64];
  snprintf(fname, sizeof(fname), "%s_%lu.jpg", ctx.device.id.c_str(), static_cast<unsigned long>(millis()));
  http.addHeader("X-File-Name", fname);
  http.addHeader("X-Device-Time", String(static_cast<unsigned long>(time(nullptr))));
  if (ctx.upload.apiToken.length()) {
    http.addHeader("Authorization", "Bearer " + ctx.upload.apiToken);
  }
  http.setTimeout(15000);

  int code = http.POST(const_cast<uint8_t*>(data), len);
  ctx.http.lastStatus = code;

  if (code <= 0) {
    ctx.http.lastError = http.errorToString(code);
    http.end();
    return false;
  }
  String payload = http.getString();
  ctx.http.lastError = payload;
  http.end();
  return (code >= 200 && code < 300);
}

bool captureAndUploadOnce() {
  auto& ctx = app();
  if (!ctx.camera.inited) return false;

  camera_fb_t* fb = safeGrab();
  if (!fb) {
    ctx.http.lastError = "fb=null";
    ctx.http.lastStatus = 0;
    return false;
  }

  evaluateLowLightMetrics();
  bool ok = uploadFrameToApi(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return ok;
}
