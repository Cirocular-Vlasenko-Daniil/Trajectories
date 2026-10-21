// Заглушки Arduino для прогона прошивки на хосте. Только то, что
// Trajectories действительно вызывает, — список снят grep'ом по trajectories.cpp.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>
#include <cstddef>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define SERIAL_8N1 0x800001c
#define RISING 3
#define IRAM_ATTR
#define CHANGE 1
#ifndef PI
#define PI 3.14159265358979323846f
#endif
#define TWO_PI (2.0f * PI)

unsigned long millis();
unsigned long micros();
void delay(unsigned long);
void pinMode(uint8_t, uint8_t);
int  digitalRead(uint8_t);
int  analogRead(uint8_t);
void randomSeed(unsigned long);
long random(long);
long random(long, long);
void attachInterrupt(int, void (*)(), int);
int  digitalPinToInterrupt(uint8_t);
void noInterrupts();
void interrupts();

using String = std::string;

class HardwareSerial {
public:
  HardwareSerial(int = 0) {}
  void begin(unsigned long, int = 0, int = -1, int = -1);
  int  available();
  int  availableForWrite();
  int  read();
  void write(uint8_t);
  size_t write(const uint8_t *, size_t);
};
extern HardwareSerial Serial;
