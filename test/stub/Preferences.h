#pragma once
#include "Arduino.h"
#include <cstddef>
class Preferences {
public:
  bool     begin(const char *, bool = false);
  void     end();
  size_t   putBytes(const char *, const void *, size_t);
  size_t   getBytes(const char *, void *, size_t);
  size_t   getBytesLength(const char *);
  size_t   putUShort(const char *, uint16_t);
  uint16_t getUShort(const char *, uint16_t = 0);
  bool     remove(const char *);
};
