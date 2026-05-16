#pragma once
#include "Arduino.h"

class WiFiClientSecure {
public:
  void setInsecure() {}
  int connect(const char*, uint16_t) { return 0; }
  void stop() {}
  bool connected() { return false; }
  int available() { return 0; }
  String readStringUntil(char) { return ""; }
  size_t print(const String&) { return 0; }
  size_t println(const String& s = "") { return 0; }
};
