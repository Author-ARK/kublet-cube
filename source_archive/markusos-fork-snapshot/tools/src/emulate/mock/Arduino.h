#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <string>

// ---------------------------------------------------------------------------
// SDL2 — needed for millis/delay/keyboard state
// ---------------------------------------------------------------------------
#include <SDL2/SDL.h>

// ---------------------------------------------------------------------------
// Arduino type aliases
// ---------------------------------------------------------------------------
typedef bool boolean;
typedef uint8_t byte;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define HIGH 1
#define LOW  0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

#define DEC 10
#define HEX 16

#define DEG_TO_RAD 0.017453292519943295

// WiFi status
#define WL_CONNECTED 3

// ---------------------------------------------------------------------------
// Arduino String class (minimal)
// ---------------------------------------------------------------------------
class String : public std::string {
public:
  String() : std::string() {}
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
  String(int val) : std::string(std::to_string(val)) {}
  String(float val, int dec = 2) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", dec, val);
    assign(buf);
  }
  const char* c_str() const { return std::string::c_str(); }
  int toInt() const { return atoi(c_str()); }
  float toFloat() const { return atof(c_str()); }
  int indexOf(char c) const { auto p = find(c); return p == npos ? -1 : (int)p; }
  int indexOf(const char* s) const { auto p = find(s); return p == npos ? -1 : (int)p; }
  int indexOf(const char* s, int from) const { auto p = find(s, from); return p == npos ? -1 : (int)p; }
  int indexOf(char c, int from) const { auto p = find(c, from); return p == npos ? -1 : (int)p; }
  String substring(int from) const { return String(std::string::substr(from)); }
  String substring(int from, int to) const { return String(std::string::substr(from, to - from)); }
  bool startsWith(const char* s) const { return find(s) == 0; }
  bool endsWith(const char* s) const {
    size_t sl = strlen(s);
    return size() >= sl && compare(size() - sl, sl, s) == 0;
  }
  bool endsWith(const String& s) const { return endsWith(s.c_str()); }
  void toLowerCase() {
    for (auto& c : *this) c = tolower((unsigned char)c);
  }
  void toUpperCase() {
    for (auto& c : *this) c = toupper((unsigned char)c);
  }
  char charAt(unsigned int i) const { return i < size() ? (*this)[i] : 0; }
  unsigned int length() const { return (unsigned int)size(); }
  bool concat(const char* s) { append(s); return true; }
  bool concat(char c) { push_back(c); return true; }
  void trim() {
    auto s = find_first_not_of(" \t\r\n");
    auto e = find_last_not_of(" \t\r\n");
    if (s == npos) clear(); else assign(substr(s, e - s + 1));
  }
  void replace(const String& from, const String& to) {
    size_t pos = 0;
    while ((pos = find(from, pos)) != npos) {
      std::string::replace(pos, from.size(), to);
      pos += to.size();
    }
  }
};

// ---------------------------------------------------------------------------
// Serial mock — prints to stdout
// ---------------------------------------------------------------------------
class HardwareSerial {
public:
  void begin(unsigned long) {}
  void end() {}

  void print(const char* s) { printf("%s", s); }
  void print(char c) { putchar(c); }
  void print(int v, int base = DEC) {
    if (base == HEX) printf("%x", v);
    else printf("%d", v);
  }
  void print(unsigned int v) { printf("%u", v); }
  void print(long v) { printf("%ld", v); }
  void print(unsigned long v) { printf("%lu", v); }
  void print(float v, int dec = 2) { printf("%.*f", dec, v); }
  void print(double v, int dec = 2) { printf("%.*f", dec, v); }
  void print(const String& s) { printf("%s", s.c_str()); }

  void println() { putchar('\n'); }
  void println(const char* s) { printf("%s\n", s); }
  void println(char c) { printf("%c\n", c); }
  void println(int v, int base = DEC) { print(v, base); putchar('\n'); }
  void println(unsigned int v) { printf("%u\n", v); }
  void println(long v) { printf("%ld\n", v); }
  void println(unsigned long v) { printf("%lu\n", v); }
  void println(float v, int dec = 2) { printf("%.*f\n", dec, v); }
  void println(double v, int dec = 2) { printf("%.*f\n", dec, v); }
  void println(const String& s) { printf("%s\n", s.c_str()); }

  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list args;
    va_start(args, fmt);
    int r = vprintf(fmt, args);
    va_end(args);
    return r;
  }

  operator bool() const { return true; }
};

extern HardwareSerial Serial;

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield();

// ---------------------------------------------------------------------------
// GPIO — digitalRead returns keyboard state for button pin
// ---------------------------------------------------------------------------
void pinMode(uint8_t pin, uint8_t mode);
int digitalRead(uint8_t pin);
void digitalWrite(uint8_t pin, uint8_t val);
void analogWrite(uint8_t pin, int val);

// Emulator-internal: update button state from SDL
void _emu_set_button_state(bool pressed);

// ---------------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------------
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);

inline uint32_t esp_random() { return (uint32_t)rand(); }

// ---------------------------------------------------------------------------
// Math helpers
// ---------------------------------------------------------------------------
#ifndef constrain
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#endif

#ifndef _max
#define _max(a,b) ((a)>(b)?(a):(b))
#endif
#ifndef _min
#define _min(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif

inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ---------------------------------------------------------------------------
// Time — NTP stubs using system clock
// ---------------------------------------------------------------------------
struct tm;
inline void configTzTime(const char*, const char*, const char* = nullptr, const char* = nullptr) {}
inline void configTime(long, int, const char*, const char* = nullptr) {}
inline bool getLocalTime(struct tm* info, uint32_t = 5000) {
  time_t now = time(nullptr);
  *info = *localtime(&now);
  return true;
}

// ---------------------------------------------------------------------------
// WiFi stub
// ---------------------------------------------------------------------------
class IPAddress {
  String _str;
public:
  IPAddress(const char* s = "127.0.0.1") : _str(s) {}
  String toString() const { return _str; }
  const char* c_str() const { return _str.c_str(); }
};

class WiFiClass {
public:
  int status() { return WL_CONNECTED; }
  IPAddress localIP() { return IPAddress("127.0.0.1"); }
  const char* macAddress() { return "00:00:00:00:00:00"; }
  void mode(int) {}
  void begin(const char*, const char*) {}
};

extern WiFiClass WiFi;

// ---------------------------------------------------------------------------
// EEPROM / NVS stubs
// ---------------------------------------------------------------------------
class EEPROMClass {
public:
  void begin(int) {}
  uint8_t read(int) { return 0; }
  void write(int, uint8_t) {}
  void commit() {}
};

// ---------------------------------------------------------------------------
// Misc ESP32 functions
// ---------------------------------------------------------------------------
inline void esp_restart() { exit(0); }
inline uint32_t ESP_getFreeHeap() { return 200000; }

class EspClass {
public:
  uint32_t getFreeHeap() { return 200000; }
  uint32_t getMaxAllocHeap() { return 100000; }
  uint32_t getFreePsram() { return 0; }
  void restart() { exit(0); }
};

inline EspClass ESP;
