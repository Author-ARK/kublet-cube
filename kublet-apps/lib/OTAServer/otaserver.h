#pragma once

#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

#include <WiFi.h>
#include <Preferences.h>

inline Preferences pref;

class OTAServer {
  private:

  public:
    void init();
    void start();
    void run();
    void handle();
    void stop();

    void connectWiFi();
};
