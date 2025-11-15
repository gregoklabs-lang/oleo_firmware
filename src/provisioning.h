#pragma once

#include <Arduino.h>

#include <functional>

namespace Provisioning {
struct CredentialsData
{
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
};

using CredentialsCallback = std::function<void(const CredentialsData &)>;

void begin(const String &deviceId, CredentialsCallback callback);

bool startBle();

void stopBle();

bool isActive();

bool isProvisioningAllowed();

void notifyStatus(const String &message);

void loop();
}

