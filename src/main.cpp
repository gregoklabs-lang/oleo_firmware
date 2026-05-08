#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <stdarg.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <mqtt_client.h>
#include <driver/periph_ctrl.h>
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include "soc/soc.h"
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <cstring>
#include <string>

#include "Config.hpp"
#include "oled_display.h"
#include "provisioning.h"
#include "sensor_registry.h"
#include "sht45_sensor.h"

#ifndef DEVICE_PREFIX
#define DEVICE_PREFIX "ERROR_PREFIX_"
#endif

#ifndef TOPIC_BASE
#define TOPIC_BASE "ERROR_TOPIC/"
#endif

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// ======================
// 🔹 CONFIGURACIÓN AWS
// ======================
constexpr const char kDefaultAwsEndpoint[] = "awvhj0h4worjs-ats.iot.us-east-1.amazonaws.com";
constexpr int32_t kDefaultAwsPort = 8883;
constexpr const char kDefaultAwsRegion[] = "us-east-1";
constexpr const char kDefaultThingName[] = "";
constexpr const char kDefaultEnv[] = "prod";
constexpr const char kDefaultDeviceKind[] = "climate_sensor";
constexpr const char kDefaultRootCaPath[] = "/certs/AmazonRootCA1.pem";
constexpr const char kDefaultDeviceCertPath[] = "/certs/device.pem.crt";
constexpr const char kDefaultPrivateKeyPath[] = "/certs/private.pem.key";
constexpr const char kDiagWifiRetriesKey[] = "wifi_retries";
constexpr const char kDiagMqttRetriesKey[] = "mqtt_retries";
constexpr const char kDiagLastResetKey[] = "last_reset_reason";
constexpr const char kDiagResetCountKey[] = "reset_count_total";
constexpr const char kDiagWdtKey[] = "wdt_resets";
constexpr const char kCertRootKey[] = "root_ca";
constexpr const char kCertDeviceKey[] = "device_cert";
constexpr const char kCertPrivateKey[] = "private_key";
constexpr const char kAwsEndpointKey[] = "endpoint";
constexpr const char kAwsPortKey[] = "port";
constexpr const char kAwsThingKey[] = "thing";
constexpr const char kAwsRegionKey[] = "region";
constexpr const char kDeviceIdKey[] = "device_id";
constexpr const char kDeviceUserKey[] = "user_id";
constexpr const char kDeviceEnvKey[] = "env";
constexpr const char kWifiSsidKey[] = "ssid";
constexpr const char kWifiPassKey[] = "password";

esp_mqtt_client_handle_t g_mqttClient = nullptr;

namespace
{
  constexpr int kBleButtonPin = 0;
  constexpr uint32_t kBleSessionDurationMs = 60000;
  constexpr uint32_t kWifiConnectTimeoutMs = 15000;
  constexpr uint32_t kButtonDebounceMs = 200;
  constexpr uint32_t kBleActivationHoldMs = 3000;
  constexpr uint32_t kIdentityLogDelayMs = 6000;
  constexpr uint16_t kMqttKeepAliveSeconds = 15;
  constexpr uint32_t kAwsBackoffInitialMs = 1000;
  constexpr uint32_t kAwsBackoffMaxMs = 16000;
  constexpr uint32_t kAwsInitialConnectGraceMs = 3000;
  constexpr uint32_t kWifiBackoffDelaysMs[] = {2000, 4000, 8000, 16000, 30000, 60000};
  constexpr size_t kWifiBackoffStepCount =
      sizeof(kWifiBackoffDelaysMs) / sizeof(kWifiBackoffDelaysMs[0]);
  constexpr uint32_t kTaskWatchdogTimeoutSeconds = 8;
  constexpr uint32_t kHardwareWatchdogTimeoutMs = 12000;
  constexpr uint32_t kHardwareWatchdogPrescaler = 8000;

  enum class SystemState : uint8_t
  {
    WIFI_DISCONNECTED = 0,
    WIFI_CONNECTED,
    BLE_ACTIVE,
  };

  String g_deviceId;
  String g_userId;
  String g_environment;
  String g_awsRegion;
  String g_macAddress;
  String g_bootSessionId;
  SystemState g_state = SystemState::WIFI_DISCONNECTED;

  bool g_wifiConnected = false;
  bool g_wifiConnecting = false;
  uint32_t g_wifiConnectStart = 0;
  uint32_t g_nextWifiAttemptMs = 0;
  size_t g_wifiBackoffIndex = 0;

  bool g_bleActive = false;
  uint32_t g_bleStartMs = 0;

  volatile bool g_bleButtonInterrupt = false;
  uint32_t g_lastButtonHandledMs = 0;
  bool g_bleButtonPending = false;
  uint32_t g_bleButtonPressStartMs = 0;
  uint32_t g_identityLogTargetMs = 0;
  bool g_identityLogReady = false;

  // ===== AWS Flags
  bool g_mqttConnected = false;
  uint32_t g_nextAwsAttemptMs = 0;
  uint32_t g_currentAwsBackoffMs = kAwsBackoffInitialMs;
  bool g_claimPending = false;
  bool g_sensorRegistryPending = true;
  bool g_awsCredentialsLoaded = false;
  bool g_spiffsReady = false;
  bool g_mqttClientStarted = false;
  uint32_t g_eventSequence = 0;
  String g_pendingClaimEventKey;
  String g_pendingSensorRegistryEventKey;
  String g_rootCaPem;
  String g_deviceCertPem;
  String g_privateKeyPem;
  bool g_taskWatchdogEnabled = false;
  bool g_hwWatchdogEnabled = false;
  unsigned long lastHeartbeatMs = 0;
  unsigned long lastTelemetryMs = 0;
  unsigned long lastSensorLogMs = 0;
  const unsigned long HEARTBEAT_INTERVAL = 60000; // 60s
  const unsigned long TELEMETRY_INTERVAL = 8000; // Tiempo en que se envia playload
  const unsigned long SENSOR_LOG_INTERVAL = 8000; // Tiempo de registro en esp32
  String g_rootCaPath;
  String g_deviceCertPath;
  String g_privateKeyPath;

  String toArduino(const std::string &value)
  {
    return value.empty() ? String() : String(value.c_str());
  }

  String buildBootSessionId()
  {
    char buffer[17] = {0};
    const uint32_t a = static_cast<uint32_t>(esp_random());
    const uint32_t b = static_cast<uint32_t>(esp_random());
    snprintf(buffer, sizeof(buffer), "%08lx%08lx",
             static_cast<unsigned long>(a),
             static_cast<unsigned long>(b));
    return String(buffer);
  }

  String nextEventKey(const char *kind)
  {
    ++g_eventSequence;
    char buffer[128] = {0};
    snprintf(buffer,
             sizeof(buffer),
             "%s:%s:%s:%lu:%lu",
             kind ? kind : "event",
             g_deviceId.c_str(),
             g_bootSessionId.c_str(),
             static_cast<unsigned long>(g_eventSequence),
             static_cast<unsigned long>(millis()));
    return String(buffer);
  }

  void seedConfigDefaults()
  {
    if (!Config::exists("aws", kAwsEndpointKey))
    {
      Config::setString("aws", kAwsEndpointKey, std::string(kDefaultAwsEndpoint));
    }
    if (!Config::exists("aws", kAwsRegionKey))
    {
      Config::setString("aws", kAwsRegionKey, std::string(kDefaultAwsRegion));
    }
    if (!Config::exists("aws", kAwsThingKey))
    {
      Config::setString("aws", kAwsThingKey, std::string(kDefaultThingName));
    }
    if (!Config::exists("aws", kAwsPortKey))
    {
      Config::setInt("aws", kAwsPortKey, kDefaultAwsPort);
    }
    if (!Config::exists("device", kDeviceEnvKey))
    {
      Config::setString("device", kDeviceEnvKey, std::string(kDefaultEnv));
    }
    if (!Config::exists("certs", kCertRootKey))
    {
      Config::setString("certs", kCertRootKey, std::string(kDefaultRootCaPath));
    }
    if (!Config::exists("certs", kCertDeviceKey))
    {
      Config::setString("certs", kCertDeviceKey, std::string(kDefaultDeviceCertPath));
    }
    if (!Config::exists("certs", kCertPrivateKey))
    {
      Config::setString("certs", kCertPrivateKey, std::string(kDefaultPrivateKeyPath));
    }
    if (!Config::exists("diag", kDiagWifiRetriesKey))
    {
      Config::setInt("diag", kDiagWifiRetriesKey, 0);
    }
    if (!Config::exists("diag", kDiagMqttRetriesKey))
    {
      Config::setInt("diag", kDiagMqttRetriesKey, 0);
    }
    if (!Config::exists("diag", kDiagLastResetKey))
    {
      Config::setInt("diag", kDiagLastResetKey, 0);
    }
    if (!Config::exists("diag", kDiagResetCountKey))
    {
      Config::setInt("diag", kDiagResetCountKey, 0);
    }
    if (!Config::exists("diag", kDiagWdtKey))
    {
      Config::setInt("diag", kDiagWdtKey, 0);
    }
  }

  void incrementDiagCounter(const char *key)
  {
    const int32_t current = Config::getInt("diag", key, 0);
    Config::setInt("diag", key, current + 1);
  }

  bool loadWifiCredentials(String &ssid, String &password)
  {
    ssid = toArduino(Config::getString("wifi", kWifiSsidKey, ""));
    password = toArduino(Config::getString("wifi", kWifiPassKey, ""));
    return ssid.length() > 0;
  }

  // ========================== LOGGING
  void logWithDeviceId(const char *fmt, ...)
  {
    char buffer[256] = {0};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    const char *deviceId = g_deviceId.length() > 0 ? g_deviceId.c_str() : "UNKNOWN";
    Serial.printf("[%s] %s", deviceId, buffer);
  }
  void IRAM_ATTR onBleButtonPressed() { g_bleButtonInterrupt = true; }

  // ========================== AWS HELPERS
  void clearAwsCredentials()
  {
    if (g_mqttClient)
    {
      esp_mqtt_client_stop(g_mqttClient);
      esp_mqtt_client_destroy(g_mqttClient);
      g_mqttClient = nullptr;
    }
    g_mqttClientStarted = false;
    g_mqttConnected = false;
    g_awsCredentialsLoaded = false;
    g_sensorRegistryPending = true;
    g_rootCaPem = "";
    g_deviceCertPem = "";
    g_privateKeyPem = "";
    g_nextAwsAttemptMs = 0;
    g_currentAwsBackoffMs = kAwsBackoffInitialMs;
    g_pendingClaimEventKey = "";
    g_pendingSensorRegistryEventKey = "";
  }

  void resetAwsBackoff()
  {
    g_currentAwsBackoffMs = kAwsBackoffInitialMs;
    g_nextAwsAttemptMs = 0;
  }

  void scheduleAwsBackoff(const char *reason)
  {
    incrementDiagCounter(kDiagMqttRetriesKey);
    const uint32_t delayMs = g_currentAwsBackoffMs;
    logWithDeviceId("[MQTT] Reintento por %s en %lu ms\n",
                    reason ? reason : "reintento",
                    static_cast<unsigned long>(delayMs));
    g_nextAwsAttemptMs = millis() + delayMs;
    if (g_currentAwsBackoffMs < kAwsBackoffMaxMs)
    {
      const uint32_t doubled = g_currentAwsBackoffMs * 2;
      g_currentAwsBackoffMs = doubled > kAwsBackoffMaxMs ? kAwsBackoffMaxMs : doubled;
    }
  }

  esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event)
  {
    if (!event)
    {
      return ESP_FAIL;
    }

    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
      g_mqttConnected = true;
      resetAwsBackoff();
      logWithDeviceId("[MQTT] Conectado\n");
      break;
    case MQTT_EVENT_DISCONNECTED:
      g_mqttConnected = false;
      logWithDeviceId("[MQTT] Desconectado\n");
      scheduleAwsBackoff("desconexion");
      break;
    case MQTT_EVENT_ERROR:
      logWithDeviceId("[MQTT] Error en evento MQTT\n");
      break;
    default:
      break;
    }

    return ESP_OK;
  }

  bool setupAWS()
{
  if (g_awsCredentialsLoaded && g_mqttClient)
  {
    return true;
  }

  if (!g_spiffsReady)
  {
    Serial.println("[AWS] ? SPIFFS no montado");
    return false;
  }

  g_rootCaPath = toArduino(Config::getString("certs", kCertRootKey, kDefaultRootCaPath));
  g_deviceCertPath = toArduino(Config::getString("certs", kCertDeviceKey, kDefaultDeviceCertPath));
  g_privateKeyPath = toArduino(Config::getString("certs", kCertPrivateKey, kDefaultPrivateKeyPath));

  if (!SPIFFS.exists(g_rootCaPath.c_str()) || !SPIFFS.exists(g_deviceCertPath.c_str()) ||
      !SPIFFS.exists(g_privateKeyPath.c_str()))
  {
    Serial.println("[AWS] ? Certificados no encontrados en SPIFFS");
    clearAwsCredentials();
    return false;
  }

  File ca = SPIFFS.open(g_rootCaPath.c_str(), "r");
  File cert = SPIFFS.open(g_deviceCertPath.c_str(), "r");
  File key = SPIFFS.open(g_privateKeyPath.c_str(), "r");

  if (!ca || !cert || !key)
  {
    Serial.println("[AWS] ? No se pudieron abrir los certificados");
    if (ca)
      ca.close();
    if (cert)
      cert.close();
    if (key)
      key.close();
    clearAwsCredentials();
    return false;
  }

  g_rootCaPem = ca.readString();
  g_deviceCertPem = cert.readString();
  g_privateKeyPem = key.readString();

  ca.close();
  cert.close();
  key.close();

  if (g_rootCaPem.length() == 0 || g_deviceCertPem.length() == 0 || g_privateKeyPem.length() == 0)
  {
    Serial.println("[AWS] ? Certificados vacios o corruptos");
    clearAwsCredentials();
    return false;
  }

  if (g_mqttClient)
  {
    esp_mqtt_client_stop(g_mqttClient);
    esp_mqtt_client_destroy(g_mqttClient);
    g_mqttClient = nullptr;
    g_mqttClientStarted = false;
  }

  String endpoint = toArduino(Config::getString("aws", kAwsEndpointKey, kDefaultAwsEndpoint));
  g_awsRegion = toArduino(Config::getString("aws", kAwsRegionKey, kDefaultAwsRegion));
  String thingName =
      toArduino(Config::getString("aws", kAwsThingKey, std::string(g_deviceId.c_str())));
  if (thingName.isEmpty())
  {
    thingName = g_deviceId;
  }
  const int32_t awsPort = Config::getInt("aws", kAwsPortKey, kDefaultAwsPort);

  if (endpoint.isEmpty())
  {
    Serial.println("[AWS] ? Endpoint no configurado");
    return false;
  }

  esp_mqtt_client_config_t config = {};
  config.host = endpoint.c_str();
  config.port = static_cast<int>(awsPort);
  config.client_id = thingName.c_str();
  config.transport = MQTT_TRANSPORT_OVER_SSL;
  config.cert_pem = g_rootCaPem.c_str();
  config.client_cert_pem = g_deviceCertPem.c_str();
  config.client_key_pem = g_privateKeyPem.c_str();
  config.buffer_size = 1024;
  config.keepalive = kMqttKeepAliveSeconds;
  config.event_handle = mqttEventHandler;

  g_mqttClient = esp_mqtt_client_init(&config);
  if (!g_mqttClient)
  {
    Serial.println("[AWS] ? No se pudo crear el cliente MQTT");
    clearAwsCredentials();
    return false;
  }

  g_awsCredentialsLoaded = true;
  g_mqttClientStarted = false;
  resetAwsBackoff();
  logWithDeviceId("[AWS] Configuracion MQTT lista\n");
  return true;
}

bool connectAWS()
  {
    if (!g_awsCredentialsLoaded || !g_mqttClient)
    {
      g_mqttConnected = false;
      return false;
    }

    const uint32_t now = millis();
    if (g_nextAwsAttemptMs != 0 && now < g_nextAwsAttemptMs)
    {
      return false;
    }

    if (!g_mqttClientStarted)
    {
      Serial.print("[AWS] 🔌 Iniciando cliente MQTT... ");
      if (esp_mqtt_client_start(g_mqttClient) == ESP_OK)
      {
        Serial.println("listo");
        g_mqttClientStarted = true;
        g_nextAwsAttemptMs = millis() + kAwsInitialConnectGraceMs;
        return true;
      }
      Serial.println("fallo");
      scheduleAwsBackoff("inicio fallido");
      return false;
    }

    esp_err_t err = esp_mqtt_client_reconnect(g_mqttClient);
    if (err == ESP_OK)
    {
      Serial.println("[AWS] Reintentando conexion MQTT");
      g_nextAwsAttemptMs = millis() + g_currentAwsBackoffMs;
      return true;
    }

    logWithDeviceId("[AWS] Error al reintentar MQTT (%d)\n", static_cast<int>(err));
    scheduleAwsBackoff("error reconectar");
    return false;
  }

  void sendProvisioningClaim()
  {
    if (!g_claimPending || !g_mqttClient || !g_mqttConnected)
    {
      return;
    }

    if (g_userId.isEmpty())
    {
      logWithDeviceId("[AWS] Mensaje MQTT no enviado (user_id vacio)\n");
      g_claimPending = false;
      return;
    }

    if (g_pendingClaimEventKey.length() == 0)
    {
      g_pendingClaimEventKey = nextEventKey("claim");
    }

    const String payload = "{\"device_id\":\"" + g_deviceId +
                           "\",\"user_id\":\"" + g_userId +
                           "\",\"device_kind\":\"" + String(kDefaultDeviceKind) +
                           "\",\"event_key\":\"" + g_pendingClaimEventKey + "\"}";
    const String topic = String(TOPIC_BASE) + g_deviceId + "/claim";

    logWithDeviceId("[AWS] Publicando claim -> topic=%s\n", topic.c_str());

    const int msgId = esp_mqtt_client_publish(g_mqttClient,
                                              topic.c_str(),
                                              payload.c_str(),
                                              static_cast<int>(payload.length()),
                                              1,
                                              0);
    if (msgId >= 0)
    {
      logWithDeviceId("[AWS] Mensaje MQTT enviado\n");
      g_claimPending = false;
      g_pendingClaimEventKey = "";
    }
    else
    {
      logWithDeviceId("[AWS] Mensaje MQTT no enviado\n");
    }
  }

  void sendSensorRegistry()
  {
    if (!g_sensorRegistryPending || !g_mqttClient || !g_mqttConnected)
    {
      return;
    }

    const String topic = SensorRegistry::buildTopic(g_deviceId);
    if (g_pendingSensorRegistryEventKey.length() == 0)
    {
      g_pendingSensorRegistryEventKey = nextEventKey("sensor_registry");
    }
    const String payload = SensorRegistry::buildPayload(g_deviceId, g_pendingSensorRegistryEventKey);

    logWithDeviceId("[AWS] Publicando sensor_registry -> topic=%s bytes=%u\n",
                    topic.c_str(),
                    static_cast<unsigned>(payload.length()));

    const int msgId = esp_mqtt_client_publish(g_mqttClient,
                                              topic.c_str(),
                                              payload.c_str(),
                                              static_cast<int>(payload.length()),
                                              1,
                                              0);
    if (msgId >= 0)
    {
      logWithDeviceId("[AWS] sensor_registry enviado\n");
      g_sensorRegistryPending = false;
      g_pendingSensorRegistryEventKey = "";
    }
    else
    {
      logWithDeviceId("[AWS] sensor_registry no enviado\n");
    }
  }

  void handleAWS()
  {
    if (!g_wifiConnected || !g_awsCredentialsLoaded || !g_mqttClient)
    {
      g_mqttConnected = false;
      resetAwsBackoff();
      return;
    }

    if (!g_mqttConnected)
    {
      connectAWS();
      return;
    }

    sendProvisioningClaim();
    sendSensorRegistry();
  }
  String formatState(SystemState state)
  {
    switch (state)
    {
    case SystemState::WIFI_CONNECTED:
      return "WIFI_CONNECTED";
    case SystemState::BLE_ACTIVE:
      return "BLE_ACTIVE";
    default:
      return "WIFI_DISCONNECTED";
    }
  }

  void updateSystemState()
  {
    SystemState newState = SystemState::WIFI_DISCONNECTED;
    if (g_bleActive)
    {
      newState = SystemState::BLE_ACTIVE;
    }
    else if (g_wifiConnected)
    {
      newState = SystemState::WIFI_CONNECTED;
    }

    if (newState != g_state)
    {
      g_state = newState;
      logWithDeviceId("[ESTADO] %s\n", formatState(g_state).c_str());
    }
  }

  void applyWifiConnectionStatus(bool connected)
  {
    if (g_wifiConnected != connected)
    {
      g_wifiConnected = connected;
      logWithDeviceId("[WIFI] Estado -> %s\n", connected ? "conectado" : "desconectado");
      Display::setConnectionStatus(connected);
      updateSystemState();
    }
    else
    {
      Display::setConnectionStatus(connected);
    }
  }

  void sendHeartbeat()
  {
      if (!g_mqttConnected)
      {
          Serial.println("[HEARTBEAT] Saltado (MQTT offline)");
          return;
      }

      String topic = String(TOPIC_BASE) + g_deviceId + "/heartbeat";
      const String eventKey = nextEventKey("heartbeat");

      JsonDocument doc;
      doc["mqtt_topic"] = topic;
      doc["client_id"] = g_deviceId;
      doc["wifi_rssi"] = WiFi.RSSI();
      doc["heap_free"] = esp_get_free_heap_size();
      doc["uptime_ms"] = millis();
      doc["fw"] = FW_VERSION;
      doc["event_key"] = eventKey;

      char buffer[256] = {0};
      const size_t len = serializeJson(doc, buffer, sizeof(buffer));
      if (len == 0 || len >= sizeof(buffer))
      {
          Serial.println("[HEARTBEAT] serialize failed");
          return;
      }

      int mid = esp_mqtt_client_publish(
          g_mqttClient,
          topic.c_str(),
          buffer,
          static_cast<int>(len),
          1,
          0);

      if (mid < 0)
      {
          Serial.printf("[HEARTBEAT] Publicación fallida en %s\n", topic.c_str());
      }
      else
      {
          Serial.printf("[HEARTBEAT] Enviado MID=%d -> %s\n", mid, topic.c_str());
      }
  }

  void sendTelemetry()
  {
      if (!g_mqttConnected)
      {
          Serial.println("[TELEMETRY] Saltado (MQTT offline)");
          return;
      }

      Sht45Sensor::Reading reading;
      if (!Sht45Sensor::read(reading) || !reading.valid)
      {
          Serial.println("[TELEMETRY] Lectura SHT45 fallida");
          return;
      }

      const String topic = Sht45Sensor::buildTelemetryTopic(g_deviceId);
      const String eventKey = nextEventKey("telemetry");
      const String payload = Sht45Sensor::buildTelemetryPayload(g_deviceId, reading, millis(), eventKey);

      const int mid = esp_mqtt_client_publish(
          g_mqttClient,
          topic.c_str(),
          payload.c_str(),
          static_cast<int>(payload.length()),
          1,
          0);

      if (mid < 0)
      {
          Serial.printf("[TELEMETRY] Publicacion fallida en %s\n", topic.c_str());
      }
      else
      {
          Serial.printf("[TELEMETRY] Enviado MID=%d -> %s\n", mid, topic.c_str());
      }
  }

  void logSensorReading()
  {
      Sht45Sensor::Reading reading;
      if (!Sht45Sensor::read(reading) || !reading.valid)
      {
          Serial.println("[SHT45] Lectura fallida");
          return;
      }

      Serial.printf("[SHT45] T=%.2f C H=%.2f %% VPD=%.2f kPa\n",
                    reading.temperatureC,
                    reading.humidityRh,
                    reading.vpdKpa);
  }

  void resetWifiBackoff()
  {
    g_wifiBackoffIndex = 0;
    g_nextWifiAttemptMs = 0;
  }

  void scheduleWifiReconnect(const char *reason)
  {
    incrementDiagCounter(kDiagWifiRetriesKey);
    if (g_wifiBackoffIndex >= kWifiBackoffStepCount)
    {
      logWithDeviceId("[WIFI] Backoff maximo alcanzado, reiniciando...\n");
      delay(100);
      esp_restart();
      return;
    }

    const size_t idx = g_wifiBackoffIndex;
    const uint32_t delayMs = kWifiBackoffDelaysMs[idx];
    logWithDeviceId("[WIFI] Reintento por %s en %lu ms\n",
                    reason ? reason : "reintento",
                    static_cast<unsigned long>(delayMs));
    g_nextWifiAttemptMs = millis() + delayMs;
    if (g_wifiBackoffIndex < kWifiBackoffStepCount)
    {
      ++g_wifiBackoffIndex;
    }
  }

  void clearStoredWifiCredentials()
  {
    const bool wasConnected = g_wifiConnected;
    WiFi.disconnect(true, true);
    g_wifiConnecting = false;
    applyWifiConnectionStatus(false);
    resetWifiBackoff();
    Config::setString("wifi", kWifiSsidKey, std::string());
    Config::setString("wifi", kWifiPassKey, std::string());
    if (wasConnected)
    {
      Provisioning::notifyStatus("wifi:desconectado");
    }
    logWithDeviceId("[WIFI] Credenciales eliminadas\n");
  }

  void stopBleSession()
  {
    if (!g_bleActive)
    {
      return;
    }

    Provisioning::stopBle();
    g_bleActive = false;
    Display::setBleActive(false);
    updateSystemState();
  }

  void startBleSession()
  {
    if (!Provisioning::isProvisioningAllowed())
    {
      logWithDeviceId("[BLE] Ventana de aprovisionamiento cerrada\n");
      return;
    }
    if (Provisioning::startBle())
    {
      g_bleActive = true;
      g_bleStartMs = millis();
      Display::setBleActive(true);
      updateSystemState();
      logWithDeviceId("[BLE] Sesion de aprovisionamiento activa por 60s\n");
    }
    else if (g_bleActive)
    {
      g_bleStartMs = millis();
    }
    else
    {
      logWithDeviceId("[BLE] No se pudo iniciar el modo de aprovisionamiento\n");
    }
  }

  void startWifiConnection(const char *ssid = nullptr, const char *password = nullptr,
                           bool resetBackoffOnStart = false)
  {
    if (resetBackoffOnStart)
    {
      resetWifiBackoff();
    }

    WiFi.disconnect(false, false);

    String connectSsid;
    String connectPassword;
    if (ssid && strlen(ssid) > 0)
    {
      connectSsid = ssid;
      connectPassword = password ? String(password) : String();
    }
    else if (!loadWifiCredentials(connectSsid, connectPassword))
    {
      logWithDeviceId("[WIFI] No hay credenciales configuradas\n");
      return;
    }

    logWithDeviceId("[WIFI] Conectando a '%s'\n", connectSsid.c_str());
    WiFi.begin(connectSsid.c_str(), connectPassword.length() > 0 ? connectPassword.c_str() : nullptr);

    g_wifiConnecting = true;
    g_wifiConnectStart = millis();
  }

  bool hasStoredCredentials()
  {
    String ssid;
    String password;
    return loadWifiCredentials(ssid, password);
  }

  void persistDeviceId()
  {
    Config::setString("device", kDeviceIdKey, std::string(g_deviceId.c_str()));
    if (!Config::exists("aws", kAwsThingKey) ||
        Config::getString("aws", kAwsThingKey, "").empty())
    {
      Config::setString("aws", kAwsThingKey, std::string(g_deviceId.c_str()));
    }
  }

  void loadStoredUserId()
  {
    g_userId = toArduino(Config::getString("device", kDeviceUserKey, ""));
  }

  void storeUserId(const String &userId)
  {
    g_userId = userId;
    Config::setString("device", kDeviceUserKey, std::string(g_userId.c_str()));
  }

  void scheduleIdentityLog()
  {
    g_identityLogTargetMs = millis() + kIdentityLogDelayMs;
    g_identityLogReady = false;
  }

  void logIdentityIfDue()
  {
    if (g_identityLogReady)
    {
      return;
    }

    if (millis() < g_identityLogTargetMs)
    {
      return;
    }

    const char *userId = g_userId.length() > 0 ? g_userId.c_str() : "(sin user_id)";
    logWithDeviceId("[IDENTIDAD] MAC disponible\n");
    logWithDeviceId("[IDENTIDAD] user_id %s\n",
                    g_userId.length() > 0 ? "configurado" : "ausente");
    g_identityLogReady = true;
  }

  String buildDeviceId()
  {
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK)
    {
      WiFi.macAddress(mac);
    }

    char macFormatted[18] = {0};
    snprintf(macFormatted, sizeof(macFormatted), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    g_macAddress = String(macFormatted);

    char suffix[7] = {0};
    snprintf(suffix, sizeof(suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);

    String id = DEVICE_PREFIX;
    id += suffix;
    return id;
  }

  void ensureDeviceIdentity()
  {
    const std::string storedId = Config::getString("device", kDeviceIdKey, "");
    if (!storedId.empty())
    {
      g_deviceId = storedId.c_str();
    }
    else
    {
      g_deviceId = buildDeviceId();
      persistDeviceId();
    }
    g_environment = toArduino(Config::getString("device", kDeviceEnvKey, kDefaultEnv));
  }

  void onProvisionedCredentials(const Provisioning::CredentialsData &creds)
  {
    logWithDeviceId("[BLE] Credenciales recibidas via BLE\n");
    Config::setString("wifi", kWifiSsidKey, std::string(creds.ssid.c_str()));
    Config::setString("wifi", kWifiPassKey, std::string(creds.password.c_str()));
    const std::string storedToken = Config::getString("device", "provision_token", "");
    if (storedToken.empty() && creds.provisionToken.length() > 0)
    {
      Config::setString("device", "provision_token", std::string(creds.provisionToken.c_str()));
    }

    if (creds.deviceId.length() > 0 && creds.deviceId != g_deviceId)
    {
      g_deviceId = creds.deviceId;
      persistDeviceId();
      Provisioning::begin(g_deviceId, onProvisionedCredentials);
    }

    if (creds.endpoint.length() > 0)
    {
      Config::setString("aws", kAwsEndpointKey, std::string(creds.endpoint.c_str()));
    }
    if (creds.region.length() > 0)
    {
      Config::setString("aws", kAwsRegionKey, std::string(creds.region.c_str()));
      g_awsRegion = creds.region;
    }
    if (creds.environment.length() > 0)
    {
      Config::setString("device", kDeviceEnvKey, std::string(creds.environment.c_str()));
      g_environment = creds.environment;
    }
    if (creds.thingName.length() > 0)
    {
      Config::setString("aws", kAwsThingKey, std::string(creds.thingName.c_str()));
    }
    if (creds.awsPort > 0)
    {
      Config::setInt("aws", kAwsPortKey, creds.awsPort);
    }

    Provisioning::notifyStatus("wifi:conectando");
    applyWifiConnectionStatus(false);
    startWifiConnection(creds.ssid.c_str(), creds.password.c_str(), true);

    if (creds.userId.length() > 0)
    {
      storeUserId(creds.userId);
      scheduleIdentityLog();
      logWithDeviceId("[BLE] user_id recibido\n");
    }

    g_claimPending = true;
  }

  void handleBleButton()
  {
    uint32_t now = millis();

    if (g_bleButtonInterrupt)
    {
      g_bleButtonInterrupt = false;

      if (now - g_lastButtonHandledMs >= kButtonDebounceMs &&
          digitalRead(kBleButtonPin) == LOW)
      {
        if (!g_bleButtonPending)
        {
          g_bleButtonPending = true;
          g_bleButtonPressStartMs = now;
        }
        g_lastButtonHandledMs = now;
      }
    }

    if (!g_bleButtonPending)
    {
      return;
    }

    if (digitalRead(kBleButtonPin) == LOW)
    {
      now = millis();
      if (now - g_bleButtonPressStartMs >= kBleActivationHoldMs)
      {
        g_bleButtonPending = false;
        g_lastButtonHandledMs = now;
        clearStoredWifiCredentials();
        startBleSession();
      }
      return;
    }

    g_bleButtonPending = false;
  }

  void handleBleTimeout()
  {
    if (!g_bleActive)
    {
      return;
    }

    if (millis() - g_bleStartMs >= kBleSessionDurationMs)
    {
      logWithDeviceId("[BLE] Tiempo de aprovisionamiento agotado\n");
      stopBleSession();
    }
  }

  void handleWifiStatus()
  {
    if (!g_wifiConnected && !g_wifiConnecting && hasStoredCredentials())
    {
      if (g_nextWifiAttemptMs == 0 || millis() >= g_nextWifiAttemptMs)
      {
        if (g_nextWifiAttemptMs != 0)
        {
          logWithDeviceId("[WIFI] Ejecutando reintento programado\n");
        }
        startWifiConnection();
      }
    }

    wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED)
    {
      if (!g_wifiConnected)
      {
        applyWifiConnectionStatus(true);
        {
          String ip = WiFi.localIP().toString();
          logWithDeviceId("[WIFI] IP: %s\n", ip.c_str());
        }
        Provisioning::notifyStatus("wifi:conectado");
        if (setupAWS())
        {
          connectAWS();   // 👈 Se conecta a AWS inmediatamente
        }
        g_claimPending = true;
        g_sensorRegistryPending = true;
        stopBleSession();
        resetWifiBackoff();
      }
      g_wifiConnecting = false;
      return;
    }

    if (g_wifiConnected)
    {
      logWithDeviceId("[WIFI] Conexion perdida\n");
      applyWifiConnectionStatus(false);
      Provisioning::notifyStatus("wifi:desconectado");
      g_wifiConnecting = false;
      scheduleWifiReconnect("desconexion wifi");
    }

    if (!g_wifiConnecting)
    {
      return;
    }

    const uint32_t elapsed = millis() - g_wifiConnectStart;
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
        elapsed > kWifiConnectTimeoutMs)
    {
      logWithDeviceId("[WIFI] Error al conectar\n");
      Provisioning::notifyStatus("wifi:error");
      WiFi.disconnect(false, false);
      g_wifiConnecting = false;
      applyWifiConnectionStatus(false);
      scheduleWifiReconnect("error conexion");
      return;
    }
  }

  void configureButton()
  {
    pinMode(kBleButtonPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(kBleButtonPin), onBleButtonPressed,
                    FALLING);
  }

  uint32_t hardwareWatchdogTicks(uint32_t timeoutMs)
  {
    const uint32_t clockHz = APB_CLK_FREQ / kHardwareWatchdogPrescaler;
    return (timeoutMs * clockHz) / 1000;
  }

  void hardwareWatchdogWriteEnable()
  {
    TIMERG0.wdtwprotect.wdt_wkey = TIMG_WDT_WKEY_VALUE;
  }

  void hardwareWatchdogWriteDisable()
  {
    TIMERG0.wdtwprotect.wdt_wkey = 0;
  }

  bool enableHardwareWatchdog()
  {
    if (g_hwWatchdogEnabled)
    {
      return true;
    }

    periph_module_enable(PERIPH_TIMG0_MODULE);
    hardwareWatchdogWriteEnable();

    TIMERG0.wdtconfig0.wdt_en = 0;
    TIMERG0.wdtconfig0.wdt_flashboot_mod_en = 0;
    TIMERG0.wdtconfig0.wdt_sys_reset_length = TIMG_WDT_RESET_LENGTH_3200_NS;
    TIMERG0.wdtconfig0.wdt_cpu_reset_length = TIMG_WDT_RESET_LENGTH_3200_NS;
    TIMERG0.wdtconfig0.wdt_use_xtal = 0;
    TIMERG0.wdtconfig0.wdt_stg3 = TIMG_WDT_STG_SEL_OFF;
    TIMERG0.wdtconfig0.wdt_stg2 = TIMG_WDT_STG_SEL_OFF;
    TIMERG0.wdtconfig0.wdt_stg1 = TIMG_WDT_STG_SEL_OFF;
    TIMERG0.wdtconfig0.wdt_stg0 = TIMG_WDT_STG_SEL_RESET_SYSTEM;

    TIMERG0.wdtconfig1.wdt_clk_prescale = kHardwareWatchdogPrescaler;
    TIMERG0.wdtconfig1.wdt_divcnt_rst = 1;

    TIMERG0.wdtconfig2.wdt_stg0_hold = hardwareWatchdogTicks(kHardwareWatchdogTimeoutMs);
    TIMERG0.wdtconfig3.wdt_stg1_hold = 0;
    TIMERG0.wdtconfig4.wdt_stg2_hold = 0;
    TIMERG0.wdtconfig5.wdt_stg3_hold = 0;

    TIMERG0.wdtconfig0.wdt_conf_update_en = 1;
    TIMERG0.wdtconfig0.wdt_en = 1;

    hardwareWatchdogWriteDisable();
    g_hwWatchdogEnabled = true;
    return true;
  }

  void feedHardwareWatchdog()
  {
    if (!g_hwWatchdogEnabled)
    {
      return;
    }
    hardwareWatchdogWriteEnable();
    TIMERG0.wdtfeed.wdt_feed = 1;
    hardwareWatchdogWriteDisable();
  }

  void disableHardwareWatchdog()
  {
    if (!g_hwWatchdogEnabled)
    {
      return;
    }
    hardwareWatchdogWriteEnable();
    TIMERG0.wdtconfig0.wdt_en = 0;
    TIMERG0.wdtconfig0.wdt_conf_update_en = 1;
    hardwareWatchdogWriteDisable();
    periph_module_disable(PERIPH_TIMG0_MODULE);
    g_hwWatchdogEnabled = false;
  }

  void setupWatchdogs()
  {
    bool initialized = false;
    if (!g_taskWatchdogEnabled &&
        esp_task_wdt_init(kTaskWatchdogTimeoutSeconds, true) == ESP_OK)
    {
      esp_task_wdt_add(nullptr);
      g_taskWatchdogEnabled = true;
      initialized = true;
    }

    if (enableHardwareWatchdog())
    {
      initialized = true;
    }

    if (initialized)
    {
      logWithDeviceId("[WATCHDOG] initialized\n");
    }
  }

  void feedWatchdog()
  {
    if (g_taskWatchdogEnabled)
    {
      esp_task_wdt_reset();
    }
    feedHardwareWatchdog();

    static uint32_t lastLogMs = 0;
    const uint32_t now = millis();
    if (now - lastLogMs >= 30000)
    {
      logWithDeviceId("[WATCHDOG] alive\n");
      lastLogMs = now;
    }
  }

  void disableWatchdogs()
  {
    if (g_taskWatchdogEnabled)
    {
      esp_task_wdt_delete(nullptr);
      esp_task_wdt_deinit();
      g_taskWatchdogEnabled = false;
    }
    disableHardwareWatchdog();
  }

  void recordResetInfo()
  {
    const esp_reset_reason_t reason = esp_reset_reason();
    Config::setInt("diag", kDiagLastResetKey, static_cast<int32_t>(reason));
    incrementDiagCounter(kDiagResetCountKey);
    if (reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT)
    {
      incrementDiagCounter(kDiagWdtKey);
    }
  }

  void logWatchdogResetIfNeeded()
  {
    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT)
    {
      logWithDeviceId("[WATCHDOG] *** reset detected ***\n");
    }
  }

} // namespace

void setup()
{
  Serial.begin(115200);
  Config::init();
  seedConfigDefaults();
  recordResetInfo();
  logWatchdogResetIfNeeded();

  g_spiffsReady = SPIFFS.begin(false);
  if (g_spiffsReady)
  {
    Serial.println("[SPIFFS] OK: montado correctamente");
  }
  else
  {
    Serial.println("[SPIFFS] Aviso: error al montar (sin formatear)");
    g_spiffsReady = SPIFFS.begin(true);
    if (g_spiffsReady)
    {
      Serial.println("[SPIFFS] OK: formateado y montado");
    }
    else
    {
      Serial.println("[SPIFFS] ❌ No se pudo montar SPIFFS");
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  ensureDeviceIdentity();
  loadStoredUserId();
  scheduleIdentityLog();
  logWithDeviceId("[BOOT] device_id: %s\n", g_deviceId.c_str());
  logWithDeviceId("[BOOT] entorno: %s\n", g_environment.c_str());
  g_bootSessionId = buildBootSessionId();
  logWithDeviceId("[BOOT] boot_session_id: %s\n", g_bootSessionId.c_str());

  Display::begin();
  Display::setConnectionStatus(false);
  Display::setBleActive(false);
  Display::forceRender();

  if (Sht45Sensor::begin())
  {
    Serial.println("[SHT45] Sensor inicializado");
  }
  else
  {
    Serial.println("[SHT45] No se encontro SHT45");
  }

  Provisioning::begin(g_deviceId, onProvisionedCredentials);


  configureButton();
  setupWatchdogs();

  if (hasStoredCredentials())
  {
    logWithDeviceId("[WIFI] Credenciales guardadas detectadas\n");
    startWifiConnection(nullptr, nullptr, true);
  }
  else
  {
    logWithDeviceId("[WIFI] No hay credenciales guardadas\n");
    applyWifiConnectionStatus(false);
    updateSystemState();
  }
}

void loop()
{
  feedWatchdog();
  unsigned long now = millis();
  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeatMs = now;
  }
  if (now - lastTelemetryMs >= TELEMETRY_INTERVAL) {
    sendTelemetry();
    lastTelemetryMs = now;
  }
  if (now - lastSensorLogMs >= SENSOR_LOG_INTERVAL) {
    logSensorReading();
    lastSensorLogMs = now;
  }
  handleBleButton();
  handleBleTimeout();
  handleWifiStatus();
  logIdentityIfDue();
  handleAWS(); // 👈 mantiene viva la conexión MQTT

  if (!g_wifiConnecting && !g_wifiConnected && !g_bleActive)
  {
    updateSystemState();
  }

  Provisioning::loop();
  Display::loop();

  delay(1);
}


