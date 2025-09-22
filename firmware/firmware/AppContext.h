#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "esp_camera.h"

struct SensorTuning {
  bool whitebal = true;
  int wbMode = 0;
  bool hmirror = false;
  bool vflip = false;
  int brightness = 0;
  int contrast = 1;
  int saturation = 1;
  int sharpness = 1;
  bool awbGain = true;
  bool gainCtrl = true;
  bool exposureCtrl = true;
  uint8_t gainceilingIndex = 4;
  int aeLevel = 0;
  bool lensCorr = true;
  bool rawGma = true;
  bool bpcEnabled = true;
  bool wpcEnabled = true;
  bool dcwEnabled = true;
  bool colorbarEnabled = false;
  int specialEffect = 0;
};

struct CameraState {
  framesize_t frameSize = FRAMESIZE_VGA;
  framesize_t frameSizeTarget = FRAMESIZE_VGA;
  String frameSizeKey = "VGA";
  String frameSizeKeyTarget = "VGA";
  int jpegQuality = 12;
  int jpegQualityTarget = 12;
  String lastUsedFrameSizeKey = "VGA";
  bool inited = false;
  int currentXclkHz = 20000000;
  uint8_t failedGrabStreak = 0;
  unsigned long lastReinitMs = 0;
  SensorTuning tuning{};
  SensorTuning target{};
};

struct NetworkState {
  String ssid;
  String password;
  bool portalMode = false;
};

struct BackendState {
  String baseUrl;
  String token;
  uint32_t revision = 0;
  unsigned long lastConfigPollMs = 0;
  uint32_t pollIntervalSec = 5;
};

struct UploadState {
  String apiUrl;
  String apiToken;
  bool autoUpload = false;
  uint32_t intervalSec = 10;
  unsigned long lastUploadMs = 0;
};

struct LowLightState {
  bool boostEnabled = true;
  bool active = false;
  uint8_t score = 0;
  unsigned long lastLogMs = 0;
};

struct HttpState {
  int lastStatus = 0;
  String lastError = "-";
};

struct DeviceInfo {
  String id;
};

struct FirmwareState {
  WebServer server{80};
  DNSServer dnsServer;
  Preferences prefs;
  NetworkState network;
  BackendState backend;
  UploadState upload;
  CameraState camera;
  LowLightState lowLight;
  HttpState http;
  DeviceInfo device;
};

FirmwareState& app();