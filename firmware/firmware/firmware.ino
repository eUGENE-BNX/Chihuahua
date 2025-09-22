#include <WiFi.h>
#include "esp_camera.h"
#include "esp_log.h"

#include "AppContext.h"
#include "BackendClient.h"
#include "CameraController.h"
#include "ConfigStorage.h"
#include "Logging.h"
#include "NetworkManager.h"

void setup() {
  Serial.begin(115200);
  delay(150);

  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set("cam_hal", ESP_LOG_NONE);
  esp_log_level_set("camera", ESP_LOG_NONE);

  auto& ctx = app();
  ctx.device.id = getDeviceIdHex();
  LOGV("[Boot] ID=%s, PSRAM=%s\n", ctx.device.id.c_str(), psramFound() ? "OK" : "NO");

  loadPrefs();
  ensureWiFiOrPortal();

  if (!ctx.network.portalMode) {
    registerWithBackend();
    if (!initCamera()) {
      LOGE_LN("[CAM] init failed (will retry)");
    }
    fetchConfigFromBackend();
    testUploadConnectivity();

    if (ctx.upload.autoUpload && ctx.camera.inited && WiFi.status() == WL_CONNECTED) {
      bool ok = captureAndUploadOnce();
      if (!ok) {
        LOGE("[Upload@boot] FAIL HTTP=%d info=%s\n", ctx.http.lastStatus, ctx.http.lastError.c_str());
      } else {
        LOGV("[Upload@boot] OK HTTP=%d info=%s\n", ctx.http.lastStatus, ctx.http.lastError.c_str());
      }
      ctx.upload.lastUploadMs = millis();
    }
  } else {
    LOGV_LN("[Portal] WiFi + Backend portal active");
  }
}

void loop() {
  auto& ctx = app();
  if (ctx.network.portalMode) {
    ctx.dnsServer.processNextRequest();
    ctx.server.handleClient();
    return;
  }

  static unsigned long lastCamRetry = 0;
  if (!ctx.camera.inited && millis() - lastCamRetry > 5000) {
    lastCamRetry = millis();
    initCamera();
  }

  if (WiFi.status() == WL_CONNECTED) {
    unsigned long now = millis();
    if (now - ctx.backend.lastConfigPollMs >= ctx.backend.pollIntervalSec * 1000UL) {
      ctx.backend.lastConfigPollMs = now;
      fetchConfigFromBackend();
    }
  }

  if (ctx.upload.autoUpload && ctx.camera.inited && WiFi.status() == WL_CONNECTED) {
    unsigned long now = millis();
    if (now - ctx.upload.lastUploadMs >= ctx.upload.intervalSec * 1000UL) {
      ctx.upload.lastUploadMs = now;
      bool ok = captureAndUploadOnce();
      if (!ok) {
        LOGE("[Upload] FAIL HTTP=%d info=%s\n", ctx.http.lastStatus, ctx.http.lastError.c_str());
      } else {
        LOGV("[Upload] OK HTTP=%d info=%s\n", ctx.http.lastStatus, ctx.http.lastError.c_str());
      }
    }
  }

  delay(10);
}