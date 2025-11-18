#include "AnalogPpfdMonitor.h"

#include <Arduino.h>
#include <algorithm>
#include <driver/adc.h>
#include <utility>

namespace PpfdAnalogMonitor
{
  namespace
  {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    constexpr uint8_t kAdcPin = 4; // GPIO4 / ADC1_CH0
#else
    constexpr uint8_t kAdcPin = 36; // ADC1_CH0 on classic ESP32
#endif
    constexpr uint16_t kAdcResolutionBits = 12;
    constexpr float kAdcReferenceVoltage = 1.1f;
    constexpr float kAdcReferenceVoltageMv = kAdcReferenceVoltage * 1000.0f;
    constexpr uint16_t kAdcMaxValue = (1u << kAdcResolutionBits) - 1;
    constexpr unsigned long kSampleIntervalMs = 50;
    constexpr char kUnits[] = "umol/m2/s";

    Calibration g_calibration{};
    bool g_enabled = true;
    unsigned long g_lastSampleTickMs = 0;
    uint32_t g_mvAccumulator = 0;
    uint32_t g_rawAccumulator = 0;
    uint8_t g_samplesAccumulated = 0;
    TelemetryCallback g_telemetryCallback;

    constexpr adc_attenuation_t resolveAttenuation()
    {
#if defined(ADC_0db)
      return ADC_0db;
#elif defined(ADC_ATTEN_DB_0)
      return static_cast<adc_attenuation_t>(ADC_ATTEN_DB_0);
#elif defined(ADC_ATTEN_0db)
      return static_cast<adc_attenuation_t>(ADC_ATTEN_0db);
#else
      return static_cast<adc_attenuation_t>(0);
#endif
    }

    void configureAdc()
    {
      analogReadResolution(kAdcResolutionBits);
      const adc_attenuation_t pinAttenuation = resolveAttenuation();
      analogSetPinAttenuation(kAdcPin, pinAttenuation);
    }

    void resetAveraging()
    {
      g_mvAccumulator = 0;
      g_rawAccumulator = 0;
      g_samplesAccumulated = 0;
    }

    uint16_t estimateRawFromMillivolts(uint32_t millivolts)
    {
      const float normalized = static_cast<float>(millivolts) / kAdcReferenceVoltageMv;
      const float scaled = normalized * static_cast<float>(kAdcMaxValue);
      const float clamped = std::max(0.0f, std::min(static_cast<float>(kAdcMaxValue), scaled));
      return static_cast<uint16_t>(clamped);
    }

    float computeSensorVoltage(float adcVoltage)
    {
      const float dividerRatio = (g_calibration.r1Ohms + g_calibration.r2Ohms) / g_calibration.r2Ohms;
      return adcVoltage * dividerRatio * g_calibration.calibrationFactor;
    }

    float computePpfd(float sensorVoltage)
    {
      const float normalized = sensorVoltage / g_calibration.sensorVoltageMax;
      return normalized * g_calibration.ppfdFullScale;
    }

    void printReading(uint16_t raw, float vAdc, float vSensor, float ppfd)
    {
      Serial.printf("RAW=%04u  Vadc=%.3fV  Vsensor=%.3fV  PPFD=%04.0f %s\n",
                    raw,
                    vAdc,
                    vSensor,
                    ppfd,
                    kUnits);
    }
  } // namespace

  void begin()
  {
    pinMode(kAdcPin, INPUT);
    configureAdc();
    resetAveraging();
    g_lastSampleTickMs = 0;
  }

  void loop()
  {
    if (!g_enabled)
    {
      return;
    }

    const unsigned long now = millis();
    if (now - g_lastSampleTickMs < kSampleIntervalMs)
    {
      return;
    }
    g_lastSampleTickMs = now;

    const uint32_t millivolts = analogReadMilliVolts(kAdcPin);
    g_mvAccumulator += millivolts;
    g_rawAccumulator += estimateRawFromMillivolts(millivolts);
    ++g_samplesAccumulated;

    const uint8_t requiredSamples = std::max<uint8_t>(1, g_calibration.samplesPerReading);
    if (g_samplesAccumulated < requiredSamples)
    {
      return;
    }

    const float avgMillivolts = static_cast<float>(g_mvAccumulator) / g_samplesAccumulated;
    const uint16_t raw = static_cast<uint16_t>(static_cast<float>(g_rawAccumulator) / g_samplesAccumulated);
    resetAveraging();

    const float vAdc = avgMillivolts / 1000.0f;
    const float vSensor = computeSensorVoltage(vAdc);
    const float ppfd = computePpfd(vSensor);

    printReading(raw, vAdc, vSensor, ppfd);
    if (g_telemetryCallback)
    {
      g_telemetryCallback(ppfd);
    }
  }

  void setCalibration(const Calibration &calibration)
  {
    g_calibration = calibration;
    if (g_calibration.samplesPerReading == 0)
    {
      g_calibration.samplesPerReading = 1;
    }
    resetAveraging();
  }

  void setTelemetryCallback(TelemetryCallback callback)
  {
    g_telemetryCallback = std::move(callback);
  }

  void enable(bool enabled)
  {
    g_enabled = enabled;
  }

  bool isEnabled()
  {
    return g_enabled;
  }
} // namespace PpfdAnalogMonitor
