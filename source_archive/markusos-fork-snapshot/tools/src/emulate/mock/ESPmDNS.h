#pragma once
class MDNSClass {
public:
  bool begin(const char*) { return true; }
  void addService(const char*, const char*, int) {}
};
inline MDNSClass MDNS;
