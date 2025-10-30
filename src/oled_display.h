#pragma once

#include <Arduino.h>

namespace Display {
void begin();

void setConnectionStatus(bool connected);

void setBleActive(bool active);

void forceRender();

void loop();
}

