#include "Arduino.h"

#include <cstdarg>

namespace {
uint32_t g_fakeMillis = 0;
int g_digitalValue = HIGH;
}

SerialMock Serial;

void SerialMock::begin(unsigned long) {}
void SerialMock::println(const char*) {}
void SerialMock::println(int) {}
void SerialMock::println(unsigned int) {}
void SerialMock::println(long) {}
void SerialMock::println(unsigned long) {}
void SerialMock::println(float) {}
void SerialMock::println(double) {}
void SerialMock::print(const char*) {}
void SerialMock::print(int) {}
void SerialMock::print(unsigned int) {}
void SerialMock::print(long) {}
void SerialMock::print(unsigned long) {}
void SerialMock::print(float) {}
void SerialMock::print(double) {}
void SerialMock::printf(const char*, ...) {}

uint32_t millis() { return g_fakeMillis; }

void delay(uint32_t ms) { g_fakeMillis += ms; }

void pinMode(int, int) {}

int digitalRead(int) { return g_digitalValue; }

void arduino_test_set_millis(uint32_t value) { g_fakeMillis = value; }

void arduino_test_advance_millis(uint32_t delta) { g_fakeMillis += delta; }

void arduino_test_set_digital_read(int value) { g_digitalValue = value; }
