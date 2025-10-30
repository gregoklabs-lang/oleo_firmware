#pragma once

#include <cstdint>

#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 0

void delay(uint32_t ms);
uint32_t millis();
void pinMode(int pin, int mode);
int digitalRead(int pin);

class SerialMock {
 public:
  void begin(unsigned long baud);
  void println(const char* text);
  void println(int value);
  void println(unsigned int value);
  void println(long value);
  void println(unsigned long value);
  void println(float value);
  void println(double value);
  template <typename T>
  void println(const T&) {}

  void print(const char* text);
  void print(int value);
  void print(unsigned int value);
  void print(long value);
  void print(unsigned long value);
  void print(float value);
  void print(double value);
  template <typename T>
  void print(const T&) {}

  void printf(const char* fmt, ...);
};

extern SerialMock Serial;

void arduino_test_set_millis(uint32_t value);
void arduino_test_advance_millis(uint32_t delta);
void arduino_test_set_digital_read(int value);
