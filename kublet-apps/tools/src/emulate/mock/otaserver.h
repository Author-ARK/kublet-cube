#pragma once

#include "Arduino.h"
#include "ArduinoJSON.h"
#include <fstream>
#include <sstream>
#include <string>

#ifndef EMU_APP_DIR
#define EMU_APP_DIR ""
#endif

// Stub Preferences — loads defaults from assets/preferences.json
class Preferences {
public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  String getString(const char* key, const String& def = "") {
    std::string appDir = EMU_APP_DIR;
    if (appDir.empty()) return def;

    std::ifstream f(appDir + "/assets/preferences.json");
    if (!f.is_open()) return def;

    std::ostringstream ss;
    ss << f.rdbuf();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, ss.str());
    if (err) {
      printf("[EMU] WARNING: Failed to parse preferences.json: %s\n", err.c_str());
      return def;
    }

    const char* val = doc[key];
    if (!val) {
      printf("[EMU] WARNING: Preference key '%s' not found in preferences.json\n", key);
      return def;
    }
    return String(val);
  }
  unsigned long getULong(const char* key, unsigned long def = 0) {
    String val = getString(key, "");
    if (val.length() == 0) return def;
    return strtoul(val.c_str(), nullptr, 10);
  }
  void putString(const char*, const String&) {}
  void putULong(const char*, unsigned long) {}
};

inline Preferences pref;

// Forward-declare the global server from WebServer.h
class WebServer;
extern WebServer server;

class OTAServer {
public:
  void init() { Serial.println("[EMU] OTAServer init (no-op)"); }
  void start() {}
  void run();  // defined in WebServer_impl.cpp — starts TCP listener
  void handle();  // defined in WebServer_impl.cpp — calls server.handleClient()
  void stop() {}
  void connectWiFi() { Serial.println("[EMU] WiFi simulated — connected"); }
};

