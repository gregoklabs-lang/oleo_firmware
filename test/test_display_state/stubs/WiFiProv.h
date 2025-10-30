#pragma once

#include "Arduino.h"

#define WIFI_PROV_SCHEME_BLE 0
#define WIFI_PROV_SCHEME_HANDLER_FREE_BTDM 0
#define WIFI_PROV_SECURITY_1 1

class WiFiProvClass {
 public:
  void beginProvision(int, int, int, const char*, const char*, const char*) {}
  void printQR(const char*, const char*, const char*) {}
};

extern WiFiProvClass WiFiProv;
