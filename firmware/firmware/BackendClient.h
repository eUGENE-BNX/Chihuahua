#pragma once

#include <Arduino.h>

void registerWithBackend();
bool fetchConfigFromBackend();
void testUploadConnectivity();
bool uploadFrameToApi(const uint8_t* data, size_t len);
bool captureAndUploadOnce();