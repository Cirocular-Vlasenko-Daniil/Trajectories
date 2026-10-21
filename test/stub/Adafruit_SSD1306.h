#pragma once
#include "Arduino.h"
#include "Wire.h"
#define SSD1306_BLACK 0
#define SSD1306_WHITE 1
#define SSD1306_SWITCHCAPVCC 2
class Adafruit_SSD1306 {
public:
  Adafruit_SSD1306(int, int, TwoWire *, int) {}
  bool begin(int, int);
  void clearDisplay();
  void display();
  void setTextSize(uint8_t);
  void setTextColor(uint16_t);
  void setCursor(int16_t, int16_t);
  void print(const char *);
  void print(char);
  void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t);
  void drawRect(int16_t, int16_t, int16_t, int16_t, uint16_t);
  void drawPixel(int16_t, int16_t, uint16_t);
  void drawFastHLine(int16_t, int16_t, int16_t, uint16_t);
  void getTextBounds(const char *, int16_t, int16_t, int16_t *, int16_t *, uint16_t *, uint16_t *);
  uint8_t *getBuffer();
};
