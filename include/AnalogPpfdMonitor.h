#pragma once

#include <Arduino.h>
#include <functional>

namespace PpfdAnalogMonitor
{
  struct Calibration
  {
    float r1Ohms = 3569.0f;
    float r2Ohms = 1100.0f;
    float sensorVoltageMax = 5.0f;
    float ppfdFullScale = 2500.0f;
    float calibrationFactor = 1.0f;
    uint8_t samplesPerReading = 10;
  };

  using TelemetryCallback = std::function<void(float value)>;

  void begin();
  void loop();

  void setCalibration(const Calibration &calibration);
  void setTelemetryCallback(TelemetryCallback callback);
  void enable(bool enabled);
  bool isEnabled();
} // namespace PpfdAnalogMonitor
