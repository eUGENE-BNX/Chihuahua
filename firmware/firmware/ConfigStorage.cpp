#include "ConfigStorage.h"

#include "AppContext.h"
#include "CameraController.h"
#include "Logging.h"

namespace {
struct FsItem {
  const char* key;
  framesize_t val;
  const char* label;
};

FsItem kFrameSizeMap[] = {
  {"QQVGA",  FRAMESIZE_QQVGA,  "160x120 (QQVGA)"},
  {"QVGA",   FRAMESIZE_QVGA,   "320x240 (QVGA)"},
  {"CIF",    FRAMESIZE_CIF,    "400x296 (CIF)"},
  {"VGA",    FRAMESIZE_VGA,    "640x480 (VGA)"},
  {"SVGA",   FRAMESIZE_SVGA,   "800x600 (SVGA)"},
  {"XGA",    FRAMESIZE_XGA,    "1024x768 (XGA)"},
  {"SXGA",   FRAMESIZE_SXGA,   "1280x1024 (SXGA)"},
  {"UXGA",   FRAMESIZE_UXGA,   "1600x1200 (UXGA)"},
};
}

framesize_t framesizeFromKey(const String& key) {
  for (auto& item : kFrameSizeMap) {
    if (key.equalsIgnoreCase(item.key)) {
      return item.val;
    }
  }
  return FRAMESIZE_VGA;
}

const char* keyFromFramesize(framesize_t fs) {
  for (auto& item : kFrameSizeMap) {
    if (item.val == fs) {
      return item.key;
    }
  }
  return "VGA";
}


String defaultUploadUrl(const String& base) {
  if (base.isEmpty()) return "";
  if (base.endsWith("/")) return base + "upload";
  return base + "/upload";
}

const char* labelFromFramesize(framesize_t fs) {
  for (auto& item : kFrameSizeMap) {
    if (item.val == fs) {
      return item.label;
    }
  }
  return "640x480 (VGA)";
}

String getDeviceIdHex() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[13];
  sprintf(buf, "%012llX", mac);
  return String(buf);
}

void loadPrefs() {
  auto& ctx = app();
  auto& camera = ctx.camera;
  auto& tuning = camera.tuning;
  auto& target = camera.target;

  ctx.prefs.begin("cfg", true);
  ctx.network.ssid = ctx.prefs.getString("wifi_ssid", "");
  ctx.network.password = ctx.prefs.getString("wifi_pass", "");
  ctx.backend.baseUrl = ctx.prefs.getString("be_url", "");
  ctx.backend.token = ctx.prefs.getString("be_tok", "");
  ctx.upload.apiUrl = ctx.prefs.getString("api_url", "");
  ctx.upload.apiToken = ctx.prefs.getString("api_token", "");
  camera.jpegQuality = ctx.prefs.getInt("jpeg_q", 12);
  camera.frameSizeKey = ctx.prefs.getString("fs_key", "VGA");
  ctx.upload.autoUpload = ctx.prefs.getBool("auto_up", false);
  ctx.upload.intervalSec = ctx.prefs.getUInt("up_int", 10);
  ctx.backend.revision = ctx.prefs.getUInt("cfg_rev", 0);

  tuning.whitebal = ctx.prefs.getBool("awb", true);
  tuning.wbMode = ctx.prefs.getInt("wbm", 0);
  tuning.hmirror = ctx.prefs.getBool("hmr", false);
  tuning.vflip = ctx.prefs.getBool("vfl", false);
  tuning.brightness = ctx.prefs.getInt("bri", 0);
  tuning.contrast = ctx.prefs.getInt("con", 1);
  tuning.saturation = ctx.prefs.getInt("sat", 1);
  tuning.sharpness = ctx.prefs.getInt("shp", 1);
  tuning.awbGain = ctx.prefs.getBool("awg", true);
  tuning.gainCtrl = ctx.prefs.getBool("agc", true);
  tuning.exposureCtrl = ctx.prefs.getBool("aec", true);
  tuning.gainceilingIndex = static_cast<uint8_t>(ctx.prefs.getInt("gci", 4));
  tuning.aeLevel = ctx.prefs.getInt("ael", 0);
  tuning.lensCorr = ctx.prefs.getBool("lenc", true);
  tuning.rawGma = ctx.prefs.getBool("rgm", true);
  tuning.bpcEnabled = ctx.prefs.getBool("bpc", true);
  tuning.wpcEnabled = ctx.prefs.getBool("wpc", true);
  tuning.dcwEnabled = ctx.prefs.getBool("dcw", true);
  tuning.colorbarEnabled = ctx.prefs.getBool("clb", false);
  tuning.specialEffect = ctx.prefs.getInt("spe", 0);
  ctx.lowLight.boostEnabled = ctx.prefs.getBool("low_light", true);
  ctx.prefs.end();
  if (ctx.upload.apiUrl.isEmpty() && !ctx.backend.baseUrl.isEmpty()) {
    ctx.upload.apiUrl = defaultUploadUrl(ctx.backend.baseUrl);
  }

  camera.frameSize = framesizeFromKey(camera.frameSizeKey);
  camera.frameSizeTarget = camera.frameSize;
  camera.frameSizeKeyTarget = camera.frameSizeKey;

  if (camera.jpegQuality < 5) camera.jpegQuality = 5;
  if (camera.jpegQuality > 63) camera.jpegQuality = 63;
  camera.jpegQualityTarget = camera.jpegQuality;

  if (ctx.upload.intervalSec < 1) ctx.upload.intervalSec = 1;
  if (ctx.upload.intervalSec > 3600) ctx.upload.intervalSec = 3600;

  tuning.brightness = constrain(tuning.brightness, -2, 2);
  tuning.contrast   = constrain(tuning.contrast,   -2, 2);
  tuning.saturation = constrain(tuning.saturation, -2, 2);
  tuning.sharpness  = constrain(tuning.sharpness,  -2, 2);
  tuning.aeLevel    = constrain(tuning.aeLevel,    -2, 2);
  tuning.specialEffect = constrain(tuning.specialEffect, 0, 6);
  if (tuning.gainceilingIndex > 5) tuning.gainceilingIndex = 5;

  target = tuning;

  resetLowLightState();
}

void savePrefs() {
  auto& ctx = app();
  auto& camera = ctx.camera;
  auto& target = camera.target;

  ctx.prefs.begin("cfg", false);
  ctx.prefs.putString("wifi_ssid", ctx.network.ssid);
  ctx.prefs.putString("wifi_pass", ctx.network.password);
  ctx.prefs.putString("be_url", ctx.backend.baseUrl);
  ctx.prefs.putString("be_tok", ctx.backend.token);
  ctx.prefs.putString("api_url", ctx.upload.apiUrl);
  ctx.prefs.putString("api_token", ctx.upload.apiToken);
  ctx.prefs.putInt("jpeg_q", camera.jpegQualityTarget);
  ctx.prefs.putString("fs_key", camera.frameSizeKeyTarget);
  ctx.prefs.putBool("auto_up", ctx.upload.autoUpload);
  ctx.prefs.putUInt("up_int", ctx.upload.intervalSec);
  ctx.prefs.putUInt("cfg_rev", ctx.backend.revision);

  ctx.prefs.putBool("awb", target.whitebal);
  ctx.prefs.putInt("wbm", target.wbMode);
  ctx.prefs.putBool("hmr", target.hmirror);
  ctx.prefs.putBool("vfl", target.vflip);
  ctx.prefs.putInt("bri", target.brightness);
  ctx.prefs.putInt("con", target.contrast);
  ctx.prefs.putInt("sat", target.saturation);
  ctx.prefs.putInt("shp", target.sharpness);
  ctx.prefs.putBool("awg", target.awbGain);
  ctx.prefs.putBool("agc", target.gainCtrl);
  ctx.prefs.putBool("aec", target.exposureCtrl);
  ctx.prefs.putInt("gci", target.gainceilingIndex);
  ctx.prefs.putInt("ael", target.aeLevel);
  ctx.prefs.putBool("lenc", target.lensCorr);
  ctx.prefs.putBool("rgm", target.rawGma);
  ctx.prefs.putBool("bpc", target.bpcEnabled);
  ctx.prefs.putBool("wpc", target.wpcEnabled);
  ctx.prefs.putBool("dcw", target.dcwEnabled);
  ctx.prefs.putBool("clb", target.colorbarEnabled);
  ctx.prefs.putInt("spe", target.specialEffect);
  ctx.prefs.putBool("low_light", ctx.lowLight.boostEnabled);
  ctx.prefs.end();
}
