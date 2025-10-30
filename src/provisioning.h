#pragma once

#include <Arduino.h>

#include <functional>

namespace Provisioning {
using CredentialsCallback =
    std::function<void(const String &, const String &, const String &)>;

void begin(const String &deviceId, CredentialsCallback callback);

bool startBle();

void stopBle();

bool isActive();

void notifyStatus(const String &message);

void loop();
}

