/*
  ESP32-CAM + OV2640 ??? Headless ??? Remote Config incl. AWB/WB/HMirror/VFlip/Bri/Con/Sat (Single .ino)

  - Uzaktan ayarlar (backend): framesize, jpegQuality, uploadIntervalSec, autoUpload,
    whitebal, wbMode, hmirror, vflip, brightness, contrast, saturation.
  - Register payload ekstra kimlik: uniqueId(=deviceId), chipModel, chipRev, cores, psram, flashSize, sdk.
  - Stabilite: modem sleep (WIFI_PS_MIN_MODEM), fb_count=1, GRAB_WHEN_EMPTY, XCLK fallback (20/16.5/10 MHz).
  - Log: 115200 8N1 ASCII. Boot'ta ba??lant?? testi + ilk kare denemesi (autoUpload a????ksa).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "esp_camera.h"
#include <time.h>
#include "esp_log.h"
#include <limits.h>
#include "esp_event.h"
#include "esp_wifi.h"

constexpr bool kVerboseLogging = false;
#define LOGV(fmt, ...) do { if (kVerboseLogging) Serial.printf((fmt), ##__VA_ARGS__); } while (0)
#define LOGV_LN(msg) do { if (kVerboseLogging) Serial.println((msg)); } while (0)
#define LOGE(fmt, ...) Serial.printf((fmt), ##__VA_ARGS__)
#define LOGE_LN(msg) Serial.println((msg))

constexpr uint16_t kLowLightAecActivate = 850;
constexpr uint16_t kLowLightAecRelease = 700;
constexpr uint8_t  kLowLightGainActivate = 24;
constexpr uint8_t  kLowLightGainRelease = 18;
constexpr uint8_t  kLowLightScoreThreshold = 3;
constexpr uint8_t  kLowLightScoreMax = 6;
constexpr unsigned long kLowLightLogIntervalMs = 10000UL;


// ==== Camera Pins (AI Thinker) ===============================================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ==== Globals =================================================================
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

const byte DNS_PORT = 53;
bool portalMode = false;

unsigned long lastUploadMs = 0;
unsigned long lastCfgPollMs = 0;
uint32_t cfgPollIntervalSec = 5;

// WiFi & Backend
String wifiSsid, wifiPass;
String backendUrl = "";
String backendToken = "";
const char* BE_PATH_REGISTER = "/api/register";
const char* BE_PATH_CONFIG   = "/api/config";
uint32_t cfgRevision         = 0;

// Upload hedefi
String apiUrl = "";
String apiToken = "";

// Aktif/Target ayarlar
bool autoUpload = false;
uint32_t uploadIntervalSec = 10;
int jpegQuality = 12;
framesize_t frameSize = FRAMESIZE_VGA;
String frameSizeKey = "VGA";

int jpegQualityTarget = 12;
framesize_t frameSizeTarget = FRAMESIZE_VGA;
String frameSizeKeyTarget = "VGA";

// >>> Yeni: Gelimi kamera ayarlar (aktif + hedef)
bool whitebal = true, whitebalTarget = true;       // AWB
int  wbMode = 0, wbModeTarget = 0;                 // 0 Auto, 1 Sunny, 2 Cloudy, 3 Office, 4 Home
bool hmirror = false, hmirrorTarget = false;
bool vflip  = false,  vflipTarget  = false;
int  brightness = 0, brightnessTarget = 0;         // -2..2
int  contrast   = 1, contrastTarget   = 1;         // -2..2
int  saturation = 1, saturationTarget = 1;         // -2..2
int  sharpness  = 1, sharpnessTarget  = 1;         // -2..2
bool awbGain = true, awbGainTarget = true;
bool gainCtrl = true, gainCtrlTarget = true;
bool exposureCtrl = true, exposureCtrlTarget = true;
uint8_t gainceilingIndex = 4, gainceilingIndexTarget = 4;   // maps to GAINCEILING_32X
int  aeLevel = 0, aeLevelTarget = 0;               // -2..2
bool lensCorr = true, lensCorrTarget = true;
bool rawGma = true, rawGmaTarget = true;
bool bpcEnabled = true, bpcTarget = true;
bool wpcEnabled = true, wpcTarget = true;
bool dcwEnabled = true, dcwTarget = true;
bool colorbarEnabled = false, colorbarTarget = false;
int  specialEffect = 0, specialEffectTarget = 0;   // 0..6

// Son kullan??lan FS (fallback olursa farkl?? olabilir)
String lastUsedFrameSizeKey = "VGA";

// Durum
int lastHttpStatus = 0;
String lastHttpError = "-";
String deviceId;
bool cameraInited = false;
int currentXclkHz = 20000000;
uint8_t failedGrabStreak = 0;
unsigned long lastCamReinitMs = 0;

bool lowLightBoostEnabled = true;
bool lowLightActive = false;
uint8_t lowLightScore = 0;
unsigned long lastLowLightLogMs = 0;

void applyStationPowerProfile() {
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_15dBm);
}

void applyAccessPointPowerProfile() {
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);
}

void applyLowLightProfile(sensor_t* s, bool active) {
  if (!s) return;
  if (!active) {
    s->set_aec2(s, 0);
    return;
  }
  s->set_gain_ctrl(s, 1);
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_gainceiling(s, GAINCEILING_32X);
  s->set_aec2(s, 1);
  int boostAe = aeLevelTarget;
  if (boostAe < 0) boostAe = 0;
  if (boostAe > 2) boostAe = 2;
  s->set_ae_level(s, boostAe);
  s->set_dcw(s, 1);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
}

void refreshLowLightProfile() {
  if (!cameraInited) return;
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  applyManualSensorParams(s);
  applyLowLightProfile(s, lowLightBoostEnabled && lowLightActive);
}

void resetLowLightState() {
  lowLightScore = 0;
  lowLightActive = false;
  refreshLowLightProfile();
}

gainceiling_t gainceilingFromIndex(int idx) {
  static const gainceiling_t kMap[] = {
    GAINCEILING_2X,
    GAINCEILING_4X,
    GAINCEILING_8X,
    GAINCEILING_16X,
    GAINCEILING_32X,
    GAINCEILING_64X,
  };
  if (idx < 0) idx = 0;
  if (idx >= (int)(sizeof(kMap) / sizeof(kMap[0]))) {
    idx = (sizeof(kMap) / sizeof(kMap[0])) - 1;
  }
  return kMap[idx];
}

void applyManualSensorParams(sensor_t* s) {
  if (!s) return;
  int wb  = constrain(wbModeTarget, 0, 4);
  int bri = constrain(brightnessTarget, -2, 2);
  int con = constrain(contrastTarget,   -2, 2);
  int sat = constrain(saturationTarget, -2, 2);
  int shp = constrain(sharpnessTarget,  -2, 2);
  int ae  = constrain(aeLevelTarget,    -2, 2);
  int spe = constrain(specialEffectTarget, 0, 6);
  uint8_t gcIdx = gainceilingIndexTarget;
  if (gcIdx > 5) gcIdx = 5;

  s->set_whitebal(s, whitebalTarget ? 1 : 0);
  s->set_wb_mode(s, wb);
  s->set_hmirror(s, hmirrorTarget ? 1 : 0);
  s->set_vflip(s,  vflipTarget  ? 1 : 0);
  s->set_brightness(s, bri);
  s->set_contrast(s,   con);
  s->set_saturation(s, sat);
  s->set_sharpness(s,  shp);
  s->set_awb_gain(s, awbGainTarget ? 1 : 0);
  s->set_gain_ctrl(s, gainCtrlTarget ? 1 : 0);
  s->set_exposure_ctrl(s, exposureCtrlTarget ? 1 : 0);
  s->set_gainceiling(s, gainceilingFromIndex(gcIdx));
  s->set_ae_level(s, ae);
  s->set_lenc(s, lensCorrTarget ? 1 : 0);
  s->set_raw_gma(s, rawGmaTarget ? 1 : 0);
  s->set_bpc(s, bpcTarget ? 1 : 0);
  s->set_wpc(s, wpcTarget ? 1 : 0);
  s->set_dcw(s, dcwTarget ? 1 : 0);
  s->set_colorbar(s, colorbarTarget ? 1 : 0);
  s->set_special_effect(s, spe);

  whitebal = whitebalTarget;
  wbMode = wb;
  hmirror = hmirrorTarget;
  vflip = vflipTarget;
  brightness = bri;
  contrast = con;
  saturation = sat;
  sharpness = shp;
  awbGain = awbGainTarget;
  gainCtrl = gainCtrlTarget;
  exposureCtrl = exposureCtrlTarget;
  gainceilingIndex = gcIdx;
  aeLevel = ae;
  lensCorr = lensCorrTarget;
  rawGma = rawGmaTarget;
  bpcEnabled = bpcTarget;
  wpcEnabled = wpcTarget;
  dcwEnabled = dcwTarget;
  colorbarEnabled = colorbarTarget;
  specialEffect = spe;
}

void updateLowLightObserver(uint16_t aecValue, uint8_t agcGain) {
  if (!lowLightBoostEnabled) return;
  bool requestBoost = (aecValue >= kLowLightAecActivate) || (agcGain >= kLowLightGainActivate);
  bool requestRelease = (aecValue <= kLowLightAecRelease) && (agcGain <= kLowLightGainRelease);
  if (requestBoost) {
    if (lowLightScore < kLowLightScoreMax) lowLightScore++;
  } else if (requestRelease) {
    if (lowLightScore > 0) lowLightScore--;
  } else if (lowLightScore > 0) {
    lowLightScore--;
  }
  bool newActive = (lowLightScore >= kLowLightScoreThreshold);
  if (newActive != lowLightActive) {
    lowLightActive = newActive;
    refreshLowLightProfile();
    LOGV("[LowLight] %s (aec=%u gain=%u score=%u)\n", newActive ? "enabled" : "disabled", aecValue, agcGain, lowLightScore);
    lastLowLightLogMs = millis();
  } else if (kVerboseLogging) {
    unsigned long now = millis();
    if (now - lastLowLightLogMs > kLowLightLogIntervalMs) {
      LOGV("[LowLight] state=%d aec=%u gain=%u score=%u\n", newActive ? 1 : 0, aecValue, agcGain, lowLightScore);
      lastLowLightLogMs = now;
    }
  }
}

void evaluateLowLightMetrics() {
  if (!lowLightBoostEnabled || !cameraInited) return;
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  updateLowLightObserver(s->status.aec_value, s->status.agc_gain);
}

// ==== Helpers =================================================================
String getDeviceIdHex() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[13];
  sprintf(buf, "%012llX", mac);
  return String(buf);
}
struct FsItem { const char* key; framesize_t val; const char* label; };
FsItem FS_MAP[] = {
  {"QQVGA",  FRAMESIZE_QQVGA,  "160x120 (QQVGA)"},
  {"QVGA",   FRAMESIZE_QVGA,   "320x240 (QVGA)"},
  {"CIF",    FRAMESIZE_CIF,    "400x296 (CIF)"},
  {"VGA",    FRAMESIZE_VGA,    "640x480 (VGA)"},
  {"SVGA",   FRAMESIZE_SVGA,   "800x600 (SVGA)"},
  {"XGA",    FRAMESIZE_XGA,    "1024x768 (XGA)"},
  {"SXGA",   FRAMESIZE_SXGA,   "1280x1024 (SXGA)"},
  {"UXGA",   FRAMESIZE_UXGA,   "1600x1200 (UXGA)"},
};
framesize_t framesizeFromKey(const String& key){ for (auto &it:FS_MAP) if(key.equalsIgnoreCase(it.key)) return it.val; return FRAMESIZE_VGA; }
const char* keyFromFramesize(framesize_t fs){ for (auto &it:FS_MAP) if(it.val==fs) return it.key; return "VGA"; }
const char* labelFromFramesize(framesize_t fs){ for (auto &it:FS_MAP) if(it.val==fs) return it.label; return "640x480 (VGA)"; }

// ==== NVS =====================================================================
void loadPrefs() {
  prefs.begin("cfg", true);
  wifiSsid   = prefs.getString("wifi_ssid", "");
  wifiPass   = prefs.getString("wifi_pass", "");
  backendUrl = prefs.getString("be_url", "");
  backendToken = prefs.getString("be_tok", "");
  apiUrl     = prefs.getString("api_url", "");
  apiToken   = prefs.getString("api_token", "");
  jpegQuality = prefs.getInt("jpeg_q", 12);
  frameSizeKey = prefs.getString("fs_key", "VGA");
  autoUpload = prefs.getBool("auto_up", false);
  uploadIntervalSec = prefs.getUInt("up_int", 10);
  cfgRevision = prefs.getUInt("cfg_rev", 0);

  // yeni geli??mi?? ayarlar
  whitebal = prefs.getBool("awb", true);
  wbMode = prefs.getInt("wbm", 0);
  hmirror = prefs.getBool("hmr", false);
  vflip = prefs.getBool("vfl", false);
  brightness = prefs.getInt("bri", 0);
  contrast   = prefs.getInt("con", 1);
  saturation = prefs.getInt("sat", 1);
  sharpness  = prefs.getInt("shp", 1);
  awbGain    = prefs.getBool("awg", true);
  gainCtrl   = prefs.getBool("agc", true);
  exposureCtrl = prefs.getBool("aec", true);
  gainceilingIndex = (uint8_t)prefs.getInt("gci", 4);
  aeLevel = prefs.getInt("ael", 0);
  lensCorr = prefs.getBool("lenc", true);
  rawGma = prefs.getBool("rgm", true);
  bpcEnabled = prefs.getBool("bpc", true);
  wpcEnabled = prefs.getBool("wpc", true);
  dcwEnabled = prefs.getBool("dcw", true);
  colorbarEnabled = prefs.getBool("clb", false);
  specialEffect = prefs.getInt("spe", 0);
  lowLightBoostEnabled = prefs.getBool("low_light", true);
  prefs.end();

  // Aktif <-> Target e??itle
  frameSize = framesizeFromKey(frameSizeKey);
  frameSizeTarget = frameSize; frameSizeKeyTarget = frameSizeKey;
  if (jpegQuality < 5) jpegQuality = 5; if (jpegQuality > 63) jpegQuality = 63;
  jpegQualityTarget = jpegQuality;
  if (uploadIntervalSec < 1) uploadIntervalSec = 1; if (uploadIntervalSec > 3600) uploadIntervalSec = 3600;

  whitebalTarget = whitebal;
  wbModeTarget   = wbMode;
  hmirrorTarget  = hmirror;
  vflipTarget    = vflip;
  brightness = constrain(brightness, -2, 2);
  contrast   = constrain(contrast,   -2, 2);
  saturation = constrain(saturation, -2, 2);
  sharpness  = constrain(sharpness,  -2, 2);
  aeLevel    = constrain(aeLevel,    -2, 2);
  specialEffect = constrain(specialEffect, 0, 6);
  if (gainceilingIndex > 5) gainceilingIndex = 5;

  brightnessTarget = brightness;
  contrastTarget   = contrast;
  saturationTarget = saturation;
  sharpnessTarget  = sharpness;
  awbGainTarget    = awbGain;
  gainCtrlTarget   = gainCtrl;
  exposureCtrlTarget = exposureCtrl;
  gainceilingIndexTarget = gainceilingIndex;
  aeLevelTarget = aeLevel;
  lensCorrTarget = lensCorr;
  rawGmaTarget = rawGma;
  bpcTarget = bpcEnabled;
  wpcTarget = wpcEnabled;
  dcwTarget = dcwEnabled;
  colorbarTarget = colorbarEnabled;
  specialEffectTarget = specialEffect;

  resetLowLightState();
}
void savePrefs() {
  prefs.begin("cfg", false);
  prefs.putString("wifi_ssid", wifiSsid);
  prefs.putString("wifi_pass", wifiPass);
  prefs.putString("be_url", backendUrl);
  prefs.putString("be_tok", backendToken);
  prefs.putString("api_url", apiUrl);
  prefs.putString("api_token", apiToken);
  prefs.putInt("jpeg_q", jpegQualityTarget);
  prefs.putString("fs_key", frameSizeKeyTarget);
  prefs.putBool("auto_up", autoUpload);
  prefs.putUInt("up_int", uploadIntervalSec);
  prefs.putUInt("cfg_rev", cfgRevision);
  // yeni geli??mi?? ayarlar
  prefs.putBool("awb", whitebalTarget);
  prefs.putInt("wbm", wbModeTarget);
  prefs.putBool("hmr", hmirrorTarget);
  prefs.putBool("vfl", vflipTarget);
  prefs.putInt("bri", brightnessTarget);
  prefs.putInt("con", contrastTarget);
  prefs.putInt("sat", saturationTarget);
  prefs.putInt("shp", sharpnessTarget);
  prefs.putBool("awg", awbGainTarget);
  prefs.putBool("agc", gainCtrlTarget);
  prefs.putBool("aec", exposureCtrlTarget);
  prefs.putInt("gci", gainceilingIndexTarget);
  prefs.putInt("ael", aeLevelTarget);
  prefs.putBool("lenc", lensCorrTarget);
  prefs.putBool("rgm", rawGmaTarget);
  prefs.putBool("bpc", bpcTarget);
  prefs.putBool("wpc", wpcTarget);
  prefs.putBool("dcw", dcwTarget);
  prefs.putBool("clb", colorbarTarget);
  prefs.putInt("spe", specialEffectTarget);
  prefs.putBool("low_light", lowLightBoostEnabled);
  prefs.end();
}

// ==== Camera init/apply =======================================================
bool initCameraWithXclk(int xclkHz) {
  currentXclkHz = xclkHz;

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = xclkHz;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size   = frameSize;
  config.jpeg_quality = jpegQuality;
  config.fb_count     = 2;
  config.fb_location  = psramFound()?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    LOGE("[CAM] init err=0x%x @%dHz\n", err, xclkHz);
    cameraInited = false;
    return false;
  }

  // ??lk parametreler
  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, frameSize);
    s->set_quality(s, jpegQuality);
  }
  cameraInited = true;
  LOGV("[CAM] init ok @%dHz, %s, q=%d\n", xclkHz, labelFromFramesize(frameSize), jpegQuality);
  applyAdvancedParams();
  refreshLowLightProfile();
  return true;
}
bool initCamera() {
  if (initCameraWithXclk(20000000)) return true;
  delay(200);
  if (initCameraWithXclk(16500000)) return true;
  delay(200);
  if (initCameraWithXclk(10000000)) return true;
  return false;
}

// >>> Yeni: geli??mi?? parametreleri uygula (her ??a??r??da)
void applyAdvancedParams() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  applyManualSensorParams(s);
  if (lowLightBoostEnabled && lowLightActive) {
    applyLowLightProfile(s, true);
  }
}

void applyConfigIfNeeded() {
  if (!cameraInited) return;
  bool needReinit = (frameSizeTarget != frameSize);

  if (needReinit) {
    LOGV("[CFG] reinit FS: %s -> %s\n",
      labelFromFramesize(frameSize), labelFromFramesize(frameSizeTarget));
    esp_camera_deinit(); cameraInited = false;
    frameSize = frameSizeTarget; jpegQuality = jpegQualityTarget;
    delay(150);
    if (!initCamera()) { LOGE_LN("[CFG] reinit failed"); return; }
  } else if (jpegQualityTarget != jpegQuality) {
    sensor_t* s = esp_camera_sensor_get();
    if (s) { s->set_quality(s, jpegQualityTarget); jpegQuality = jpegQualityTarget; LOGV("[CFG] set q=%d\n", jpegQuality); }
  }
  // Geli??mi?? parametreleri her seferinde uygula
  applyAdvancedParams();
  if (!lowLightBoostEnabled) {
    resetLowLightState();
  } else {
    refreshLowLightProfile();
  }
}

void maybeReinitLowerXclk() {
  int next = currentXclkHz;
  if (currentXclkHz > 16500000) next = 16500000;
  else if (currentXclkHz > 10000000) next = 10000000;
  if (next != currentXclkHz) {
    LOGV("[CAM] re-init lower XCLK: %d -> %d\n", currentXclkHz, next);
    esp_camera_deinit(); cameraInited = false; delay(200);
    initCameraWithXclk(next);
  }
}

// ==== Safe grab ==============================================================
camera_fb_t* safeGrab() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) { failedGrabStreak = 0; lastUsedFrameSizeKey = keyFromFramesize(frameSize); return fb; }

  failedGrabStreak++;
  if (failedGrabStreak == 3) {
    LOGE("[CAM] fb_get failed (streak=%u)\n", failedGrabStreak);
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    framesize_t wanted = frameSize;
    framesize_t fallback[] = {FRAMESIZE_SVGA, FRAMESIZE_VGA, FRAMESIZE_QVGA};
    for (auto fs : fallback) {
      if (fs >= wanted) continue;
      s->set_framesize(s, fs);
      camera_fb_t* fb2 = esp_camera_fb_get();
      if (fb2) {
        lastUsedFrameSizeKey = keyFromFramesize(fs);
        s->set_framesize(s, wanted);
        failedGrabStreak = 0;
        return fb2;
      }
    }
    s->set_framesize(s, wanted);
  }

  if (failedGrabStreak >= 3 && millis() - lastCamReinitMs > 7000) {
    lastCamReinitMs = millis();
    maybeReinitLowerXclk();
  }
  return nullptr;
}

// ==== WiFi / Portal =========================================================
bool connectWiFiSTA(const String& ssid, const String& pass, uint32_t timeoutMs=15000) {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  applyStationPowerProfile();
  LOGV("[WiFi] Connecting to '%s' ...\n", ssid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) { delay(200); if (kVerboseLogging) Serial.print("."); }
  if (kVerboseLogging) Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    LOGV("[WiFi] Connected. IP: %s RSSI:%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  LOGE_LN("[WiFi] Connect failed.");
  return false;
}
void startCaptivePortal() {
  portalMode = true;
  WiFi.mode(WIFI_AP);
  applyAccessPointPowerProfile();
  IPAddress apIP(192,168,4,1), netMsk(255,255,255,0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  String apSsid = "ESP32CAM-" + deviceId.substring(6);
  WiFi.softAP(apSsid.c_str()); // Open AP
  LOGV("[AP] SSID: %s  IP: %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  dnsServer.start(DNS_PORT, "*", apIP);

  server.onNotFound([](){
    if (portalMode) { server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true); server.send(302, "text/plain", ""); return; }
    server.send(404, "text/plain", "Not found");
  });
  server.on("/", HTTP_GET, [](){
    String html =
      String(F("<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<style>body{font-family:sans-serif;margin:24px}label{display:block;margin:.6rem 0 .2rem}"
               "input,button{width:100%;padding:.6rem;font-size:16px}button{margin-top:1rem}</style>"
               "<h2>ESP32-CAM Kurulum</h2>")) +
      "<p>Device ID: <b>" + deviceId + "</b></p>"
      "<form method='POST' action='/save'>"
      "<label>WiFi SSID</label><input name='ssid' required>"
      "<label>WiFi Password</label><input name='pass' type='password' required>"
      "<label>Backend URL (ex: http://192.168.1.10:8000)</label><input name='be' required>"
      "<label>Backend Token (Bearer)</label><input name='betok' type='password' required>"
      "<button type='submit'>Save & Reboot</button></form>"
      "<p style='margin-top:1rem;color:#666'>After reboot, local UI is disabled. Manage via your PC admin interface.</p>";
    server.send(200, "text/html", html);
  });
  server.on("/save", HTTP_POST, [](){
    if (!server.hasArg("ssid") || !server.hasArg("pass") || !server.hasArg("be") || !server.hasArg("betok")) { server.send(400,"text/plain","Missing fields."); return; }
    wifiSsid = server.arg("ssid"); wifiPass = server.arg("pass");
    backendUrl = server.arg("be"); backendToken = server.arg("betok");
    if (backendUrl.endsWith("/")) backendUrl.remove(backendUrl.length()-1);
    prefs.begin("cfg", false);
    prefs.putString("wifi_ssid", wifiSsid);
    prefs.putString("wifi_pass", wifiPass);
    prefs.putString("be_url", backendUrl);
    prefs.putString("be_tok", backendToken);
    prefs.end();
    server.send(200,"text/html","<meta charset='utf-8'>Saved. Rebooting...");
    delay(400); ESP.restart();
  });
  server.begin();
}
void ensureWiFiOrPortal() {
  bool needPortal = (wifiSsid.length()==0 || wifiPass.length()==0 || backendUrl.length()==0 || backendToken.length()==0);
  if (!needPortal && connectWiFiSTA(wifiSsid, wifiPass)) { portalMode=false; return; }
  startCaptivePortal();
}

// ==== HTTP utils & JSON mini =================================================
String joinUrl(const String& base, const char* path) {
  if (base.length() == 0) return String(path ? path : "");
  if (!path || path[0] == '\0') return base;
  if (base.endsWith("/")) { if (path[0] == '/') return base + (path + 1); return base + String(path); }
  else { if (path[0] == '/') return base + String(path); return base + "/" + String(path); }
}
String jsonGetString(const String& body, const char* key) {
  String k = "\"" + String(key) + "\"";
  int i = body.indexOf(k); if (i < 0) return "";
  i = body.indexOf(':', i + k.length()); if (i < 0) return "";
  int q1 = body.indexOf('\"', i+1); if (q1 < 0) return "";
  int q2 = body.indexOf('\"', q1+1); if (q2 < 0) return "";
  return body.substring(q1+1, q2);
}
long jsonGetInt(const String& body, const char* key, long defv = LONG_MIN) {
  String k = "\"" + String(key) + "\"";
  int i = body.indexOf(k); if (i < 0) return defv;
  i = body.indexOf(':', i + k.length()); if (i < 0) return defv;
  int j = i+1; while (j < (int)body.length() && (body[j]==' '||body[j]=='\t')) j++;
  int s = j; while (j < (int)body.length() && ((body[j]>='0'&&body[j]<='9')||body[j]=='-')) j++;
  if (s==j) return defv; return body.substring(s, j).toInt();
}
bool jsonGetBool(const String& body, const char* key, bool defVal) {
  String k = "\"" + String(key) + "\"";
  int i = body.indexOf(k); if (i < 0) return defVal;
  i = body.indexOf(':', i + k.length()); if (i < 0) return defVal;
  int j = i+1; while (j < (int)body.length() && (body[j]==' '||body[j]=='\t')) j++;
  if (body.startsWith("true", j)) return true;
  if (body.startsWith("false", j)) return false;
  return defVal;
}

// ==== Backend register / config ==============================================
void registerWithBackend() {
  if (backendUrl.length()==0 || backendToken.length()==0 || WiFi.status()!=WL_CONNECTED) return;
  String url = joinUrl(backendUrl, BE_PATH_REGISTER);
  WiFiClient client; HTTPClient http;
  if (!http.begin(client, url)) { LOGE_LN("[BE] http.begin failed (register)"); return; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + backendToken);

  // Ek kimlik bilgileri
  String payload = String("{") +
    "\"deviceId\":\""+deviceId+"\","+
    "\"uniqueId\":\""+deviceId+"\","+
    "\"fw\":\"1.0.0\","+
    "\"ip\":\""+WiFi.localIP().toString()+"\","+
    "\"rssi\":"+String(WiFi.RSSI())+","+
    "\"model\":\"ESP32-CAM-OV2640\","+
    "\"chipModel\":\""+String(ESP.getChipModel())+"\","+
    "\"chipRev\":"+String(ESP.getChipRevision())+","+
    "\"cores\":"+String(ESP.getChipCores())+","+
    "\"psram\":"+(psramFound() ? String("true") : String("false"))+","+
    "\"flashSize\":"+String(ESP.getFlashChipSize())+","+
    "\"sdk\":\""+String(ESP.getSdkVersion())+"\"" +
  "}";

  http.setTimeout(15000);
  int code = http.POST((uint8_t*)payload.c_str(), payload.length());
  LOGV("[BE] register POST => %d\n", code);
  http.end();
}

bool fetchConfigFromBackend() {
  if (backendUrl.length()==0 || backendToken.length()==0 || WiFi.status()!=WL_CONNECTED) return false;

  String url = joinUrl(backendUrl, BE_PATH_CONFIG);
  url += "?deviceId=" + deviceId + "&rev=" + String(cfgRevision);

  WiFiClient client; HTTPClient http;
  if (!http.begin(client, url)) { LOGE_LN("[BE] http.begin failed (config)"); return false; }
  http.addHeader("Authorization", "Bearer " + backendToken);
  http.setTimeout(15000);

  int code = http.GET();
  if (code <= 0) { LOGE("[BE] GET config err: %d\n", code); http.end(); return false; }
  String body = http.getString(); http.end();

  long rev = jsonGetInt(body, "rev", LONG_MIN);

  // Temel ayarlar
  String fsKey = jsonGetString(body, "framesize");
  long q = jsonGetInt(body, "jpegQuality", jpegQualityTarget);
  long ival = jsonGetInt(body, "uploadIntervalSec", uploadIntervalSec);
  String newUploadUrl = jsonGetString(body, "uploadUrl");
  String newUploadTok = jsonGetString(body, "uploadToken");
  bool newAuto = jsonGetBool(body, "autoUpload", autoUpload);
  bool newLowLight = jsonGetBool(body, "lowLightBoost", lowLightBoostEnabled);

  if (fsKey.length()) { frameSizeTarget = framesizeFromKey(fsKey); frameSizeKeyTarget = fsKey; }
  if (q < 5) q = 5; if (q > 63) q = 63; jpegQualityTarget = (int)q;
  if (ival < 1) ival = 1; if (ival > 3600) ival = 3600; uploadIntervalSec = (uint32_t)ival;
  if (newUploadUrl.length()) apiUrl = newUploadUrl;
  if (newUploadTok.length()) apiToken = newUploadTok;
  autoUpload = newAuto;
  if (lowLightBoostEnabled != newLowLight) {
    lowLightBoostEnabled = newLowLight;
    resetLowLightState();
  }

  // >>> Yeni: geli??mi?? ayarlar
  whitebalTarget   = jsonGetBool(body, "whitebal", whitebalTarget);
  wbModeTarget     = (int)jsonGetInt(body, "wbMode", wbModeTarget);
  hmirrorTarget    = jsonGetBool(body, "hmirror", hmirrorTarget);
  vflipTarget      = jsonGetBool(body, "vflip",   vflipTarget);
  brightnessTarget = (int)jsonGetInt(body, "brightness", brightnessTarget);
  contrastTarget   = (int)jsonGetInt(body, "contrast",   contrastTarget);
  saturationTarget = (int)jsonGetInt(body, "saturation", saturationTarget);
  sharpnessTarget  = (int)jsonGetInt(body, "sharpness", sharpnessTarget);
  awbGainTarget    = jsonGetBool(body, "awbGain", awbGainTarget);
  gainCtrlTarget   = jsonGetBool(body, "gainCtrl", gainCtrlTarget);
  exposureCtrlTarget = jsonGetBool(body, "exposureCtrl", exposureCtrlTarget);
  {
    long gc = jsonGetInt(body, "gainceiling", LONG_MIN);
    if (gc != LONG_MIN && gc >= 0) gainceilingIndexTarget = (uint8_t)gc;
  }
  aeLevelTarget    = (int)jsonGetInt(body, "aeLevel", aeLevelTarget);
  lensCorrTarget   = jsonGetBool(body, "lensCorr", lensCorrTarget);
  rawGmaTarget     = jsonGetBool(body, "rawGma", rawGmaTarget);
  bpcTarget        = jsonGetBool(body, "bpc", bpcTarget);
  wpcTarget        = jsonGetBool(body, "wpc", wpcTarget);
  dcwTarget        = jsonGetBool(body, "dcw", dcwTarget);
  colorbarTarget   = jsonGetBool(body, "colorbar", colorbarTarget);
  specialEffectTarget = (int)jsonGetInt(body, "specialEffect", specialEffectTarget);

  applyConfigIfNeeded();

  if (rev != LONG_MIN) cfgRevision = (uint32_t)rev;
  savePrefs();

  LOGV("[CFG] auto=%d int=%lus fs=%s q=%d url=%s toklen=%u\n",
    autoUpload?1:0, (unsigned long)uploadIntervalSec, labelFromFramesize(frameSizeTarget),
    jpegQualityTarget, apiUrl.c_str(), (unsigned)apiToken.length());
  LOGV("[CFG] awb=%d wbMode=%d hmir=%d vflip=%d bri=%d con=%d sat=%d\n",
    whitebalTarget?1:0, wbModeTarget, hmirrorTarget?1:0, vflipTarget?1:0,
    brightnessTarget, contrastTarget, saturationTarget);
  return true;
}

// Ba??lant?? testi
void testUploadConnectivity(){
  if (!kVerboseLogging) return;
  if (apiUrl.length()==0 || WiFi.status()!=WL_CONNECTED) return;
  WiFiClient client; HTTPClient http;
  if (!http.begin(client, apiUrl)) { LOGV_LN("[TEST] begin fail"); return; }
  if (apiToken.length()) http.addHeader("Authorization", "Bearer " + apiToken);
  http.setTimeout(8000);
  int code = http.GET();
  LOGV("[TEST] GET %s => %d\n", apiUrl.c_str(), code);
  http.end();
}

// ==== Upload ==================================================================
bool uploadFrameToApi(const uint8_t* data, size_t len) {
  if (apiUrl.length() == 0) { lastHttpError = "No API URL"; lastHttpStatus = 0; return false; }
  if (WiFi.status() != WL_CONNECTED) { lastHttpError = "No WiFi"; lastHttpStatus = 0; return false; }

  WiFiClient client; HTTPClient http;
  if (!http.begin(client, apiUrl)) { lastHttpError = "http.begin()"; lastHttpStatus = 0; return false; }
  http.addHeader("Content-Type", "image/jpeg", true);
  http.addHeader("X-Device-ID", deviceId);
  http.addHeader("X-Frame-Size", lastUsedFrameSizeKey);
  http.addHeader("X-JPEG-Quality", String(jpegQuality));
  char fname[64]; snprintf(fname, sizeof(fname), "%s_%lu.jpg", deviceId.c_str(), (unsigned long)millis());
  http.addHeader("X-File-Name", fname);
  http.addHeader("X-Device-Time", String((unsigned long)time(nullptr)));
  if (apiToken.length()) http.addHeader("Authorization", "Bearer " + apiToken);
  http.setTimeout(15000);

  int code = http.POST((uint8_t*)data, len);
  lastHttpStatus = code;

  if (code <= 0) { lastHttpError = http.errorToString(code); http.end(); return false; }
  String payload = http.getString(); lastHttpError = payload; http.end();
  return (code >= 200 && code < 300);
}
camera_fb_t* safeGrab();
bool captureAndUploadOnce() {
  if (!cameraInited) return false;
  camera_fb_t * fb = safeGrab();
  if (!fb) { lastHttpError = "fb=null"; lastHttpStatus = 0; return false; }
  evaluateLowLightMetrics();
  bool ok = uploadFrameToApi(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return ok;
}

// ==== Setup / Loop ===========================================================
void setup() {
  Serial.begin(115200);
  delay(150);

  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set("cam_hal", ESP_LOG_NONE);
  esp_log_level_set("camera", ESP_LOG_NONE);

  deviceId = getDeviceIdHex();
  LOGV("[Boot] ID=%s, PSRAM=%s\n", deviceId.c_str(), psramFound()?"OK":"NO");

  loadPrefs();
  ensureWiFiOrPortal();

  if (!portalMode) {
    registerWithBackend();
    if (!initCamera()) LOGE_LN("[CAM] init failed (will retry)");
    fetchConfigFromBackend();
    testUploadConnectivity();

    if (autoUpload && cameraInited && WiFi.status()==WL_CONNECTED) {
      bool ok = captureAndUploadOnce();
      if (!ok) {
        LOGE("[Upload@boot] FAIL HTTP=%d info=%s\n", lastHttpStatus, lastHttpError.c_str());
      } else {
        LOGV("[Upload@boot] OK HTTP=%d info=%s\n", lastHttpStatus, lastHttpError.c_str());
      }
      lastUploadMs = millis();
    }
  } else {
    LOGV_LN("[Portal] WiFi + Backend portal active");
  }
}

void loop() {
  if (portalMode) { dnsServer.processNextRequest(); server.handleClient(); return; }

  static unsigned long lastCamRetry=0;
  if (!cameraInited && millis()-lastCamRetry>5000) { lastCamRetry = millis(); initCamera(); }

  if (WiFi.status()==WL_CONNECTED) {
    if (millis() - lastCfgPollMs >= cfgPollIntervalSec*1000UL) {
      lastCfgPollMs = millis();
      fetchConfigFromBackend();
    }
  }

  if (autoUpload && cameraInited && WiFi.status()==WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastUploadMs >= (uploadIntervalSec*1000UL)) {
      lastUploadMs = now;
      bool ok = captureAndUploadOnce();
      if (!ok) {
        LOGE("[Upload] FAIL HTTP=%d info=%s\n", lastHttpStatus, lastHttpError.c_str());
      } else {
        LOGV("[Upload] OK HTTP=%d info=%s\n", lastHttpStatus, lastHttpError.c_str());
      }
    }
  }

  delay(10);
}













