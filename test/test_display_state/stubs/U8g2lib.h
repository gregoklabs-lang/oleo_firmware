#pragma once

#include <cstdint>
#include <cstring>

#include "Arduino.h"

#define U8X8_PIN_NONE 0xFF

static const uint8_t u8g2_font_6x10_tf = 0;

class U8G2_SSD1306_128X64_NONAME_F_HW_I2C {
 public:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C(int, int, int, int) {}
  void begin() {}
  void setContrast(uint8_t) {}
  void setBusClock(uint32_t) {}
  void drawFrame(int, int, int, int) {}
  void clearBuffer() {}
  void sendBuffer() {}
  void setFont(uint8_t) {}
  int16_t getStrWidth(const char* text) const {
    return text ? static_cast<int16_t>(std::strlen(text)) * 6 : 0;
  }
  void drawStr(int, int, const char*) {}
  void setDrawColor(int) {}
  void drawBox(int, int, int, int) {}
};
