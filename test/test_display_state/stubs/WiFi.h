#pragma once

#include <cstdint>
#include <functional>

#include "Arduino.h"

#define WIFI_MODE_STA 1
#define WIFI_MODE_NULL 0

#define WL_CONNECTED 3
#define WL_DISCONNECTED 6

class IPAddress {
 public:
  explicit IPAddress(uint32_t addr = 0) : addr_(addr) {}
  uint32_t addr() const { return addr_; }

 private:
  uint32_t addr_;
};

typedef enum {
  ARDUINO_EVENT_WIFI_STA_CONNECTED = 0,
  ARDUINO_EVENT_WIFI_STA_GOT_IP,
  ARDUINO_EVENT_WIFI_STA_DISCONNECTED,
  ARDUINO_EVENT_PROV_START,
  ARDUINO_EVENT_PROV_CRED_RECV,
  ARDUINO_EVENT_PROV_CRED_FAIL,
  ARDUINO_EVENT_PROV_CRED_SUCCESS,
  ARDUINO_EVENT_PROV_END,
} arduino_event_id_t;

struct arduino_event_info_t {
  struct {
    struct {
      uint32_t addr;
    } ip_info;
  } got_ip;
  struct {
    int reason;
  } wifi_sta_disconnected;
  struct {
    const char* ssid;
    const char* password;
  } prov_cred_recv;
  int prov_fail_reason = 0;
};

struct arduino_event_t {
  arduino_event_id_t event_id;
  arduino_event_info_t event_info;
};

class WiFiClass {
 public:
  using EventHandler = std::function<void(arduino_event_t*)>;

  void onEvent(EventHandler handler);
  void mode(int mode);
  void begin();
  int status() const;
  void setStatus(int status);
  void disconnect(bool wifioff, bool erase);
  const char* SSID() const;
  void setSSID(const char* ssid);
  IPAddress localIP() const;
  void setLocalIP(IPAddress ip);

 private:
  EventHandler handler_ = nullptr;
  int status_ = WL_DISCONNECTED;
  const char* ssid_ = "";
  IPAddress ip_{};
};

extern WiFiClass WiFi;
