#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"
#include "ArduinoJSON.h"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>

#define HTTP_CODE_OK 200

// Follow redirect modes
#define HTTPC_STRICT_FOLLOW_REDIRECTS 1
#define HTTPC_FORCE_FOLLOW_REDIRECTS 2

// ---------------------------------------------------------------------------
// WiFiClient stub — supports reading from an in-memory buffer
// ---------------------------------------------------------------------------
class WiFiClient {
public:
  WiFiClient() {}

  bool connect(const char*, uint16_t) { return true; }
  void stop() { _pos = 0; _data.clear(); }
  bool connected() { return _pos < _data.size(); }
  int available() { return (int)(_data.size() - _pos); }

  int read() {
    if (_pos < _data.size()) return (uint8_t)_data[_pos++];
    return -1;
  }

  int read(uint8_t* buf, size_t len) {
    return (int)readBytes(buf, len);
  }

  size_t readBytes(uint8_t* buf, size_t len) {
    size_t avail = _data.size() - _pos;
    size_t n = len < avail ? len : avail;
    if (n > 0) { memcpy(buf, _data.data() + _pos, n); _pos += n; }
    return n;
  }

  String readStringUntil(char terminator) {
    std::string result;
    while (_pos < _data.size()) {
      char c = _data[_pos++];
      if (c == terminator) break;
      result += c;
    }
    return String(result.c_str());
  }

  size_t write(const uint8_t*, size_t len) { return len; }
  size_t write(uint8_t) { return 1; }
  int printf(const char*, ...) { return 0; }

  void setTimeout(int) {}

  // Internal: load data for stream simulation
  void _loadData(const std::vector<char>& data) { _data = data; _pos = 0; }
  void _loadData(const std::string& s) { _data.assign(s.begin(), s.end()); _pos = 0; }

private:
  std::vector<char> _data;
  size_t _pos = 0;
};

// ---------------------------------------------------------------------------
// Fixture matching: loads from EMU_APP_DIR/assets/http_fixtures.json
// ---------------------------------------------------------------------------
#ifndef EMU_APP_DIR
#define EMU_APP_DIR ""
#endif

namespace _emu_http {

// Simple wildcard match: * matches any substring
inline bool wildcardMatch(const char* pattern, const char* str) {
  while (*pattern && *str) {
    if (*pattern == '*') {
      pattern++;
      if (!*pattern) return true;
      while (*str) {
        if (wildcardMatch(pattern, str)) return true;
        str++;
      }
      return false;
    }
    if (*pattern != *str) return false;
    pattern++; str++;
  }
  while (*pattern == '*') pattern++;
  return *pattern == '\0' && *str == '\0';
}

// Read an entire file into a string (binary-safe)
inline std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Fixture {
  std::string urlPattern;
  std::string file;
  int status;
};

inline std::vector<Fixture> loadFixtures() {
  std::vector<Fixture> fixtures;
  std::string appDir = EMU_APP_DIR;
  if (appDir.empty()) return fixtures;

  std::string content = readFile(appDir + "/assets/http_fixtures.json");
  if (content.empty()) return fixtures;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, content);
  if (err) {
    printf("[EMU] WARNING: Failed to parse http_fixtures.json: %s\n", err.c_str());
    return fixtures;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    Fixture f;
    f.urlPattern = obj["url"] | "";
    f.file = obj["file"] | "";
    f.status = obj["status"] | 200;

    if (!f.urlPattern.empty() && !f.file.empty()) {
      fixtures.push_back(f);
    }
  }
  return fixtures;
}

// Match URL against fixtures and return file content + status
struct MatchResult {
  bool matched;
  int status;
  std::string content;
};

inline MatchResult matchFixture(const std::string& url) {
  static std::vector<Fixture> fixtures = loadFixtures();
  static std::vector<size_t> matchCounts(fixtures.size(), 0);

  std::string appDir = EMU_APP_DIR;

  for (size_t i = 0; i < fixtures.size(); i++) {
    if (wildcardMatch(fixtures[i].urlPattern.c_str(), url.c_str())) {
      // For patterns with wildcards, cycle through numbered files if they exist
      // e.g., item_1.json, item_2.json, etc.
      std::string filename = fixtures[i].file;

      // Check if there are numbered variants (name_N.ext pattern)
      size_t dot = filename.rfind('.');
      if (dot != std::string::npos) {
        std::string base = filename.substr(0, dot);
        std::string ext = filename.substr(dot);
        // Try next numbered variant
        size_t uscore = base.rfind('_');
        if (uscore != std::string::npos) {
          std::string numPart = base.substr(uscore + 1);
          bool isNum = !numPart.empty();
          for (char c : numPart) { if (!isdigit(c)) { isNum = false; break; } }
          if (isNum) {
            int startNum = atoi(numPart.c_str());
            int tryNum = startNum + (int)matchCounts[i];
            std::string tryFile = base.substr(0, uscore + 1) + std::to_string(tryNum) + ext;
            std::string tryPath = appDir + "/assets/" + tryFile;
            std::string tryContent = readFile(tryPath);
            if (!tryContent.empty()) {
              matchCounts[i]++;
              return {true, fixtures[i].status, tryContent};
            }
            // Reset counter and fall back to the base file
            matchCounts[i] = 0;
          }
        }
      }

      std::string path = appDir + "/assets/" + filename;
      std::string content = readFile(path);
      if (!content.empty()) {
        matchCounts[i]++;
        return {true, fixtures[i].status, content};
      }
    }
  }
  printf("[EMU] WARNING: No fixture matched URL: %s\n", url.c_str());
  return {false, 200, "{}"};
}

} // namespace _emu_http

// ---------------------------------------------------------------------------
// HTTPClient mock with fixture loading
// ---------------------------------------------------------------------------
class HTTPClient {
public:
  void begin(const String& url) { _url = url; }
  void begin(const String& host, int port, const String& path) {
    _url = String("http://") + host + ":" + String(port) + path;
  }
  bool begin(class WiFiClientSecure&, const String& url) { _url = url; return true; }
  bool begin(WiFiClient&, const String& url) { _url = url; return true; }

  void addHeader(const String&, const String&) {}
  void setFollowRedirects(int) {}
  void setReuse(bool) {}
  void setTimeout(int) {}
  void setUserAgent(const String&) {}
  void setAuthorization(const char*) {}
  void setAuthorization(const char*, const char*) {}

  int GET() {
    auto result = _emu_http::matchFixture(std::string(_url.c_str()));
    _responseBody = result.content;
    _status = result.status;
    _stream._loadData(_responseBody);
    return _status;
  }

  int POST(const String&) {
    auto result = _emu_http::matchFixture(std::string(_url.c_str()));
    _responseBody = result.content;
    _status = result.status;
    _stream._loadData(_responseBody);
    return _status;
  }

  String getString() { return String(_responseBody.c_str()); }
  int getSize() { return (int)_responseBody.size(); }
  bool connected() { return _stream.connected(); }

  WiFiClient* getStreamPtr() { return &_stream; }

  void end() {
    _url = "";
    _responseBody.clear();
  }

private:
  String _url;
  std::string _responseBody;
  int _status = 200;
  WiFiClient _stream;
};
