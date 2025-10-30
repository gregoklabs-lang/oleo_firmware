#include <unity.h>

#include "Arduino.h"
#include "WiFi.h"

enum DispState : uint8_t;

extern volatile bool pendingFadeToConnected;
extern volatile uint8_t dots;
extern volatile DispState g_dispState;
extern uint32_t lastAnimMs;

void SysProvEvent(arduino_event_t* sys_event);
void loop();

void setUp() {
  pendingFadeToConnected = false;
  dots = 0;
  g_dispState = DISP_CONECTANDO;
  lastAnimMs = 0;
  arduino_test_set_millis(0);
  arduino_test_set_digital_read(HIGH);
}

void tearDown() {}

void test_fade_transitions_to_connected_after_got_ip() {
  arduino_event_t event = {};
  event.event_id = ARDUINO_EVENT_WIFI_STA_GOT_IP;
  event.event_info.got_ip.ip_info.addr = 0x01010101;

  SysProvEvent(&event);
  TEST_ASSERT_TRUE(pendingFadeToConnected);

  arduino_test_set_millis(400);
  loop();

  TEST_ASSERT_FALSE(pendingFadeToConnected);
  TEST_ASSERT_EQUAL_UINT8(DISP_CONECTADO, g_dispState);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fade_transitions_to_connected_after_got_ip);
  return UNITY_END();
}
