#include "sensor_registry.h"

#include <ArduinoJson.h>

#ifndef TOPIC_BASE
#define TOPIC_BASE "ERROR_TOPIC/"
#endif

namespace SensorRegistry
{
namespace
{
constexpr const char kSensorKey[] = "ambient_1";
constexpr const char kSensorType[] = "sht45";
constexpr const char kVendor[] = "adafruit";
constexpr const char kModel[] = "SHT45";
constexpr const char kI2cAddress[] = "0x44";
constexpr const char kBus[] = "i2c0";
constexpr const char kPosition[] = "canopy";
constexpr const char kMetadataSource[] = "firmware";
} // namespace

String buildTopic(const String &deviceId)
{
  return String(TOPIC_BASE) + deviceId + "/sensor_registry";
}

String buildPayload(const String &deviceId, const String &eventKey)
{
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["sensor_key"] = kSensorKey;
  doc["sensor_type"] = kSensorType;
  doc["vendor"] = kVendor;
  doc["model"] = kModel;
  doc["i2c_address"] = kI2cAddress;
  doc["bus"] = kBus;
  doc["position"] = kPosition;
  doc["is_active"] = true;

  JsonArray measures = doc["measures"].to<JsonArray>();
  measures.add("temperature_c");
  measures.add("humidity_rh");
  measures.add("vpd_kpa");

  JsonObject unitMap = doc["unit_map"].to<JsonObject>();
  unitMap["temperature_c"] = "C";
  unitMap["humidity_rh"] = "%";
  unitMap["vpd_kpa"] = "kPa";

  JsonObject metadata = doc["metadata"].to<JsonObject>();
  metadata["source"] = kMetadataSource;
  doc["event_key"] = eventKey;

  String payload;
  serializeJson(doc, payload);
  return payload;
}
} // namespace SensorRegistry
