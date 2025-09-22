#pragma once

#include <Arduino.h>
#include "esp_camera.h"

framesize_t framesizeFromKey(const String& key);
const char* keyFromFramesize(framesize_t fs);
const char* labelFromFramesize(framesize_t fs);

void loadPrefs();
void savePrefs();

String defaultUploadUrl(const String& base);
String getDeviceIdHex();
