#pragma once
class UpdateClass {
public:
  bool begin(size_t = 0) { return false; }
  size_t write(const uint8_t*, size_t) { return 0; }
  bool end(bool = true) { return false; }
  bool hasError() { return false; }
  void setMD5(const char*) {}
  bool isFinished() { return false; }
};
inline UpdateClass Update;
