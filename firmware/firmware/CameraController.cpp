#include "CameraController.h"

#include <Arduino.h>
#include "esp_camera.h"

#include "AppContext.h"
#include "ConfigStorage.h"
#include "Logging.h"

namespace {
void EvaluateLowLightMetricsInternal();
void RefreshLowLightProfileInternal();
void ResetLowLightStateInternal();
constexpr uint16_t kLowLightAecActivate = 850;
constexpr uint16_t kLowLightAecRelease = 700;
constexpr uint8_t kLowLightGainActivate = 24;
constexpr uint8_t kLowLightGainRelease = 18;
constexpr uint8_t kLowLightScoreThreshold = 3;
constexpr uint8_t kLowLightScoreMax = 6;
constexpr unsigned long kLowLightLogIntervalMs = 10000UL;

constexpr int PWDN_GPIO_NUM = 32;
constexpr int RESET_GPIO_NUM = -1;
constexpr int XCLK_GPIO_NUM = 0;
constexpr int SIOD_GPIO_NUM = 26;
constexpr int SIOC_GPIO_NUM = 27;
constexpr int Y9_GPIO_NUM = 35;
constexpr int Y8_GPIO_NUM = 34;
constexpr int Y7_GPIO_NUM = 39;
constexpr int Y6_GPIO_NUM = 36;
constexpr int Y5_GPIO_NUM = 21;
constexpr int Y4_GPIO_NUM = 19;
constexpr int Y3_GPIO_NUM = 18;
constexpr int Y2_GPIO_NUM = 5;
constexpr int VSYNC_GPIO_NUM = 25;
constexpr int HREF_GPIO_NUM = 23;
constexpr int PCLK_GPIO_NUM = 22;

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
  if (idx >= static_cast<int>(sizeof(kMap) / sizeof(kMap[0]))) {
    idx = static_cast<int>(sizeof(kMap) / sizeof(kMap[0])) - 1;
  }
  return kMap[idx];
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

  auto& target = app().camera.target;
  int boostAe = constrain(target.aeLevel, 0, 2);
  s->set_ae_level(s, boostAe);
  s->set_dcw(s, 1);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
}

void applyManualSensorParams(sensor_t* s) {
  if (!s) return;
  auto& ctx = app();
  auto& camera = ctx.camera;
  auto& target = camera.target;
  auto& active = camera.tuning;

  int wb  = constrain(target.wbMode, 0, 4);
  int bri = constrain(target.brightness, -2, 2);
  int con = constrain(target.contrast,   -2, 2);
  int sat = constrain(target.saturation, -2, 2);
  int shp = constrain(target.sharpness,  -2, 2);
  int ae  = constrain(target.aeLevel,    -2, 2);
  int spe = constrain(target.specialEffect, 0, 6);
  uint8_t gcIdx = target.gainceilingIndex;
  if (gcIdx > 5) gcIdx = 5;

  s->set_whitebal(s, target.whitebal ? 1 : 0);
  s->set_wb_mode(s, wb);
  s->set_hmirror(s, target.hmirror ? 1 : 0);
  s->set_vflip(s,  target.vflip  ? 1 : 0);
  s->set_brightness(s, bri);
  s->set_contrast(s,   con);
  s->set_saturation(s, sat);
  s->set_sharpness(s,  shp);
  s->set_awb_gain(s, target.awbGain ? 1 : 0);
  s->set_gain_ctrl(s, target.gainCtrl ? 1 : 0);
  s->set_exposure_ctrl(s, target.exposureCtrl ? 1 : 0);
  s->set_gainceiling(s, gainceilingFromIndex(gcIdx));
  s->set_ae_level(s, ae);
  s->set_lenc(s, target.lensCorr ? 1 : 0);
  s->set_raw_gma(s, target.rawGma ? 1 : 0);
  s->set_bpc(s, target.bpcEnabled ? 1 : 0);
  s->set_wpc(s, target.wpcEnabled ? 1 : 0);
  s->set_dcw(s, target.dcwEnabled ? 1 : 0);
  s->set_colorbar(s, target.colorbarEnabled ? 1 : 0);
  s->set_special_effect(s, spe);

  active.whitebal = target.whitebal;
  active.wbMode = wb;
  active.hmirror = target.hmirror;
  active.vflip = target.vflip;
  active.brightness = bri;
  active.contrast = con;
  active.saturation = sat;
  active.sharpness = shp;
  active.awbGain = target.awbGain;
  active.gainCtrl = target.gainCtrl;
  active.exposureCtrl = target.exposureCtrl;
  active.gainceilingIndex = gcIdx;
  active.aeLevel = ae;
  active.lensCorr = target.lensCorr;
  active.rawGma = target.rawGma;
  active.bpcEnabled = target.bpcEnabled;
  active.wpcEnabled = target.wpcEnabled;
  active.dcwEnabled = target.dcwEnabled;
  active.colorbarEnabled = target.colorbarEnabled;
  active.specialEffect = spe;
}

void updateLowLightObserver(uint16_t aecValue, uint8_t agcGain) {
  auto& ctx = app();
  auto& lowLight = ctx.lowLight;
  if (!lowLight.boostEnabled) return;

  bool requestBoost = (aecValue >= kLowLightAecActivate) || (agcGain >= kLowLightGainActivate);
  bool requestRelease = (aecValue <= kLowLightAecRelease) && (agcGain <= kLowLightGainRelease);

  if (requestBoost) {
    if (lowLight.score < kLowLightScoreMax) lowLight.score++;
  } else if (requestRelease) {
    if (lowLight.score > 0) lowLight.score--;
  } else if (lowLight.score > 0) {
    lowLight.score--;
  }

  bool newActive = (lowLight.score >= kLowLightScoreThreshold);
  if (newActive != lowLight.active) {
    lowLight.active = newActive;
    RefreshLowLightProfileInternal();
    LOGV("[LowLight] %s (aec=%u gain=%u score=%u)\n", newActive ? "enabled" : "disabled", aecValue, agcGain, lowLight.score);
    lowLight.lastLogMs = millis();
  } else if (kVerboseLogging) {
    unsigned long now = millis();
    if (now - lowLight.lastLogMs > kLowLightLogIntervalMs) {
      LOGV("[LowLight] state=%d aec=%u gain=%u score=%u\n", newActive ? 1 : 0, aecValue, agcGain, lowLight.score);
      lowLight.lastLogMs = now;
    }
  }
}

void EvaluateLowLightMetricsInternal() {
  auto& ctx = app();
  if (!ctx.lowLight.boostEnabled || !ctx.camera.inited) return;
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  updateLowLightObserver(s->status.aec_value, s->status.agc_gain);
}

void applyAdvancedParams() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  applyManualSensorParams(s);
  auto& ctx = app();
  if (ctx.lowLight.boostEnabled && ctx.lowLight.active) {
    applyLowLightProfile(s, true);
  }
}

void RefreshLowLightProfileInternal() {
  auto& ctx = app();
  if (!ctx.camera.inited) return;
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  applyManualSensorParams(s);
  applyLowLightProfile(s, ctx.lowLight.boostEnabled && ctx.lowLight.active);
}

void ResetLowLightStateInternal() {
  auto& ctx = app();
  ctx.lowLight.score = 0;
  ctx.lowLight.active = false;
  RefreshLowLightProfileInternal();
}

bool initCameraWithXclk(int xclkHz) {
  auto& ctx = app();
  auto& camera = ctx.camera;
  camera.currentXclkHz = xclkHz;

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

  config.frame_size   = camera.frameSize;
  config.jpeg_quality = camera.jpegQuality;
  config.fb_count     = 2;
  config.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    LOGE("[CAM] init err=0x%x @%dHz\n", err, xclkHz);
    camera.inited = false;
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, camera.frameSize);
    s->set_quality(s, camera.jpegQuality);
  }
  camera.inited = true;
  LOGV("[CAM] init ok @%dHz, %s, q=%d\n", xclkHz, labelFromFramesize(camera.frameSize), camera.jpegQuality);
  applyAdvancedParams();
  RefreshLowLightProfileInternal();
  return true;
}

void maybeReinitLowerXclk() {
  auto& ctx = app();
  auto& camera = ctx.camera;
  int next = camera.currentXclkHz;
  if (camera.currentXclkHz > 16500000) next = 16500000;
  else if (camera.currentXclkHz > 10000000) next = 10000000;
  if (next != camera.currentXclkHz) {
    LOGV("[CAM] re-init lower XCLK: %d -> %d\n", camera.currentXclkHz, next);
    esp_camera_deinit();
    camera.inited = false;
    delay(200);
    initCameraWithXclk(next);
  }
}

}  // namespace

void evaluateLowLightMetrics() {
  EvaluateLowLightMetricsInternal();
}


void refreshLowLightProfile() {
  RefreshLowLightProfileInternal();
}

void resetLowLightState() {
  ResetLowLightStateInternal();
}

bool initCamera() {
  if (initCameraWithXclk(20000000)) return true;
  delay(200);
  if (initCameraWithXclk(16500000)) return true;
  delay(200);
  if (initCameraWithXclk(10000000)) return true;
  return false;
}

void applyConfigIfNeeded() {
  auto& ctx = app();
  auto& camera = ctx.camera;
  if (!camera.inited) return;

  bool needReinit = (camera.frameSizeTarget != camera.frameSize);
  if (needReinit) {
    LOGV("[CFG] reinit FS: %s -> %s\n", labelFromFramesize(camera.frameSize), labelFromFramesize(camera.frameSizeTarget));
    esp_camera_deinit();
    camera.inited = false;
    camera.frameSize = camera.frameSizeTarget;
    camera.frameSizeKey = camera.frameSizeKeyTarget;
    camera.jpegQuality = camera.jpegQualityTarget;
    delay(150);
    if (!initCamera()) {
      LOGE_LN("[CFG] reinit failed");
      return;
    }
  } else if (camera.jpegQualityTarget != camera.jpegQuality) {
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
      s->set_quality(s, camera.jpegQualityTarget);
      camera.jpegQuality = camera.jpegQualityTarget;
      LOGV("[CFG] set q=%d\n", camera.jpegQuality);
    }
  }

  applyAdvancedParams();
  if (!ctx.lowLight.boostEnabled) {
    ResetLowLightStateInternal();
  } else {
    RefreshLowLightProfileInternal();
  }
}

camera_fb_t* safeGrab() {
  auto& ctx = app();
  auto& camera = ctx.camera;

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) {
    camera.failedGrabStreak = 0;
    camera.lastUsedFrameSizeKey = keyFromFramesize(camera.frameSize);
    return fb;
  }

  camera.failedGrabStreak++;
  if (camera.failedGrabStreak == 3) {
    LOGE("[CAM] fb_get failed (streak=%u)\n", camera.failedGrabStreak);
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    framesize_t wanted = camera.frameSize;
    framesize_t fallback[] = {FRAMESIZE_SVGA, FRAMESIZE_VGA, FRAMESIZE_QVGA};
    for (auto fs : fallback) {
      if (fs >= wanted) continue;
      s->set_framesize(s, fs);
      camera_fb_t* fb2 = esp_camera_fb_get();
      if (fb2) {
        camera.lastUsedFrameSizeKey = keyFromFramesize(fs);
        s->set_framesize(s, wanted);
        camera.failedGrabStreak = 0;
        return fb2;
      }
    }
    s->set_framesize(s, wanted);
  }

  unsigned long now = millis();
  if (camera.failedGrabStreak >= 3 && now - camera.lastReinitMs > 7000) {
    camera.lastReinitMs = now;
    maybeReinitLowerXclk();
  }
  return nullptr;
}
