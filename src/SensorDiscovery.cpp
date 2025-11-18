#include "SensorDiscovery.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cstdarg>
#include <ctime>
#include <vector>

#include <driver/adc.h>

#include "SensorId.h"

namespace SensorDiscovery
{
  namespace
  {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    constexpr uint8_t kPpfdAdcPin = 4; // GPIO4 is ADC capable on ESP32-C3
#else
    constexpr uint8_t kPpfdAdcPin = 36; // GPIO36 / ADC1_CH0 on ESP32
#endif
    constexpr const char *kPpfdAddress = "A1"; // Human readable ADC slot
    constexpr float kAdcDetectVoltage = 0.10f;
    constexpr float kAdcRemovalVoltage = 0.02f;
    constexpr uint8_t kRequiredConsecutiveSamples = 3;
    constexpr uint32_t kAdcSampleIntervalMs = 1000;

    struct SensorDescriptor
    {
      String sensorId;
      String type;
      String busType;
      String address;
      String unit;
      uint8_t pin = 0;
      bool isActive = false;
      time_t lastSeenEpoch = 0;
      uint64_t lastSeenMillis = 0;
    };

    String g_deviceId;
    PublishCallback g_publishCb;
    ReadyCallback g_readyCb;
    std::vector<SensorDescriptor> g_sensors;
    bool g_pendingReport = false;
    unsigned long g_lastDiscoveryAttemptMs = 0;
    constexpr unsigned long kDiscoveryMinPublishIntervalMs = 5000;
    unsigned long g_lastAdcSampleMs = 0;
    uint8_t g_highSamples = 0;
    uint8_t g_lowSamples = 0;
    bool g_adcPresent = false;

    void log(const char *fmt, ...)
    {
      char buffer[160] = {0};
      va_list args;
      va_start(args, fmt);
      vsnprintf(buffer, sizeof(buffer), fmt, args);
      va_end(args);
      Serial.print("[DISCOVERY] ");
      Serial.print(buffer);
    }

    void markTimestamp(SensorDescriptor &sensor)
    {
      sensor.lastSeenEpoch = time(nullptr);
      sensor.lastSeenMillis = millis();
    }

    String buildSensorId(const String &busType, const String &address)
    {
      if (g_deviceId.isEmpty())
      {
        return "SNR_UNKNOWN_" + busType + "_" + address;
      }
      return makeSensorId(g_deviceId, busType, address);
    }

    SensorDescriptor *findSensor(const String &sensorId)
    {
      for (auto &sensor : g_sensors)
      {
        if (sensor.sensorId == sensorId)
        {
          return &sensor;
        }
      }
      return nullptr;
    }

    void ensureSensor(const String &busType,
                      const String &address,
                      const String &type,
                      uint8_t pin,
                      const String &unit,
                      bool isActive)
    {
      const String sensorId = buildSensorId(busType, address);
      SensorDescriptor *sensor = findSensor(sensorId);
      if (!sensor)
      {
        g_sensors.push_back({});
        sensor = &g_sensors.back();
        sensor->sensorId = sensorId;
        sensor->type = type;
        sensor->busType = busType;
        sensor->address = address;
        sensor->unit = unit;
        sensor->pin = pin;
      }
      sensor->isActive = isActive;
      markTimestamp(*sensor);
      g_pendingReport = true;
    }

    String formatIso8601(const SensorDescriptor &sensor)
    {
      if (sensor.lastSeenEpoch > 0)
      {
        struct tm timeinfo;
#if defined(ESP_PLATFORM)
        gmtime_r(&sensor.lastSeenEpoch, &timeinfo);
#else
        time_t temp = sensor.lastSeenEpoch;
        timeinfo = *gmtime(&temp);
#endif
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        return String(buffer);
      }

      char fallback[32];
      const uint64_t seconds = sensor.lastSeenMillis / 1000ULL;
      const unsigned long long secs = static_cast<unsigned long long>(seconds % 60ULL);
      snprintf(fallback, sizeof(fallback), "1970-01-01T00:00:%02lluZ", secs);
      return String(fallback);
    }

    void handleAdcVoltage(float voltage)
    {
      if (voltage >= kAdcDetectVoltage)
      {
        g_highSamples = std::min<uint8_t>(kRequiredConsecutiveSamples, g_highSamples + 1);
        g_lowSamples = 0;
      }
      else if (voltage <= kAdcRemovalVoltage)
      {
        g_lowSamples = std::min<uint8_t>(kRequiredConsecutiveSamples, g_lowSamples + 1);
        g_highSamples = 0;
      }
      else
      {
        g_highSamples = 0;
        g_lowSamples = 0;
      }

      if (!g_adcPresent && g_highSamples >= kRequiredConsecutiveSamples)
      {
        g_adcPresent = true;
        ensureSensor("ADC", kPpfdAddress, "ppfd", kPpfdAdcPin, "µmol/m²/s", true);
        log("ADC PPFD detected on %s (%.3f V)\n", kPpfdAddress, voltage);
      }
      else if (g_adcPresent && g_lowSamples >= kRequiredConsecutiveSamples)
      {
        g_adcPresent = false;
        ensureSensor("ADC", kPpfdAddress, "ppfd", kPpfdAdcPin, "µmol/m²/s", false);
        log("ADC PPFD removed from %s\n", kPpfdAddress);
      }
    }

    void scanAdcBus()
    {
      const unsigned long now = millis();
      if (now - g_lastAdcSampleMs < kAdcSampleIntervalMs)
      {
        return;
      }
      g_lastAdcSampleMs = now;

      const int raw = analogRead(kPpfdAdcPin);
      const float voltage = (static_cast<float>(raw) / 4095.0f) * 3.3f;
      handleAdcVoltage(voltage);
    }

    void scanI2cBus()
    {
      // Hook for future I2C detection logic
    }

    void scanSdi12Bus()
    {
      // Hook for future SDI-12 detection logic
    }

    size_t jsonCapacity()
    {
      constexpr size_t base = 512;
      constexpr size_t perSensor = 256;
      return base + (g_sensors.size() * perSensor);
    }
  } // namespace

  void begin(const String &deviceId, PublishCallback publishCb, ReadyCallback readyCb)
  {
    g_deviceId = deviceId;
    g_publishCb = publishCb;
    g_readyCb = readyCb;

    pinMode(kPpfdAdcPin, INPUT);
#if defined(ADC_ATTEN_DB_11)
    analogSetPinAttenuation(kPpfdAdcPin, static_cast<adc_attenuation_t>(ADC_ATTEN_DB_11));
#elif defined(ADC_ATTEN_11db)
    analogSetPinAttenuation(kPpfdAdcPin, static_cast<adc_attenuation_t>(ADC_ATTEN_11db));
#endif
    analogReadResolution(12);

    forceRescan();
  }

  void setDeviceId(const String &deviceId)
  {
    if (g_deviceId == deviceId)
    {
      return;
    }
    g_deviceId = deviceId;
    for (auto &sensor : g_sensors)
    {
      sensor.sensorId = buildSensorId(sensor.busType, sensor.address);
    }
    g_pendingReport = true;
    g_lastDiscoveryAttemptMs = 0;
  }

  void loop()
  {
    scanAdcBus();
    scanI2cBus();
    scanSdi12Bus();

    const unsigned long now = millis();
    if (g_pendingReport && (now - g_lastDiscoveryAttemptMs >= kDiscoveryMinPublishIntervalMs))
    {
      sendDiscoveryReport();
    }
  }

  void sendDiscoveryReport(bool force)
  {
    (void)force;
    if (!g_publishCb)
    {
      return;
    }
    g_lastDiscoveryAttemptMs = millis();
    if (g_readyCb && !g_readyCb())
    {
      g_pendingReport = true;
      return;
    }

    JsonDocument doc;
    doc["device_id"] = g_deviceId;
    JsonArray sensorsArray = doc["sensors"].to<JsonArray>();
    for (const auto &sensor : g_sensors)
    {
      JsonObject sensorObj = sensorsArray.add<JsonObject>();
      sensorObj["sensor_id"] = sensor.sensorId;
      sensorObj["type"] = sensor.type;
      sensorObj["bus_type"] = sensor.busType;
      sensorObj["address"] = sensor.address;
      sensorObj["is_active"] = sensor.isActive;
      sensorObj["last_seen"] = formatIso8601(sensor);
      JsonObject metadata = sensorObj["metadata"].to<JsonObject>();
      metadata["unit"] = sensor.unit;
      metadata["pin"] = sensor.pin;
    }

    String payload;
    const size_t bytes = serializeJson(doc, payload);
    if (bytes == 0)
    {
      log("Discovery report serialization failed\n");
      g_pendingReport = true;
      return;
    }
    const String topic = "lab/devices/" + g_deviceId + "/discovery";
    const bool published = g_publishCb(topic.c_str(), payload.c_str());
    if (published)
    {
      g_pendingReport = false;
      log("Discovery report sent (%u sensors)\n", static_cast<unsigned>(sensorsArray.size()));
    }
    else
    {
      g_pendingReport = true;
      log("Discovery report publish failed (MQTT unavailable)\n");
    }
  }

  void forceRescan()
  {
    g_lastAdcSampleMs = 0;
    g_highSamples = 0;
    g_lowSamples = 0;
    g_adcPresent = false;
    g_pendingReport = true;
    g_lastDiscoveryAttemptMs = 0;
  }
} // namespace SensorDiscovery
