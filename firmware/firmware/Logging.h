#pragma once

#include <Arduino.h>

constexpr bool kVerboseLogging = false;

#define LOGV(fmt, ...) do { if (kVerboseLogging) Serial.printf((fmt), ##__VA_ARGS__); } while (0)
#define LOGV_LN(msg) do { if (kVerboseLogging) Serial.println((msg)); } while (0)
#define LOGE(fmt, ...) Serial.printf((fmt), ##__VA_ARGS__)
#define LOGE_LN(msg) Serial.println((msg))