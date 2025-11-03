#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <stdarg.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>

#include "oled_display.h"
#include "provisioning.h"

// ======================
// 🔹 CONFIGURACIÓN AWS
// ======================
const char *AWS_IOT_ENDPOINT = "a7xxu98k219gv-ats.iot.us-east-1.amazonaws.com";
const int AWS_IOT_PORT = 8883;

#define ROOT_CA_PATH "/certs/AmazonRootCA1.pem"
#define CERT_PATH "/certs/device.pem.crt"
#define KEY_PATH "/certs/private.pem.key"

WiFiClientSecure net;
PubSubClient mqttClient(net);

namespace
{
  constexpr int kBleButtonPin = 0;
  constexpr uint32_t kBleSessionDurationMs = 60000;
  constexpr uint32_t kWifiConnectTimeoutMs = 15000;
  constexpr uint32_t kButtonDebounceMs = 200;
  constexpr uint32_t kBleActivationHoldMs = 3000;
  constexpr uint32_t kIdentityLogDelayMs = 6000;
  constexpr uint32_t kAwsReconnectDelayMs = 5000;

  enum class SystemState : uint8_t
  {
    WIFI_DISCONNECTED = 0,
    WIFI_CONNECTED,
    BLE_ACTIVE,
  };

  Preferences g_identityPrefs;
  bool g_identityPrefsReady = false;
  constexpr char kPrefsNamespace[] = "identity";
  constexpr char kPrefsDeviceIdKey[] = "device_id";
  constexpr char kPrefsUserIdKey[] = "user_id";

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
  String g_macAddress;
  SystemState g_state = SystemState::WIFI_DISCONNECTED;

  bool g_wifiConnected = false;
  bool g_wifiConnecting = false;
  uint32_t g_wifiConnectStart = 0;

  bool g_bleActive = false;
  uint32_t g_bleStartMs = 0;

  volatile bool g_bleButtonInterrupt = false;
  uint32_t g_lastButtonHandledMs = 0;
  bool g_bleButtonPending = false;
  uint32_t g_bleButtonPressStartMs = 0;
  uint32_t g_identityLogTargetMs = 0;
  bool g_identityLogReady = false;

  // ===== AWS Flags
  bool g_awsConnected = false;
  uint32_t g_lastAwsAttempt = 0;
  bool g_claimPending = false;
  bool g_awsCredentialsLoaded = false;
  bool g_spiffsReady = false;
  String g_rootCaPem;
  String g_deviceCertPem;
  String g_privateKeyPem;

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
      logWithDeviceId("[SETTINGS] ⚠️ No hay configuraciones almacenadas\n");
      return;
    }

    const bool hasSetpointsStored =
        g_setpointsPrefsReady &&
        (g_setpointsPrefs.isKey(kSetpointsPhKey) ||
         g_setpointsPrefs.isKey(kSetpointsEcKey) ||
         g_setpointsPrefs.isKey(kSetpointsTempKey) ||
         g_setpointsPrefs.isKey(kSetpointsFlowKey) ||
         g_setpointsPrefs.isKey(kSetpointsModeKey) ||
         g_setpointsPrefs.isKey(kSetpointsVersionKey) ||
         g_setpointsPrefs.isKey(kSetpointsReservoirKey));

    if (hasSetpointsStored)
    {
      logWithDeviceId("[SETPOINTS] 🔁 Cargados desde NVS:\n");
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
    else
    {
      logWithDeviceId("[SETPOINTS] ⚠️ No hay setpoints almacenados\n");
    }

    logWithDeviceId("[SETTINGS] 🔁 Cargadas desde NVS:\n");
    logWithDeviceId("   - Reservoir units: %s\n", g_downlinkSettings.reservoirUnits.c_str());
    logWithDeviceId("   - Temp units: %s\n", g_downlinkSettings.temperatureUnits.c_str());
    logWithDeviceId("   - Nutrients units: %s\n", g_downlinkSettings.nutrientsUnits.c_str());
    logWithDeviceId("   - Email notifications: %d\n", g_downlinkSettings.emailNotifications ? 1 : 0);
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

    logWithDeviceId("[SETPOINTS] 🔁 Cargados desde NVS:\n");
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

    logWithDeviceId("✅ Nueva configuración recibida:\n");
    logWithDeviceId("   - Reservoir units: %s\n", g_downlinkSettings.reservoirUnits.c_str());
    logWithDeviceId("   - Temp units: %s\n", g_downlinkSettings.temperatureUnits.c_str());
    logWithDeviceId("   - Nutrients units: %s\n", g_downlinkSettings.nutrientsUnits.c_str());
    logWithDeviceId("   - Email notifications: %d\n", g_downlinkSettings.emailNotifications ? 1 : 0);
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

    logWithDeviceId("✅ Setpoints actualizados:\n");
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

  void mqttCallback(char *topic, byte *payload, unsigned int length)
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
    g_rootCaPem = "";
    g_deviceCertPem = "";
    g_privateKeyPem = "";
    g_awsCredentialsLoaded = false;
    g_awsConnected = false;
  }

  bool setupAWS()
  {
    if (!g_spiffsReady)
    {
      Serial.println("[AWS] ❌ SPIFFS no montado");
      clearAwsCredentials();
      return false;
    }

    if (!SPIFFS.exists(ROOT_CA_PATH) || !SPIFFS.exists(CERT_PATH) || !SPIFFS.exists(KEY_PATH))
    {
      Serial.println("[AWS] ❌ Certificados no encontrados en SPIFFS");
      clearAwsCredentials();
      return false;
    }

    File ca = SPIFFS.open(ROOT_CA_PATH, "r");
    File cert = SPIFFS.open(CERT_PATH, "r");
    File key = SPIFFS.open(KEY_PATH, "r");

    if (!ca || !cert || !key)
    {
      Serial.println("[AWS] ❌ No se pudieron abrir los certificados");
      if (ca) ca.close();
      if (cert) cert.close();
      if (key) key.close();
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
      Serial.println("[AWS] ❌ Certificados vacios o corruptos");
      clearAwsCredentials();
      return false;
    }

    net.setCACert(g_rootCaPem.c_str());
    net.setCertificate(g_deviceCertPem.c_str());
    net.setPrivateKey(g_privateKeyPem.c_str());

    mqttClient.setServer(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
    mqttClient.setBufferSize(1024);
    mqttClient.setCallback(mqttCallback);
    g_awsCredentialsLoaded = true;
    logWithDeviceId("[AWS] Configuracion MQTT lista\n");
    return true;
  }

  bool connectAWS()
  {
    if (!g_awsCredentialsLoaded)
    {
      g_awsConnected = false;
      return false;
    }

    if (mqttClient.connected()) return true;

    if (millis() - g_lastAwsAttempt < kAwsReconnectDelayMs) return false;
    g_lastAwsAttempt = millis();

    Serial.print("[AWS] 🔌 Conectando a IoT Core... ");
    if (mqttClient.connect(g_deviceId.c_str()))
    {
      Serial.println("✅ Conectado");
      const char *settingsTopic = g_downlinkSettingsTopic.c_str();
      if (settingsTopic && settingsTopic[0] != '\0')
      {
        if (mqttClient.subscribe(settingsTopic))
        {
          logWithDeviceId("[MQTT] Suscrito a %s\n", settingsTopic);
        }
        else
        {
          logWithDeviceId("[MQTT] ⚠️ Fallo al suscribir %s\n", settingsTopic);
        }
      }
      const char *setpointsTopic = g_downlinkSetpointsTopic.c_str();
      if (setpointsTopic && setpointsTopic[0] != '\0')
      {
        if (mqttClient.subscribe(setpointsTopic))
        {
          logWithDeviceId("[MQTT] Suscrito a %s\n", setpointsTopic);
        }
        else
        {
          logWithDeviceId("[MQTT] ⚠️ Fallo al suscribir %s\n", setpointsTopic);
        }
      }
      g_awsConnected = true;
      return true;
    }
    else
    {
      Serial.printf("❌ Falla MQTT rc=%d\n", mqttClient.state());
      g_awsConnected = false;
      return false;
    }
  }

  void sendProvisioningClaim()
  {
    if (!g_claimPending || !mqttClient.connected())
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
    const String topic = "devices/" + g_deviceId + "/claim";

    logWithDeviceId("[AWS] Publicando claim -> %s\n", payload.c_str());

    if (mqttClient.publish(topic.c_str(), payload.c_str()))
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
    if (!g_wifiConnected)
    {
      g_awsConnected = false;
      return;
    }

    if (!g_awsCredentialsLoaded)
    {
      g_awsConnected = false;
      return;
    }

    if (!mqttClient.connected())
    {
      if (connectAWS())
      {
        sendProvisioningClaim();
      }
    }
    else
    {
      mqttClient.loop();
      sendProvisioningClaim();
    }
  }

  // ========================== (Tu código original sigue igual)
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

  void clearStoredWifiCredentials()
  {
    const bool wasConnected = g_wifiConnected;
    WiFi.disconnect(true, true);
    g_wifiConnecting = false;
    applyWifiConnectionStatus(false);
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

  void startWifiConnection(const char *ssid = nullptr, const char *password = nullptr)
  {
    WiFi.disconnect(false, false);

    if (ssid && password)
    {
      logWithDeviceId("[WIFI] Conectando a '%s'\n", ssid);
      WiFi.begin(ssid, password);
    }
    else
    {
      logWithDeviceId("[WIFI] Conectando con credenciales guardadas\n");
      WiFi.begin();
    }

    g_wifiConnecting = true;
    g_wifiConnectStart = millis();
  }

  bool hasStoredCredentials()
  {
    wifi_config_t config;
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK)
    {
      return false;
    }
    return config.sta.ssid[0] != '\0';
  }

  bool beginIdentityPrefs()
  {
    if (g_identityPrefsReady)
    {
      return true;
    }

    g_identityPrefsReady = g_identityPrefs.begin(kPrefsNamespace, false);
    if (!g_identityPrefsReady)
    {
      Serial.println("[identity] No se pudieron abrir las preferencias");
    }
    return g_identityPrefsReady;
  }

  void persistDeviceId()
  {
    if (!beginIdentityPrefs())
    {
      return;
    }

    g_identityPrefs.putString(kPrefsDeviceIdKey, g_deviceId);
  }

  void loadStoredUserId()
  {
    if (!beginIdentityPrefs())
    {
      g_userId = "";
      return;
    }

    g_userId = g_identityPrefs.getString(kPrefsUserIdKey, "");
  }

  void storeUserId(const String &userId)
  {
    if (!beginIdentityPrefs())
    {
      return;
    }

    g_userId = userId;
    g_identityPrefs.putString(kPrefsUserIdKey, g_userId);
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

    String id = "OLEO_";
    id += suffix;
    return id;
  }

  void onProvisionedCredentials(const String &ssid, const String &password, const String &userId)
  {
    logWithDeviceId("[BLE] Credenciales recibidas para SSID '%s'\n", ssid.c_str());
    Provisioning::notifyStatus("wifi:conectando");
    applyWifiConnectionStatus(false);
    startWifiConnection(ssid.c_str(), password.c_str());

    if (userId.length() > 0)
    {
      storeUserId(userId);
      scheduleIdentityLog();
      logWithDeviceId("[BLE] user_id recibido: %s\n", userId.c_str());
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
      }
      g_wifiConnecting = false;
      return;
    }

    if (g_wifiConnected)
    {
      logWithDeviceId("[WIFI] Conexion perdida\n");
      applyWifiConnectionStatus(false);
      Provisioning::notifyStatus("wifi:desconectado");
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
    }
  }

  void configureButton()
  {
    pinMode(kBleButtonPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(kBleButtonPin), onBleButtonPressed,
                    FALLING);
  }

} // namespace

void setup()
{
  Serial.begin(115200);

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

  g_deviceId = buildDeviceId();
  persistDeviceId();
  loadStoredUserId();
  scheduleIdentityLog();
  logWithDeviceId("[BOOT] device_id: %s\n", g_deviceId.c_str());
  g_downlinkSettingsTopic = "devices/" + g_deviceId + "/downlink/settings";
  g_downlinkSetpointsTopic = "devices/" + g_deviceId + "/downlink/setpoints";
  loadSetpointsFromPrefs();
  loadSettingsFromPrefs();

  Display::begin();
  Display::setConnectionStatus(false);
  Display::setBleActive(false);
  Display::forceRender();

  Provisioning::begin(g_deviceId, onProvisionedCredentials);

  configureButton();

  if (hasStoredCredentials())
  {
    logWithDeviceId("[WIFI] Credenciales guardadas detectadas\n");
    startWifiConnection();
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
