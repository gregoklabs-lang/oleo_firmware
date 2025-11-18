#pragma once

#include <Arduino.h>
#include <functional>

namespace SensorDiscovery
{
  using PublishCallback = std::function<bool(const char *topic, const char *payload)>;
  using ReadyCallback = std::function<bool(void)>;

  void begin(const String &deviceId, PublishCallback publishCb, ReadyCallback readyCb);
  void setDeviceId(const String &deviceId);
  void loop();
  void sendDiscoveryReport(bool force = false);
  void forceRescan();
} // namespace SensorDiscovery
