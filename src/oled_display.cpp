#include "oled_display.h"

#include <U8g2lib.h>
#include <algorithm>

namespace {
constexpr int kWidth = 72;
constexpr int kHeight = 39;
constexpr int kXOffset = 28;
constexpr int kYOffset = 25;
constexpr uint32_t kBlinkIntervalMs = 600;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

bool g_wifiConnected = false;
bool g_bleActive = false;
bool g_blinkOn = true;
bool g_dirty = true;
uint32_t g_lastBlinkMs = 0;

void drawFrame() { g_u8g2.drawFrame(kXOffset, kYOffset, kWidth, kHeight); }

void render() {
  const int16_t centerX = kXOffset + (kWidth / 2);
  const int16_t centerY = kYOffset + (kHeight / 2);
  const int16_t circleRadius = (std::min(kWidth, kHeight) / 2) - 8;
  const int16_t fillRadius = std::max<int16_t>(0, circleRadius - 1);

  g_u8g2.clearBuffer();
  drawFrame();

  bool shouldFill = false;
  if (g_bleActive) {
    shouldFill = g_blinkOn;
  } else {
    shouldFill = g_wifiConnected;
  }
  if (shouldFill && fillRadius > 0) {
    g_u8g2.drawDisc(centerX, centerY, fillRadius);
  }

  g_u8g2.drawCircle(centerX, centerY, circleRadius);

  g_u8g2.sendBuffer();
}
}  // namespace

namespace Display {
void begin() {
  g_u8g2.begin();
  g_u8g2.setContrast(255);
  g_u8g2.setBusClock(400000);
  g_lastBlinkMs = millis();
  g_dirty = true;
}

void setConnectionStatus(bool connected) {
  if (g_wifiConnected != connected) {
    g_wifiConnected = connected;
    g_dirty = true;
  }
}

void setBleActive(bool active) {
  if (g_bleActive != active) {
    g_bleActive = active;
    g_blinkOn = true;
    g_lastBlinkMs = millis();
    g_dirty = true;
  }
}

void forceRender() {
  g_dirty = true;
  render();
  g_dirty = false;
  g_lastBlinkMs = millis();
}

void loop() {
  const uint32_t now = millis();

  if (g_bleActive && (now - g_lastBlinkMs >= kBlinkIntervalMs)) {
    g_lastBlinkMs = now;
    g_blinkOn = !g_blinkOn;
    g_dirty = true;
  }

  if (g_dirty) {
    render();
    g_dirty = false;
  }
}
}  // namespace Display

