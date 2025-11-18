#include "provisioning.h"

#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"

#include "Config.hpp"

namespace Provisioning
{
  namespace
  {
    constexpr char kServiceUuid[] = "12345678-1234-1234-1234-1234567890ab";
    constexpr char kCharacteristicUuid[] = "87654321-4321-4321-4321-0987654321ba";
    constexpr uint16_t kInvalidConnId = 0xFFFF;

    BLEServer *g_server = nullptr;
    BLECharacteristic *g_characteristic = nullptr;
    BLEAdvertising *g_advertising = nullptr;
    CredentialsCallback g_callback;
    String g_deviceId;
    bool g_initialized = false;
    bool g_sessionActive = false;
    bool g_centralConnected = false;
    uint16_t g_connId = kInvalidConnId;
    volatile bool g_restartAdvertising = false;
    bool g_bleDeviceInitialized = false;
    uint32_t g_bootMillis = 0;
    bool g_windowWarningLogged = false;

    constexpr size_t kMaxNotifyLength = 64;
    constexpr size_t kMaxSsidLength = 33;     // 32 + null
    constexpr size_t kMaxPasswordLength = 65; // 64 + null (WPA2 max)
    constexpr size_t kMaxUserIdLength = 65;   // accommodate UUID or custom id
    constexpr size_t kMaxDeviceIdLength = 65;
    constexpr size_t kMaxEndpointLength = 129;
    constexpr size_t kMaxRegionLength = 33;
    constexpr size_t kMaxEnvLength = 17;
    constexpr size_t kMaxThingNameLength = 65;
    constexpr size_t kMaxProvisionTokenLength = 65;
    constexpr uint32_t kProvisioningWindowMs = 10UL * 60UL * 1000UL;
    constexpr int kProvisioningButtonPin = 0;

    struct NotifyPayload
    {
      char message[kMaxNotifyLength];
    };

    struct CredentialsPayload
    {
      char ssid[kMaxSsidLength];
      char password[kMaxPasswordLength];
      char userId[kMaxUserIdLength];
      char deviceId[kMaxDeviceIdLength];
      char endpoint[kMaxEndpointLength];
      char region[kMaxRegionLength];
      char environment[kMaxEnvLength];
      char thingName[kMaxThingNameLength];
      char provisionToken[kMaxProvisionTokenLength];
      int32_t awsPort;
    };

    portMUX_TYPE g_queueMux = portMUX_INITIALIZER_UNLOCKED;
    volatile bool g_pendingNotify = false;
    volatile bool g_pendingCredentials = false;
    NotifyPayload g_notifyBuffer = {};
    CredentialsPayload g_credentialsBuffer = {};

    struct ParsedCredentials
    {
      bool valid = false;
      String ssid;
      String password;
      String userId;
      String deviceId;
      String endpoint;
      String region;
      String environment;
      String thingName;
      String provisionToken;
      int32_t awsPort = 0;
      bool awsPortProvided = false;
      String error;
    };

    bool isWhitespace(char c)
    {
      return c == '\r' || c == '\n' || c == '\t' || c == ' ';
    }

    void trim(std::string &text)
    {
      while (!text.empty() && isWhitespace(text.front()))
      {
        text.erase(text.begin());
      }
      while (!text.empty() && isWhitespace(text.back()))
      {
        text.pop_back();
      }
    }

    std::vector<std::string> splitTokens(const std::string &payload)
    {
      std::vector<std::string> tokens;
      size_t start = 0;
      while (start < payload.size())
      {
        size_t end = payload.find('\n', start);
        if (end == std::string::npos)
        {
          end = payload.size();
        }
        tokens.emplace_back(payload.substr(start, end - start));
        start = end + 1;
      }
      return tokens;
    }

    bool isValidDeviceId(const String &id)
    {
      if (id.isEmpty())
      {
        return true;
      }
      if (id.length() > 32)
      {
        return false;
      }
      for (size_t i = 0; i < id.length(); ++i)
      {
        const char c = id[i];
        const bool allowed = (c >= '0' && c <= '9') ||
                             (c >= 'a' && c <= 'z') ||
                             (c >= 'A' && c <= 'Z') ||
                             c == '-' || c == '_';
        if (!allowed)
        {
          return false;
        }
      }
      return true;
    }

    bool isValidPort(int32_t port)
    {
      return port > 0 && port < 65536;
    }

    bool isValidLength(const String &value, size_t maxLen)
    {
      return value.length() <= maxLen;
    }

    ParsedCredentials parseCredentials(const std::string &raw)
    {
      ParsedCredentials result;
      if (raw.empty())
      {
        result.error = "vacio";
        return result;
      }

      std::string payload = raw;
      payload.erase(std::remove(payload.begin(), payload.end(), '\r'), payload.end());
      std::replace(payload.begin(), payload.end(), '|', '\n');

      const bool keyValueMode = payload.find('=') != std::string::npos;
      std::vector<std::string> tokens = splitTokens(payload);

      if (keyValueMode)
      {
        for (std::string token : tokens)
        {
          trim(token);
          if (token.empty())
          {
            continue;
          }
          size_t eq = token.find('=');
          if (eq == std::string::npos)
          {
            continue;
          }

          std::string key(token.begin(), token.begin() + eq);
          std::string value(token.begin() + eq + 1, token.end());
          trim(key);
          trim(value);
          if (key.empty())
          {
            continue;
          }
          std::transform(key.begin(), key.end(), key.begin(),
                         [](unsigned char c)
                         { return static_cast<char>(std::tolower(c)); });
          String valueStr(value.c_str());

          if (key == "ssid" || key == "wifi_ssid")
          {
            result.ssid = valueStr;
          }
          else if (key == "password" || key == "pass" || key == "wifi_password")
          {
            result.password = valueStr;
          }
          else if (key == "user_id" || key == "userid")
          {
            result.userId = valueStr;
          }
          else if (key == "device_id")
          {
            result.deviceId = valueStr;
          }
          else if (key == "endpoint" || key == "aws_endpoint")
          {
            result.endpoint = valueStr;
          }
          else if (key == "region" || key == "aws_region")
          {
            result.region = valueStr;
          }
          else if (key == "env" || key == "environment")
          {
            result.environment = valueStr;
          }
          else if (key == "thing" || key == "thingname" || key == "thing_name")
          {
            result.thingName = valueStr;
          }
          else if (key == "token" || key == "provision_token")
          {
            result.provisionToken = valueStr;
          }
          else if (key == "aws_port" || key == "port")
          {
            result.awsPort = atoi(value.c_str());
            result.awsPortProvided = true;
          }
        }
      }
      else
      {
        if (!tokens.empty())
        {
          std::string ssid(tokens[0]);
          trim(ssid);
          result.ssid = String(ssid.c_str());
        }
        if (tokens.size() > 1)
        {
          std::string password(tokens[1]);
          trim(password);
          result.password = String(password.c_str());
        }
        if (tokens.size() > 2)
        {
          std::string user(tokens[2]);
          trim(user);
          result.userId = String(user.c_str());
        }
      }

      if (result.ssid.isEmpty())
      {
        result.error = "ssid";
        return result;
      }

      if (!isValidLength(result.ssid, 128))
      {
        result.error = "ssid_len";
        return result;
      }

      if (!isValidLength(result.password, 128))
      {
        result.error = "password_len";
        return result;
      }

      if (!isValidDeviceId(result.deviceId))
      {
        result.error = "device_id";
        return result;
      }

      if (result.awsPortProvided && !isValidPort(result.awsPort))
      {
        result.error = "aws_port";
        return result;
      }

      result.valid = true;
      return result;
    }

    void queueNotify(const char *message)
    {
      if (!message)
      {
        return;
      }
      portENTER_CRITICAL(&g_queueMux);
      strncpy(g_notifyBuffer.message, message, sizeof(g_notifyBuffer.message) - 1);
      g_notifyBuffer.message[sizeof(g_notifyBuffer.message) - 1] = '\0';
      g_pendingNotify = true;
      portEXIT_CRITICAL(&g_queueMux);
    }

    void queueCredentials(const CredentialsPayload &payload)
    {
      portENTER_CRITICAL(&g_queueMux);
      g_credentialsBuffer = payload;
      g_pendingCredentials = true;
      portEXIT_CRITICAL(&g_queueMux);
    }

    void notify(const String &message)
    {
      if (!g_characteristic)
      {
        return;
      }
      g_characteristic->setValue(message.c_str());
      if (g_centralConnected)
      {
        g_characteristic->notify();
      }
    }

    class ProvisioningCallbacks : public BLECharacteristicCallbacks
    {
      void onWrite(BLECharacteristic *characteristic) override
      {
        const std::string value = characteristic->getValue();
        ParsedCredentials credentials = parseCredentials(value);

        if (!credentials.valid)
        {
          char message[kMaxNotifyLength];
          snprintf(message, sizeof(message), "error:%s", credentials.error.c_str());
          queueNotify(message);
          return;
        }

        queueNotify("credenciales");

        CredentialsPayload payload = {};
        snprintf(payload.ssid, sizeof(payload.ssid), "%s", credentials.ssid.c_str());
        snprintf(payload.password, sizeof(payload.password), "%s", credentials.password.c_str());
        snprintf(payload.userId, sizeof(payload.userId), "%s", credentials.userId.c_str());
        snprintf(payload.deviceId, sizeof(payload.deviceId), "%s", credentials.deviceId.c_str());
        snprintf(payload.endpoint, sizeof(payload.endpoint), "%s", credentials.endpoint.c_str());
        snprintf(payload.region, sizeof(payload.region), "%s", credentials.region.c_str());
        snprintf(payload.environment, sizeof(payload.environment), "%s", credentials.environment.c_str());
        snprintf(payload.thingName, sizeof(payload.thingName), "%s", credentials.thingName.c_str());
        payload.awsPort = credentials.awsPort;
        queueCredentials(payload);
      }
    };

    class ServerCallbacks : public BLEServerCallbacks
    {
      void onConnect(BLEServer *server) override
      {
        g_centralConnected = true;
        g_connId = server->getConnId();
      }

      void onDisconnect(BLEServer *server) override
      {
        g_centralConnected = false;
        g_connId = kInvalidConnId;
        if (g_sessionActive)
        {
          g_restartAdvertising = true;
        }
      }
    };

    void configureAdvertising()
    {
      if (!g_advertising)
      {
        g_advertising = BLEDevice::getAdvertising();
      }

      if (!g_advertising)
      {
        return;
      }

      BLEAdvertisementData advData;
      advData.setName(g_deviceId.c_str());
      advData.setCompleteServices(BLEUUID(kServiceUuid));

      g_advertising->setAdvertisementData(advData);
      g_advertising->setScanResponse(false);
      g_advertising->setMinPreferred(0x06);
      g_advertising->setMaxPreferred(0x12);
    }

    void ensureInitialized(const String &deviceId)
    {
      if (g_bootMillis == 0)
      {
        g_bootMillis = millis();
      }

      g_deviceId = deviceId;
      if (!g_bleDeviceInitialized)
      {
        BLEDevice::init(g_deviceId.c_str());
        g_bleDeviceInitialized = true;
      }
      else
      {
      }

      if (!g_server)
      {
        g_server = BLEDevice::createServer();
        g_server->setCallbacks(new ServerCallbacks());

        BLEService *service = g_server->createService(kServiceUuid);
        g_characteristic = service->createCharacteristic(
            kCharacteristicUuid,
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
                BLECharacteristic::PROPERTY_NOTIFY);
        g_characteristic->addDescriptor(new BLE2902());
        g_characteristic->setCallbacks(new ProvisioningCallbacks());
        g_characteristic->setValue("inactivo");

        service->start();

        g_advertising = BLEDevice::getAdvertising();
        if (g_advertising)
        {
          g_advertising->addServiceUUID(kServiceUuid);
        }

        g_initialized = true;
      }

      configureAdvertising();
    }
  } // namespace

  void begin(const String &deviceId, CredentialsCallback callback)
  {
    g_callback = callback;
    ensureInitialized(deviceId);
    notify("inactivo");
  }

  bool startBle()
  {
    if (!g_initialized || !g_advertising)
    {
      return false;
    }

    configureAdvertising();
    g_advertising->start();
    notify("activo");
    g_sessionActive = true;
    g_restartAdvertising = false;
    return true;
  }

  void stopBle()
  {
    if (!g_initialized)
    {
      return;
    }

    if (g_advertising)
    {
      g_advertising->stop();
    }

    if (g_centralConnected && g_server && g_connId != kInvalidConnId)
    {
      g_server->disconnect(g_connId);
    }

    g_sessionActive = false;
    g_restartAdvertising = false;
    notify("inactivo");
  }

  bool isActive() { return g_sessionActive; }

  bool isProvisioningAllowed()
  {
    if (g_bootMillis == 0)
    {
      g_bootMillis = millis();
    }
    const uint32_t elapsed = millis() - g_bootMillis;
    const bool withinWindow = elapsed <= kProvisioningWindowMs;
    const bool buttonPressed = digitalRead(kProvisioningButtonPin) == LOW;
    if (withinWindow || buttonPressed)
    {
      g_windowWarningLogged = false;
      return true;
    }
    if (!g_windowWarningLogged)
    {
      Serial.println("[BLE] Provisioning no permitido (fuera de ventana)");
      g_windowWarningLogged = true;
    }
    return false;
  }

  void notifyStatus(const String &message) { notify(message); }

  void loop()
  {
    if (g_pendingNotify)
    {
      NotifyPayload payload = {};
      portENTER_CRITICAL(&g_queueMux);
      if (g_pendingNotify)
      {
        payload = g_notifyBuffer;
        g_pendingNotify = false;
      }
      portEXIT_CRITICAL(&g_queueMux);
      if (payload.message[0])
      {
        notify(String(payload.message));
      }
    }

    if (g_pendingCredentials && g_callback)
    {
      CredentialsPayload payload = {};
      portENTER_CRITICAL(&g_queueMux);
      if (g_pendingCredentials)
      {
        payload = g_credentialsBuffer;
        g_pendingCredentials = false;
      }
      portEXIT_CRITICAL(&g_queueMux);
      if (payload.ssid[0])
      {
        CredentialsData data;
        data.ssid = String(payload.ssid);
        data.password = String(payload.password);
        data.userId = String(payload.userId);
        data.deviceId = String(payload.deviceId);
        data.endpoint = String(payload.endpoint);
        data.region = String(payload.region);
        data.environment = String(payload.environment);
        data.thingName = String(payload.thingName);
        data.provisionToken = String(payload.provisionToken);
        data.awsPort = payload.awsPort;
        g_callback(data);
      }
    }

    if (g_restartAdvertising && g_sessionActive && g_advertising)
    {
      g_restartAdvertising = false;
      g_advertising->start();
    }
  }

} // namespace Provisioning
