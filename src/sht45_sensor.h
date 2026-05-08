#pragma once

#include <Arduino.h>

namespace Sht45Sensor
{
struct Reading
{
  float temperatureC = 0.0f;
  float humidityRh = 0.0f;
  float vpdKpa = 0.0f;
  bool valid = false;
};

bool begin();
bool read(Reading &reading);
String buildTelemetryTopic(const String &deviceId);
String buildTelemetryPayload(const String &deviceId, const Reading &reading, uint32_t uptimeMs, const String &eventKey);
} // namespace Sht45Sensor
