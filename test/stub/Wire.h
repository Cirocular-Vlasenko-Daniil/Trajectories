#pragma once
#include "Arduino.h"
class TwoWire {
public:
  void begin(int = -1, int = -1);
};
extern TwoWire Wire;
