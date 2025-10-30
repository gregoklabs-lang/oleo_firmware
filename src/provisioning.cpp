#include "provisioning.h"

#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace Provisioning {
namespace {
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

constexpr size_t kProvisioningQueueLength = 6;
constexpr size_t kMaxNotifyLength = 64;
constexpr size_t kMaxSsidLength = 33;    // 32 + null
constexpr size_t kMaxPasswordLength = 65; // 64 + null (WPA2 max)
constexpr size_t kMaxUserIdLength = 65;  // accommodate UUID or custom id

enum class ProvisioningEventType : uint8_t
{
  Notify = 0,
  Credentials,
};

struct NotifyPayload
{
  char message[kMaxNotifyLength];
};

struct CredentialsPayload
{
  char ssid[kMaxSsidLength];
  char password[kMaxPasswordLength];
  char userId[kMaxUserIdLength];
};

struct ProvisioningEvent
{
  ProvisioningEventType type;
  union
  {
    NotifyPayload notify;
    CredentialsPayload credentials;
  } data;
};

QueueHandle_t g_eventQueue = nullptr;

struct ParsedCredentials {
  bool valid = false;
  String ssid;
  String password;
  String userId;
  String error;
};

bool isWhitespace(char c) {
  return c == '\r' || c == '\n' || c == '\t' || c == ' ';
}

void trim(std::string &text) {
  while (!text.empty() && isWhitespace(text.front())) {
    text.erase(text.begin());
  }
  while (!text.empty() && isWhitespace(text.back())) {
    text.pop_back();
  }
}

ParsedCredentials parseCredentials(const std::string &raw) {
  ParsedCredentials result;
  if (raw.empty()) {
    result.error = "vacio";
    return result;
  }

  std::string payload = raw;
  payload.erase(std::remove(payload.begin(), payload.end(), '\r'), payload.end());

  size_t firstSeparator = payload.find('\n');
  char separatorChar = '\n';
  if (firstSeparator == std::string::npos) {
    firstSeparator = payload.find('|');
    separatorChar = '|';
  }

  if (firstSeparator == std::string::npos) {
    result.error = "formato";
    return result;
  }

  std::string ssid(payload.begin(), payload.begin() + firstSeparator);

  size_t secondSeparator = payload.find(separatorChar, firstSeparator + 1);
  std::string password;
  std::string userId;
  if (secondSeparator == std::string::npos) {
    password.assign(payload.begin() + firstSeparator + 1, payload.end());
  } else {
    password.assign(payload.begin() + firstSeparator + 1,
                    payload.begin() + secondSeparator);
    userId.assign(payload.begin() + secondSeparator + 1, payload.end());
  }

  trim(ssid);
  trim(password);
  trim(userId);

  if (ssid.empty()) {
    result.error = "ssid";
    return result;
  }

  result.valid = true;
  result.ssid = String(ssid.c_str());
  result.password = String(password.c_str());
  result.userId = String(userId.c_str());
  return result;
}

void enqueueEvent(const ProvisioningEvent &event)
{
  if (!g_eventQueue)
  {
    return;
  }
  xQueueSend(g_eventQueue, &event, 0);
}

void notify(const String &message) {
  if (!g_characteristic) {
    return;
  }
  g_characteristic->setValue(message.c_str());
  if (g_centralConnected) {
    g_characteristic->notify();
  }
}

class ProvisioningCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const std::string value = characteristic->getValue();
    ParsedCredentials credentials = parseCredentials(value);

    if (!credentials.valid) {
      ProvisioningEvent errorEvent;
      errorEvent.type = ProvisioningEventType::Notify;
      snprintf(errorEvent.data.notify.message,
               sizeof(errorEvent.data.notify.message),
               "error:%s", credentials.error.c_str());
      enqueueEvent(errorEvent);
      return;
    }

    ProvisioningEvent ackEvent;
    ackEvent.type = ProvisioningEventType::Notify;
    snprintf(ackEvent.data.notify.message,
             sizeof(ackEvent.data.notify.message),
             "credenciales");
    enqueueEvent(ackEvent);

    ProvisioningEvent credEvent;
    credEvent.type = ProvisioningEventType::Credentials;
    snprintf(credEvent.data.credentials.ssid,
             sizeof(credEvent.data.credentials.ssid),
             "%s", credentials.ssid.c_str());
    snprintf(credEvent.data.credentials.password,
             sizeof(credEvent.data.credentials.password),
             "%s", credentials.password.c_str());
    snprintf(credEvent.data.credentials.userId,
             sizeof(credEvent.data.credentials.userId),
             "%s", credentials.userId.c_str());

    if (xQueueSend(g_eventQueue, &credEvent, 0) != pdTRUE) {
      ProvisioningEvent queueErrorEvent;
      queueErrorEvent.type = ProvisioningEventType::Notify;
      snprintf(queueErrorEvent.data.notify.message,
               sizeof(queueErrorEvent.data.notify.message),
               "error:cola");
      enqueueEvent(queueErrorEvent);
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    g_centralConnected = true;
    g_connId = server->getConnId();
  }

  void onDisconnect(BLEServer *server) override {
    g_centralConnected = false;
    g_connId = kInvalidConnId;
    if (g_sessionActive) {
      g_restartAdvertising = true;
    }
  }
};

void configureAdvertising() {
  if (!g_advertising) {
    g_advertising = BLEDevice::getAdvertising();
  }

  if (!g_advertising) {
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

void ensureInitialized(const String &deviceId) {
  if (g_initialized) {
    g_deviceId = deviceId;
    configureAdvertising();
    return;
  }

  g_deviceId = deviceId;
  BLEDevice::init(g_deviceId.c_str());

  if (!g_eventQueue)
  {
    g_eventQueue = xQueueCreate(kProvisioningQueueLength, sizeof(ProvisioningEvent));
  }

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
  if (g_advertising) {
    g_advertising->addServiceUUID(kServiceUuid);
  }

  configureAdvertising();
  g_initialized = true;
}
}  // namespace

void begin(const String &deviceId, CredentialsCallback callback) {
  g_callback = callback;
  ensureInitialized(deviceId);
  notify("inactivo");
}

bool startBle() {
  if (!g_initialized || !g_advertising) {
    return false;
  }

  configureAdvertising();
  g_advertising->start();
  notify("activo");
  g_sessionActive = true;
  g_restartAdvertising = false;
  return true;
}

void stopBle() {
  if (!g_initialized) {
    return;
  }

  if (g_advertising) {
    g_advertising->stop();
  }

  if (g_centralConnected && g_server && g_connId != kInvalidConnId) {
    g_server->disconnect(g_connId);
  }

  g_sessionActive = false;
  g_restartAdvertising = false;
  notify("inactivo");
}

bool isActive() { return g_sessionActive; }

void notifyStatus(const String &message) { notify(message); }

void loop() {
  if (g_eventQueue)
  {
    ProvisioningEvent event;
    while (xQueueReceive(g_eventQueue, &event, 0) == pdTRUE)
    {
      switch (event.type)
      {
      case ProvisioningEventType::Notify:
      {
        String message(event.data.notify.message);
        notify(message);
        break;
      }
      case ProvisioningEventType::Credentials:
      {
        if (g_callback)
        {
          String ssid(event.data.credentials.ssid);
          String password(event.data.credentials.password);
          String userId(event.data.credentials.userId);
          g_callback(ssid, password, userId);
        }
        break;
      }
      default:
        break;
      }
    }
  }

  if (g_restartAdvertising && g_sessionActive && g_advertising) {
    g_restartAdvertising = false;
    g_advertising->start();
  }
}

}  // namespace Provisioning

