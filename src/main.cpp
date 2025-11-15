#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <stdarg.h>
#include <Preferences.h>
#include <esp_event.h>
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
constexpr const char kDefaultAwsEndpoint[] = "a7xxu98k219gv-ats.iot.us-east-1.amazonaws.com";
constexpr int32_t kDefaultAwsPort = 8883;
constexpr const char kDefaultAwsRegion[] = "us-east-1";
constexpr const char kDefaultThingName[] = "";
constexpr const char kDefaultEnv[] = "prod";
constexpr const char kDefaultRootCaPath[] = "/certs/AmazonRootCA1.pem";
constexpr const char kDefaultDeviceCertPath[] = "/certs/device.pem.crt";
constexpr const char kDefaultPrivateKeyPath[] = "/certs/private.pem.key";
constexpr const char kDiagWifiKey[] = "wifi_fail";
constexpr const char kDiagMqttKey[] = "mqtt_fail";
constexpr const char kDiagResetKey[] = "last_reset";
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

  Preferences g_settingsPrefs;
  bool g_settingsPrefsReady = false;
  constexpr char kSettingsPrefsNamespace[] = "settings";
  constexpr char kSettingsReservoirUnitsKey[] = "res_units";
  constexpr char kSettingsTempUnitsKey[] = "temp_units";
  constexpr char kSettingsNutrientsUnitsKey[] = "nutri_units";
  constexpr char kSettingsEmailKey[] = "email_notif";

  Preferences g_setpointsPrefs;
  bool g_setpointsPrefsReady = false;
  constexpr char kSetpointsPrefsNamespace[] = "setpoints";
  constexpr char kSetpointsPhKey[] = "ph_target";
  constexpr char kSetpointsEcKey[] = "ec_target";
  constexpr char kSetpointsTempKey[] = "temp_target";
  constexpr char kSetpointsFlowKey[] = "flow_target";
  constexpr char kSetpointsModeKey[] = "dosing_mode";
  constexpr char kSetpointsVersionKey[] = "version";
  constexpr char kSetpointsReservoirKey[] = "reservoir";

  struct DownlinkSettings
  {
    String reservoirUnits;
    String temperatureUnits;
    String nutrientsUnits;
    bool emailNotifications = false;
  };

  DownlinkSettings g_downlinkSettings;
  String g_downlinkSettingsTopic;

  struct DownlinkSetpoints
  {
    float phTarget = 0.0f;
    float ecTarget = 0.0f;
    float tempTarget = 0.0f;
    float flowTarget = 0.0f;
    String dosingMode;
    String version;
    float reservoirSize = 0.0f;
  };

  DownlinkSetpoints g_downlinkSetpoints;
  String g_downlinkSetpointsTopic;

  String g_deviceId;
  String g_userId;
  String g_environment;
  String g_awsRegion;
  String g_macAddress;
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
  bool g_awsCredentialsLoaded = false;
  bool g_spiffsReady = false;
  bool g_mqttClientStarted = false;
  String g_rootCaPem;
  String g_deviceCertPem;
  String g_privateKeyPem;
  bool g_taskWatchdogEnabled = false;
  bool g_hwWatchdogEnabled = false;
  unsigned long lastHeartbeatMs = 0;
  const unsigned long HEARTBEAT_INTERVAL = 60000; // 60s
  String g_rootCaPath;
  String g_deviceCertPath;
  String g_privateKeyPath;

  String toArduino(const std::string &value)
  {
    return value.empty() ? String() : String(value.c_str());
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
    if (!Config::exists("diag", kDiagWifiKey))
    {
      Config::setInt("diag", kDiagWifiKey, 0);
    }
    if (!Config::exists("diag", kDiagMqttKey))
    {
      Config::setInt("diag", kDiagMqttKey, 0);
    }
    if (!Config::exists("diag", kDiagResetKey))
    {
      Config::setInt("diag", kDiagResetKey, 0);
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

  void updateDownlinkTopics()
  {
    g_downlinkSettingsTopic = String(TOPIC_BASE) + g_deviceId + "/downlink/settings";
    g_downlinkSetpointsTopic = String(TOPIC_BASE) + g_deviceId + "/downlink/setpoints";
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

  const char *unitsOrDefault(const String &value, const char *fallback)
  {
    return value.length() > 0 ? value.c_str() : fallback;
  }

  void logStoredSetpoints(const char *header = "[SETPOINTS] 🔁 Cargados desde NVS:")
  {
    logWithDeviceId("%s\n", header);
    logWithDeviceId("   - pH target: %.2f\n", g_downlinkSetpoints.phTarget);
    logWithDeviceId("   - EC target: %.2f %s\n",
                    g_downlinkSetpoints.ecTarget,
                    unitsOrDefault(g_downlinkSettings.nutrientsUnits, "N/A"));
    logWithDeviceId("   - Temp target: %.2f %s\n",
                    g_downlinkSetpoints.tempTarget,
                    unitsOrDefault(g_downlinkSettings.temperatureUnits, "N/A"));
    logWithDeviceId("   - Flow target (L/min): %.2f\n", g_downlinkSetpoints.flowTarget);
    logWithDeviceId("   - Dosing mode: %s\n", g_downlinkSetpoints.dosingMode.c_str());
    logWithDeviceId("   - Version: %s\n", g_downlinkSetpoints.version.c_str());
    logWithDeviceId("   - Reservoir size: %.2f %s\n",
                    g_downlinkSetpoints.reservoirSize,
                    unitsOrDefault(g_downlinkSettings.reservoirUnits, "N/A"));
  }

  void logStoredSettings(const char *header = "[SETTINGS] 🔁 Cargadas desde NVS:")
  {
    logWithDeviceId("%s\n", header);
    logWithDeviceId("   - Reservoir units: %s\n",
                    unitsOrDefault(g_downlinkSettings.reservoirUnits, "N/A"));
    logWithDeviceId("   - Temp units: %s\n",
                    unitsOrDefault(g_downlinkSettings.temperatureUnits, "N/A"));
    logWithDeviceId("   - Nutrients units: %s\n",
                    unitsOrDefault(g_downlinkSettings.nutrientsUnits, "N/A"));
    logWithDeviceId("   - Email notifications: %d\n", g_downlinkSettings.emailNotifications ? 1 : 0);
  }

  void IRAM_ATTR onBleButtonPressed() { g_bleButtonInterrupt = true; }

  // =========================================================
  // 🔽 DOWNLINK SETTINGS HANDLER
  // =========================================================

  bool beginSettingsPrefs()
  {
    if (g_settingsPrefsReady)
    {
      return true;
    }

    g_settingsPrefsReady = g_settingsPrefs.begin(kSettingsPrefsNamespace, false);
    if (!g_settingsPrefsReady)
    {
      Serial.println("[settings] No se pudieron abrir las preferencias");
    }
    return g_settingsPrefsReady;
  }

  void loadSettingsFromPrefs()
  {
    if (!beginSettingsPrefs())
    {
      logWithDeviceId("[SETTINGS] ⚠️ No se pudieron cargar las preferencias\n");
      return;
    }

    const bool hasAnyStored =
        g_settingsPrefs.isKey(kSettingsReservoirUnitsKey) ||
        g_settingsPrefs.isKey(kSettingsTempUnitsKey) ||
        g_settingsPrefs.isKey(kSettingsNutrientsUnitsKey) ||
        g_settingsPrefs.isKey(kSettingsEmailKey);

    g_downlinkSettings.reservoirUnits = g_settingsPrefs.getString(kSettingsReservoirUnitsKey, g_downlinkSettings.reservoirUnits);
    g_downlinkSettings.temperatureUnits = g_settingsPrefs.getString(kSettingsTempUnitsKey, g_downlinkSettings.temperatureUnits);
    g_downlinkSettings.nutrientsUnits = g_settingsPrefs.getString(kSettingsNutrientsUnitsKey, g_downlinkSettings.nutrientsUnits);
    g_downlinkSettings.emailNotifications = g_settingsPrefs.getBool(kSettingsEmailKey, g_downlinkSettings.emailNotifications);

    if (!hasAnyStored)
    {
      logWithDeviceId("[SETTINGS] ⚠️ No hay configuraciones almacenadas (usando valores por defecto)\n");
    }

    logStoredSettings();
  }

  bool beginSetpointsPrefs()
  {
    if (g_setpointsPrefsReady)
    {
      return true;
    }

    g_setpointsPrefsReady = g_setpointsPrefs.begin(kSetpointsPrefsNamespace, false);
    if (!g_setpointsPrefsReady)
    {
      logWithDeviceId("[SETPOINTS] ⚠️ No se pudieron abrir las preferencias\n");
      return false;
    }

    return g_setpointsPrefsReady;
  }

  void loadSetpointsFromPrefs()
  {
    if (!beginSetpointsPrefs())
    {
      return;
    }

    const bool hasAnyStored =
        g_setpointsPrefs.isKey(kSetpointsPhKey) ||
        g_setpointsPrefs.isKey(kSetpointsEcKey) ||
        g_setpointsPrefs.isKey(kSetpointsTempKey) ||
        g_setpointsPrefs.isKey(kSetpointsFlowKey) ||
        g_setpointsPrefs.isKey(kSetpointsModeKey) ||
        g_setpointsPrefs.isKey(kSetpointsVersionKey) ||
        g_setpointsPrefs.isKey(kSetpointsReservoirKey);

    g_downlinkSetpoints.phTarget = g_setpointsPrefs.getFloat(kSetpointsPhKey, g_downlinkSetpoints.phTarget);
    g_downlinkSetpoints.ecTarget = g_setpointsPrefs.getFloat(kSetpointsEcKey, g_downlinkSetpoints.ecTarget);
    g_downlinkSetpoints.tempTarget = g_setpointsPrefs.getFloat(kSetpointsTempKey, g_downlinkSetpoints.tempTarget);
    g_downlinkSetpoints.flowTarget = g_setpointsPrefs.getFloat(kSetpointsFlowKey, g_downlinkSetpoints.flowTarget);
    g_downlinkSetpoints.dosingMode = g_setpointsPrefs.getString(kSetpointsModeKey, g_downlinkSetpoints.dosingMode);
    g_downlinkSetpoints.version = g_setpointsPrefs.getString(kSetpointsVersionKey, g_downlinkSetpoints.version);
    g_downlinkSetpoints.reservoirSize = g_setpointsPrefs.getFloat(kSetpointsReservoirKey, g_downlinkSetpoints.reservoirSize);

    if (!hasAnyStored)
    {
      logWithDeviceId("[SETPOINTS] ⚠️ No hay setpoints almacenados\n");
      return;
    }

    logStoredSetpoints();
  }

  void handleSettingsDownlink(JsonDocument &doc)
  {
    const char *cmd = doc["cmd"] | "";
    const bool isUpdateSettings =
        strcmp(cmd, "update_settings") == 0 ||
        strcmp(cmd, "update_user_settings") == 0;
    if (!isUpdateSettings)
    {
      logWithDeviceId("[MQTT] ⚠️ Comando inesperado: %s\n", cmd);
      return;
    }

    if (!doc["settings"].is<JsonObject>())
    {
      logWithDeviceId("[MQTT] ⚠️ Campo 'settings' ausente o invalido\n");
      return;
    }

    JsonObject settings = doc["settings"].as<JsonObject>();

    if (settings["reservoir_size_units"].isNull() ||
        settings["temperature_units"].isNull() ||
        settings["nutrients_units"].isNull() ||
        settings["email_notifications"].isNull())
    {
      logWithDeviceId("[MQTT] ⚠️ Configuración incompleta recibida\n");
      return;
    }

    g_downlinkSettings.reservoirUnits = settings["reservoir_size_units"].as<const char *>();
    g_downlinkSettings.temperatureUnits = settings["temperature_units"].as<const char *>();
    g_downlinkSettings.nutrientsUnits = settings["nutrients_units"].as<const char *>();
    g_downlinkSettings.emailNotifications = settings["email_notifications"].as<bool>();

    if (!beginSettingsPrefs())
    {
      logWithDeviceId("[MQTT] ⚠️ No se pudieron abrir preferencias para guardar ajustes\n");
      return;
    }

    g_settingsPrefs.putString(kSettingsReservoirUnitsKey, g_downlinkSettings.reservoirUnits);
    g_settingsPrefs.putString(kSettingsTempUnitsKey, g_downlinkSettings.temperatureUnits);
    g_settingsPrefs.putString(kSettingsNutrientsUnitsKey, g_downlinkSettings.nutrientsUnits);
    g_settingsPrefs.putBool(kSettingsEmailKey, g_downlinkSettings.emailNotifications);

    logStoredSettings("[SETTINGS] ✅ Actualizadas desde downlink:");
  }

  void handleSetpointsDownlink(JsonDocument &doc)
  {
    const char *cmd = doc["cmd"] | "";
    if (strcmp(cmd, "update_setpoints") != 0)
    {
      logWithDeviceId("[MQTT] ⚠️ Comando inesperado para setpoints: %s\n", cmd);
      return;
    }

    if (!doc["setpoints"].is<JsonObject>())
    {
      logWithDeviceId("[MQTT] ⚠️ Campo 'setpoints' ausente o invalido\n");
      return;
    }

    JsonObject sp = doc["setpoints"].as<JsonObject>();

    bool hasUpdate = false;

    if (!sp["ph_target"].isNull())
    {
      g_downlinkSetpoints.phTarget = sp["ph_target"].as<float>();
      hasUpdate = true;
    }
    if (!sp["ec_target"].isNull())
    {
      g_downlinkSetpoints.ecTarget = sp["ec_target"].as<float>();
      hasUpdate = true;
    }
    if (!sp["temp_target"].isNull())
    {
      g_downlinkSetpoints.tempTarget = sp["temp_target"].as<float>();
      hasUpdate = true;
    }
    if (!sp["flow_target_l_min"].isNull())
    {
      g_downlinkSetpoints.flowTarget = sp["flow_target_l_min"].as<float>();
      hasUpdate = true;
    }
    if (!sp["dosing_mode"].isNull())
    {
      g_downlinkSetpoints.dosingMode = sp["dosing_mode"].as<const char *>();
      hasUpdate = true;
    }
    if (!sp["version"].isNull())
    {
      g_downlinkSetpoints.version = sp["version"].as<const char *>();
      hasUpdate = true;
    }
    if (!sp["reservoir_size"].isNull())
    {
      g_downlinkSetpoints.reservoirSize = sp["reservoir_size"].as<float>();
      hasUpdate = true;
    }

    if (!hasUpdate)
    {
      logWithDeviceId("[MQTT] ⚠️ Setpoints recibidos sin cambios (todos nulos)\n");
      return;
    }

    if (!beginSetpointsPrefs())
    {
      logWithDeviceId("[MQTT] ⚠️ No se pudieron abrir preferencias para guardar setpoints\n");
      return;
    }

    g_setpointsPrefs.putFloat(kSetpointsPhKey, g_downlinkSetpoints.phTarget);
    g_setpointsPrefs.putFloat(kSetpointsEcKey, g_downlinkSetpoints.ecTarget);
    g_setpointsPrefs.putFloat(kSetpointsTempKey, g_downlinkSetpoints.tempTarget);
    g_setpointsPrefs.putFloat(kSetpointsFlowKey, g_downlinkSetpoints.flowTarget);
    g_setpointsPrefs.putString(kSetpointsModeKey, g_downlinkSetpoints.dosingMode);
    g_setpointsPrefs.putString(kSetpointsVersionKey, g_downlinkSetpoints.version);
    g_setpointsPrefs.putFloat(kSetpointsReservoirKey, g_downlinkSetpoints.reservoirSize);

    logStoredSetpoints("[SETPOINTS] ✅ Actualizados desde downlink:");
  }

  void mqttCallback(const char *topic, const uint8_t *payload, unsigned int length)
  {
    if (!topic)
    {
      logWithDeviceId("[MQTT] ⚠️ Callback sin topic\n");
      return;
    }

    const String topicStr(topic);
    const bool isSettingsTopic = g_downlinkSettingsTopic.length() && topicStr == g_downlinkSettingsTopic;
    const bool isSetpointsTopic = g_downlinkSetpointsTopic.length() && topicStr == g_downlinkSetpointsTopic;

    if (!isSettingsTopic && !isSetpointsTopic)
    {
      // Ignora otros topics manteniendo el loop limpio.
      return;
    }

    String payloadStr;
    payloadStr.reserve(length + 1);
    for (unsigned int i = 0; i < length; ++i)
    {
      payloadStr += static_cast<char>(payload[i]);
    }

    logWithDeviceId("📩 [MQTT] Mensaje en %s:\n", topicStr.c_str());
    logWithDeviceId("%s\n", payloadStr.c_str());

    DynamicJsonDocument doc(768);
    DeserializationError err = deserializeJson(doc, payloadStr);
    if (err)
    {
      logWithDeviceId("[MQTT] ⚠️ Error al parsear JSON: %s\n", err.c_str());
      return;
    }

    if (isSettingsTopic)
    {
      handleSettingsDownlink(doc);
    }
    else if (isSetpointsTopic)
    {
      handleSetpointsDownlink(doc);
    }
  }

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
    g_rootCaPem = "";
    g_deviceCertPem = "";
    g_privateKeyPem = "";
    g_nextAwsAttemptMs = 0;
    g_currentAwsBackoffMs = kAwsBackoffInitialMs;
  }

  void resetAwsBackoff()
  {
    g_currentAwsBackoffMs = kAwsBackoffInitialMs;
    g_nextAwsAttemptMs = 0;
  }

  void scheduleAwsBackoff(const char *reason)
  {
    incrementDiagCounter(kDiagMqttKey);
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
      if (g_downlinkSettingsTopic.length() > 0)
      {
        const int msgId = esp_mqtt_client_subscribe(event->client, g_downlinkSettingsTopic.c_str(), 1);
        if (msgId >= 0)
        {
          logWithDeviceId("[MQTT] Suscrito a %s\n", g_downlinkSettingsTopic.c_str());
        }
        else
        {
          logWithDeviceId("[MQTT] ⚠️ Fallo al suscribir %s\n", g_downlinkSettingsTopic.c_str());
        }
      }
      if (g_downlinkSetpointsTopic.length() > 0)
      {
        const int msgId = esp_mqtt_client_subscribe(event->client, g_downlinkSetpointsTopic.c_str(), 1);
        if (msgId >= 0)
        {
          logWithDeviceId("[MQTT] Suscrito a %s\n", g_downlinkSetpointsTopic.c_str());
        }
        else
        {
          logWithDeviceId("[MQTT] ⚠️ Fallo al suscribir %s\n", g_downlinkSetpointsTopic.c_str());
        }
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      g_mqttConnected = false;
      logWithDeviceId("[MQTT] Desconectado\n");
      scheduleAwsBackoff("desconexion");
      break;
    case MQTT_EVENT_DATA:
    {
      if (!event->topic || event->topic_len == 0)
      {
        break;
      }
      String topicCopy(event->topic, event->topic_len);
      mqttCallback(topicCopy.c_str(), reinterpret_cast<const uint8_t *>(event->data), event->data_len);
      break;
    }
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
        g_nextAwsAttemptMs = millis() + g_currentAwsBackoffMs;
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

    const String payload = "{\"device_id\":\"" + g_deviceId + "\",\"user_id\":\"" + g_userId + "\"}";
    const String topic = String(TOPIC_BASE) + g_deviceId + "/claim";

    logWithDeviceId("[AWS] Publicando claim -> %s\n", payload.c_str());

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
    }
    else
    {
      logWithDeviceId("[AWS] Mensaje MQTT no enviado\n");
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

  void sendHeartbeat() {

      if (!g_mqttConnected) {
          Serial.println("[HEARTBEAT] Saltado (MQTT offline)");
          return;
      }

      String topic = String(TOPIC_BASE) + g_deviceId + "/heartbeat";

      StaticJsonDocument<256> doc;
      doc["mqtt_topic"] = topic;
      doc["client_id"] = g_deviceId;
      doc["wifi_rssi"] = WiFi.RSSI();
      doc["heap_free"] = esp_get_free_heap_size();
      doc["uptime_ms"] = millis();
      doc["fw"] = FW_VERSION;

      char buffer[256];
      size_t len = serializeJson(doc, buffer);

      int mid = esp_mqtt_client_publish(
          g_mqttClient,
          topic.c_str(),
          buffer,
          len,
          1,   // QoS1 real
          0    // retain false
      );

      Serial.printf("[HEARTBEAT] Enviado MID=%d → %s\n", mid, topic.c_str());
  }

  void resetWifiBackoff()
  {
    g_wifiBackoffIndex = 0;
    g_nextWifiAttemptMs = 0;
  }

  void scheduleWifiReconnect(const char *reason)
  {
    incrementDiagCounter(kDiagWifiKey);
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
    logWithDeviceId("[IDENTIDAD] MAC: %s\n", g_macAddress.c_str());
    logWithDeviceId("[IDENTIDAD] user_id: %s\n", userId);
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
    logWithDeviceId("[BLE] Credenciales recibidas para SSID '%s'\n", creds.ssid.c_str());
    Config::setString("wifi", kWifiSsidKey, std::string(creds.ssid.c_str()));
    Config::setString("wifi", kWifiPassKey, std::string(creds.password.c_str()));

    if (creds.deviceId.length() > 0 && creds.deviceId != g_deviceId)
    {
      g_deviceId = creds.deviceId;
      persistDeviceId();
      updateDownlinkTopics();
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
      logWithDeviceId("[BLE] user_id recibido: %s\n", creds.userId.c_str());
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
    if (now - lastLogMs >= 1000)
    {
      logWithDeviceId("[WATCHDOG] fed\n");
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

  void logWatchdogResetIfNeeded()
  {
    const esp_reset_reason_t reason = esp_reset_reason();
    Config::setInt("diag", kDiagResetKey, static_cast<int32_t>(reason));
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
  updateDownlinkTopics();
  loadSettingsFromPrefs();
  loadSetpointsFromPrefs();

  Display::begin();
  Display::setConnectionStatus(false);
  Display::setBleActive(false);
  Display::forceRender();

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

