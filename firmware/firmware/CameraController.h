#pragma once

#include "esp_camera.h"

void refreshLowLightProfile();
void resetLowLightState();
void evaluateLowLightMetrics();
bool initCamera();
void applyConfigIfNeeded();
camera_fb_t* safeGrab();