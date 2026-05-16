#pragma once

#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

#include <WiFi.h>
#include <Preferences.h>

class OTAServer {
  private:

  public:
    void init();
    void start();
    void run();
    void handle();
    void stop();

    void connectWifi();
};