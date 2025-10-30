#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <stdarg.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
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

  bool setupAWS()
  {
    if (!SPIFFS.begin(true))
    {
      Serial.println("[SPIFFS] ❌ Error al montar");
      return false;
    }

    if (!SPIFFS.exists(ROOT_CA_PATH) || !SPIFFS.exists(CERT_PATH) || !SPIFFS.exists(KEY_PATH))
    {
      Serial.println("[AWS] ❌ Certificados no encontrados en SPIFFS");
      return false;
    }

    File ca = SPIFFS.open(ROOT_CA_PATH, "r");
    File cert = SPIFFS.open(CERT_PATH, "r");
    File key = SPIFFS.open(KEY_PATH, "r");

    if (!ca || !cert || !key)
    {
      Serial.println("[AWS] ❌ No se pudieron abrir los certificados");
      return false;
    }

    String rootCA = ca.readString();
    String deviceCert = cert.readString();
    String privateKey = key.readString();

    ca.close();
    cert.close();
    key.close();

    net.setCACert(rootCA.c_str());
    net.setCertificate(deviceCert.c_str());
    net.setPrivateKey(privateKey.c_str());

    mqttClient.setServer(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
    mqttClient.setBufferSize(1024);
    logWithDeviceId("[AWS] Configuracion MQTT lista\n");
    return true;
  }

  bool connectAWS()
  {
    if (mqttClient.connected()) return true;

    if (millis() - g_lastAwsAttempt < kAwsReconnectDelayMs) return false;
    g_lastAwsAttempt = millis();

    Serial.print("[AWS] 🔌 Conectando a IoT Core... ");
    if (mqttClient.connect(g_deviceId.c_str()))
    {
      Serial.println("✅ Conectado");
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
        setupAWS();     
        connectAWS();   // 👈 Se conecta a AWS inmediatamente
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

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  g_deviceId = buildDeviceId();
  persistDeviceId();
  loadStoredUserId();
  scheduleIdentityLog();
  logWithDeviceId("[BOOT] device_id: %s\n", g_deviceId.c_str());

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
