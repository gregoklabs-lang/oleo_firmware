#include "sht45_sensor.h"

#include <Adafruit_SHT4x.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>

#ifndef TOPIC_BASE
#define TOPIC_BASE "ERROR_TOPIC/"
#endif

namespace Sht45Sensor
{
namespace
{
constexpr const char kSensorKey[] = "ambient_1";
constexpr uint8_t kI2cSdaPin = 8;
constexpr uint8_t kI2cSclPin = 9;
constexpr uint8_t kAltI2cSdaPin = 5;
constexpr uint8_t kAltI2cSclPin = 6;
Adafruit_SHT4x g_sht4;
bool g_initialized = false;
bool g_available = false;

void scanI2cBus(uint8_t sdaPin, uint8_t sclPin)
{
  bool foundDevice = false;
  Wire.begin(sdaPin, sclPin);
  Serial.printf("[SHT45] Escaneando I2C SDA=%u SCL=%u\n", sdaPin, sclPin);
  for (uint8_t address = 1; address < 127; ++address)
  {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();
    if (error == 0)
    {
      Serial.printf("[SHT45] I2C detectado en 0x%02X\n", address);
      foundDevice = true;
    }
  }

  if (!foundDevice)
  {
    Serial.println("[SHT45] No se detectaron dispositivos I2C");
  }
}

float saturationVpKpa(float temperatureC)
{
  return 0.6108f * expf((17.27f * temperatureC) / (temperatureC + 237.3f));
}

float calculateVpdKpa(float temperatureC, float humidityRh)
{
  const float svp = saturationVpKpa(temperatureC);
  const float avp = svp * (humidityRh / 100.0f);
  const float vpd = svp - avp;
  return vpd < 0.0f ? 0.0f : vpd;
}

float roundToTwoDecimals(float value)
{
  return roundf(value * 100.0f) / 100.0f;
}
} // namespace

bool begin()
{
  if (g_initialized)
  {
    return g_available;
  }

  g_initialized = true;
  scanI2cBus(kI2cSdaPin, kI2cSclPin);
  scanI2cBus(kAltI2cSdaPin, kAltI2cSclPin);
  Wire.begin(kI2cSdaPin, kI2cSclPin);
  g_available = g_sht4.begin();
  if (!g_available)
  {
    Serial.println("[SHT45] begin() fallo");
    return false;
  }

  Serial.printf("[SHT45] Sensor detectado, serial=0x%lX\n", static_cast<unsigned long>(g_sht4.readSerial()));
  g_sht4.setPrecision(SHT4X_HIGH_PRECISION);
  g_sht4.setHeater(SHT4X_NO_HEATER);
  return true;
}

bool read(Reading &reading)
{
  if (!begin())
  {
    reading.valid = false;
    return false;
  }

  sensors_event_t humidity;
  sensors_event_t temp;
  g_sht4.getEvent(&humidity, &temp);

  reading.temperatureC = roundToTwoDecimals(temp.temperature);
  reading.humidityRh = roundToTwoDecimals(humidity.relative_humidity);
  reading.vpdKpa = roundToTwoDecimals(calculateVpdKpa(reading.temperatureC, reading.humidityRh));
  reading.valid = true;
  return true;
}

String buildTelemetryTopic(const String &deviceId)
{
  return String(TOPIC_BASE) + deviceId + "/telemetry";
}

String buildTelemetryPayload(const String &deviceId, const Reading &reading, uint32_t uptimeMs, const String &eventKey)
{
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["sensor_key"] = kSensorKey;
  doc["temperature_c"] = reading.temperatureC;
  doc["humidity_rh"] = reading.humidityRh;
  doc["vpd_kpa"] = reading.vpdKpa;
  doc["uptime_ms"] = uptimeMs;
  doc["event_key"] = eventKey;

  String payload;
  serializeJson(doc, payload);
  return payload;
}
} // namespace Sht45Sensor
