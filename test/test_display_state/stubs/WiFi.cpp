#include "WiFi.h"

WiFiClass WiFi;

void WiFiClass::onEvent(EventHandler handler) { handler_ = handler; }

void WiFiClass::mode(int) {}

void WiFiClass::begin() {}

int WiFiClass::status() const { return status_; }

void WiFiClass::setStatus(int status) { status_ = status; }

void WiFiClass::disconnect(bool, bool) { status_ = WL_DISCONNECTED; }

const char* WiFiClass::SSID() const { return ssid_; }

void WiFiClass::setSSID(const char* ssid) { ssid_ = ssid; }

IPAddress WiFiClass::localIP() const { return ip_; }

void WiFiClass::setLocalIP(IPAddress ip) { ip_ = ip; }
