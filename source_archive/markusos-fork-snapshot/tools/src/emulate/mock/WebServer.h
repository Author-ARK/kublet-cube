#pragma once
#include "Arduino.h"
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <queue>

#define HTTP_POST 1
#define HTTP_GET 0

// Forward declaration
class WebServer;

// Global server instance (matches OTAServer's `WebServer server(80)`)
extern WebServer server;

// Emulator notification queue — filled by TCP listener or --notify-at
struct _EmuNotification {
  std::string body;  // raw JSON body
};

class WebServer {
public:
  WebServer(int port = 80) : _port(port) {}

  void begin() {
    // Start TCP listener in background thread for emulator
    _startListener();
  }

  void stop() {}

  void handleClient() {
    // Process queued notifications from TCP listener or --notify-at
    std::lock_guard<std::mutex> lock(_mutex);
    while (!_queue.empty()) {
      auto notif = _queue.front();
      _queue.pop();
      _currentBody = notif.body;
      _hasBody = true;
      if (_postHandler) {
        _postHandler();
      }
      _hasBody = false;
      _currentBody.clear();
    }
  }

  // Route registration
  void on(const char* uri, int method, void(*handler)(), void(*upload)()) {
    if (method == HTTP_POST) _postHandler = handler;
  }
  void on(const char* uri, int method, void(*handler)()) {
    if (method == HTTP_POST) _postHandler = handler;
  }
  void on(const char* uri, void(*handler)()) {}

  // Request data access (used by handlers)
  bool hasArg(const char* name) {
    if (strcmp(name, "plain") == 0) return _hasBody;
    return false;
  }
  String arg(const char* name) {
    if (strcmp(name, "plain") == 0) return String(_currentBody.c_str());
    return String("");
  }
  bool hasHeader(const char*) { return false; }
  String header(const char*) { return String(""); }
  void collectHeaders(const char*[], int) {}

  // Response (no-op in emulator, just log)
  void send(int code, const char* type = "", const char* body = "") {
    // Silently accept
  }
  void send(int code, const char* type, const String& body) {}
  void sendHeader(const char*, const char*) {}

  // Upload stub
  class Upload {
  public:
    int status = 0;
    const uint8_t* buf = nullptr;
    size_t currentSize = 0;
    size_t totalSize = 0;
    String filename;
  };
  Upload upload() { return Upload(); }

  // Enqueue a notification (called from --notify-at or TCP thread)
  void enqueueNotification(const std::string& body) {
    std::lock_guard<std::mutex> lock(_mutex);
    _queue.push({body});
  }

private:
  int _port;
  std::function<void()> _postHandler;
  std::mutex _mutex;
  std::queue<_EmuNotification> _queue;
  std::string _currentBody;
  bool _hasBody = false;

  void _startListener();  // implemented in WebServer_impl.cpp
};
