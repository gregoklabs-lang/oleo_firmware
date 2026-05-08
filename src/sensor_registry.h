#pragma once

#include <Arduino.h>

namespace SensorRegistry
{
String buildPayload(const String &deviceId, const String &eventKey);
String buildTopic(const String &deviceId);
} // namespace SensorRegistry
