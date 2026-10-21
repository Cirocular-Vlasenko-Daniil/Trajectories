// Реализация заглушек: часы, кнопки, фреймбуфер, перехват MIDI и
// настоящее NVS-хранилище. Один файл на все тесты — два разных стенда
// однажды уже разъехались, и половина проверок молча перестала что-то
// значить.
#include "Arduino.h"
#include "Adafruit_SSD1306.h"
#include "Preferences.h"
#include <map>
#include <vector>

unsigned long g_ms = 0, g_us = 0;
unsigned long millis() { return g_ms; }
unsigned long micros() { return g_us; }
void delay(unsigned long) {}
void pinMode(uint8_t, uint8_t) {}
// Состояние пинов: без него digitalRead всегда возвращал HIGH, и
// настоящий updateButtons() был непроверяем — жесты приходилось
// повторять копией логики в тесте, а такой тест не поймал даже того,
// что ветка жеста в прошивку не попала.
int g_pin[64];
struct PinInit { PinInit() { for (int i = 0; i < 64; i++) g_pin[i] = HIGH; } } g_pinInit;
int  digitalRead(uint8_t p) { return p < 64 ? g_pin[p] : HIGH; }
int  analogRead(uint8_t) { return 7; }
void randomSeed(unsigned long) {}
long random(long h) { return h > 0 ? (long)(rand() % h) : 0; }
long random(long l, long h) { return h > l ? l + (long)(rand() % (h - l)) : l; }
void attachInterrupt(int, void (*)(), int) {}
int  digitalPinToInterrupt(uint8_t p) { return p; }
void noInterrupts() {}
void interrupts() {}

TwoWire Wire;
void TwoWire::begin(int, int) {}

// ---- MIDI: всё, что прошивка пишет наружу, складываем в g_midi.
std::vector<uint8_t> g_midi;
HardwareSerial Serial;
void HardwareSerial::begin(unsigned long, int, int, int) {}
int  HardwareSerial::available() { return 0; }
int  HardwareSerial::availableForWrite() { return 64; }
int  HardwareSerial::read() { return -1; }
void HardwareSerial::write(uint8_t b) { g_midi.push_back(b); }
size_t HardwareSerial::write(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) g_midi.push_back(p[i]);
  return n;
}

// ---- Экран: g_fb это пиксели, g_tx отдельный слой символов (сам
// символ кладётся в середину глифа, чтобы текст можно было прочитать).
char g_fb[32][128], g_tx[32][128];
static int g_cx = 0, g_cy = 0, g_col = 1;
void fbclear() { memset(g_fb, 0, sizeof(g_fb)); memset(g_tx, 0, sizeof(g_tx)); }
static void put(int x, int y, int c) {
  if (x >= 0 && x < 128 && y >= 0 && y < 32) g_fb[y][x] = (char)c;
}
bool Adafruit_SSD1306::begin(int, int) { return true; }
void Adafruit_SSD1306::clearDisplay() { fbclear(); }
void Adafruit_SSD1306::display() {}
void Adafruit_SSD1306::setTextSize(uint8_t) {}
void Adafruit_SSD1306::setTextColor(uint16_t c) { g_col = c; }
void Adafruit_SSD1306::setCursor(int16_t x, int16_t y) { g_cx = x; g_cy = y; }
void Adafruit_SSD1306::print(const char *s) {
  for (const char *p = s; *p; p++) {
    for (int i = 0; i < 5; i++)
      if (g_cx + i < 128 && g_cy + 3 < 32)
        g_tx[g_cy + 3][g_cx + i] = (i == 2) ? *p : '-';
    g_cx += 6;
  }
}
void Adafruit_SSD1306::print(char c) { char b[2] = {c, 0}; print(b); }
void Adafruit_SSD1306::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) put(x + i, y + j, c);
}
void Adafruit_SSD1306::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  for (int i = 0; i < w; i++) { put(x + i, y, c); put(x + i, y + h - 1, c); }
  for (int j = 0; j < h; j++) { put(x, y + j, c); put(x + w - 1, y + j, c); }
}
void Adafruit_SSD1306::drawPixel(int16_t x, int16_t y, uint16_t c) { put(x, y, c); }
void Adafruit_SSD1306::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) {
  for (int i = 0; i < w; i++) put(x + i, y, c);
}
void Adafruit_SSD1306::getTextBounds(const char *s, int16_t, int16_t, int16_t *x1,
                                     int16_t *y1, uint16_t *w, uint16_t *h) {
  *x1 = 0; *y1 = 0; *w = (uint16_t)(strlen(s) * 6); *h = 8;
}
static uint8_t g_buf[512];
uint8_t *Adafruit_SSD1306::getBuffer() {
  // Отдаём фреймбуфер в том же страничном виде, что и настоящий SSD1306.
  memset(g_buf, 0, sizeof(g_buf));
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 128; x++)
      if (g_fb[y][x]) g_buf[x + (y >> 3) * 128] |= (uint8_t)(1 << (y & 7));
  return g_buf;
}

// ---- NVS: настоящее хранилище. Заглушка, которая молча выбрасывала
// записанное, делала непроверяемыми и патчи, и их стирание.
static std::map<std::string, std::vector<uint8_t>> g_nvs;
void nvsClear() { g_nvs.clear(); }
bool Preferences::begin(const char *, bool) { return true; }
void Preferences::end() {}
size_t Preferences::putBytes(const char *k, const void *v, size_t n) {
  const uint8_t *p = (const uint8_t *)v;
  g_nvs[k] = std::vector<uint8_t>(p, p + n);
  return n;
}
size_t Preferences::getBytes(const char *k, void *v, size_t n) {
  auto it = g_nvs.find(k);
  if (it == g_nvs.end()) return 0;
  size_t m = it->second.size() < n ? it->second.size() : n;
  memcpy(v, it->second.data(), m);
  return m;
}
size_t Preferences::getBytesLength(const char *k) {
  auto it = g_nvs.find(k);
  return it == g_nvs.end() ? 0 : it->second.size();
}
size_t Preferences::putUShort(const char *k, uint16_t v) {
  g_nvs[k] = std::vector<uint8_t>((uint8_t *)&v, (uint8_t *)&v + 2);
  return 2;
}
uint16_t Preferences::getUShort(const char *k, uint16_t d) {
  auto it = g_nvs.find(k);
  if (it == g_nvs.end() || it->second.size() < 2) return d;
  uint16_t v; memcpy(&v, it->second.data(), 2); return v;
}
bool Preferences::remove(const char *k) { return g_nvs.erase(k) > 0; }
