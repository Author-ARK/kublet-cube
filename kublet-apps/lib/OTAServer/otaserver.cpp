#include "otaserver.h"

WebServer server(80);

static volatile bool _otaPendingRestart = false;
static unsigned long _otaRestartRequestedAt = 0;

/***************************************************************************************
** Function name:           init
** Description:             Initialize OTA server and endpoint
***************************************************************************************/
void OTAServer::init() {
if (!MDNS.begin("esp32")) { //http://esp32.local
    Serial.println("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");

  /* health check endpoint for deploy tool verification */
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "OK");
  });

  /* Collect custom header for firmware MD5 verification */
  const char* headerKeys[] = {"X-Firmware-MD5"};
  server.collectHeaders(headerKeys, 1);

  /*handling uploading firmware file */
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
      // Set expected MD5 if provided by deploy tool
      if (server.hasHeader("X-Firmware-MD5")) {
        String md5 = server.header("X-Firmware-MD5");
        if (md5.length() == 32) {
          Update.setMD5(md5.c_str());
          Serial.printf("OTA: MD5 verification enabled: %s\n", md5.c_str());
        }
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
        _otaPendingRestart = true;
        _otaRestartRequestedAt = millis();
      } else {
        Serial.println("OTA: Update FAILED (possible MD5 mismatch or write error)");
        Update.printError(Serial);
      }
    }
  });
}

/***************************************************************************************
** Function name:           start
** Description:             Start OTA server
***************************************************************************************/
void OTAServer::start() {
  server.begin();
}

/***************************************************************************************
** Function name:           run
** Description:             Initialize and start OTA server
***************************************************************************************/
void OTAServer::run() {
  init();
  start();
}

/***************************************************************************************
** Function name:           handle
** Description:             Handle incoming connections from client sending OTA firmware
***************************************************************************************/
void OTAServer::handle() {
  server.handleClient();
  if (_otaPendingRestart && (millis() - _otaRestartRequestedAt > 1000)) {
    Serial.println("OTA: deferred restart");
    delay(100);
    ESP.restart();
  }
}

/***************************************************************************************
** Function name:           stop
** Description:             Stop OTA server
***************************************************************************************/
void OTAServer::stop() {
  server.stop();
}

/***************************************************************************************
** Function name:           connectWifi
** Description:             Connects to WiFi
***************************************************************************************/
void OTAServer::connectWiFi() {
  pref.begin("core");
  std::string ssid = pref.getString("ssid").c_str();
  std::string pw = pref.getString("pw").c_str();
  pref.end();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pw.c_str());
  Serial.print("Connected to ");
  Serial.println(ssid.c_str());
  while((WiFi.status() != WL_CONNECTED)) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.localIP());
}
