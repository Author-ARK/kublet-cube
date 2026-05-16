#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

Preferences preferences;
OTAServer otaserver;
KGFX ui;
HTTPClient http;
JsonDocument json;
String serverUrl;

int sessionPercent = 0;
int weeklyPercent = 0;

int refreshTimeInSeconds = 300;
unsigned long lastTime = 0;

#define MATRIX_TRACK  0x0200  // Dark green track background
#define MATRIX_BRIGHT 0x07E0  // Bright green (session)
#define MATRIX_GREEN  0x05E0  // Medium green (weekly)
#define CENTER_X 120
#define CENTER_Y 112
#define OUTER_R 105
#define OUTER_IR 85
#define INNER_R 70
#define INNER_IR 50
#define ARC_START 45
#define ARC_END 315
#define ARC_SPAN 270

void drawRing(int cx, int cy, int outerR, int innerR, int percent, uint16_t color) {
  // Background track (full 270 degrees)
  ui.tft.drawSmoothArc(cx, cy, outerR, innerR, ARC_START, ARC_END, MATRIX_TRACK, TFT_BLACK, false);

  // Foreground arc proportional to percent
  if (percent > 0) {
    uint16_t fgColor = (percent > 90) ? TFT_RED : color;
    uint32_t span = (uint32_t)((long)ARC_SPAN * percent / 100);
    if (span > ARC_SPAN) span = ARC_SPAN;
    uint32_t endAngle = (ARC_START + span) % 360;
    ui.tft.drawSmoothArc(cx, cy, outerR, innerR, ARC_START, endAngle, fgColor, TFT_BLACK, true);
  }
}

void drawBoltIcon(int x, int y, uint16_t color) {
  // Lightning bolt icon ~8x14px
  ui.tft.fillTriangle(x + 4, y, x, y + 6, x + 5, y + 5, color);
  ui.tft.fillTriangle(x + 3, y + 5, x + 8, y + 6, x + 4, y + 14, color);
}

void drawCalendarIcon(int x, int y, uint16_t color) {
  // Calendar icon ~10x12px
  ui.tft.drawRect(x, y + 3, 10, 9, color);
  ui.tft.fillRect(x, y + 3, 10, 3, color);
  ui.tft.fillRect(x + 2, y, 2, 4, color);
  ui.tft.fillRect(x + 6, y, 2, 4, color);
}

void drawUI() {
  ui.tft.fillScreen(TFT_BLACK);

  // Draw concentric rings
  drawRing(CENTER_X, CENTER_Y, OUTER_R, OUTER_IR, sessionPercent, MATRIX_BRIGHT);
  drawRing(CENTER_X, CENTER_Y, INNER_R, INNER_IR, weeklyPercent, MATRIX_GREEN);

  // Percentage text inside inner ring
  char sessionStr[8];
  char weeklyStr[8];
  snprintf(sessionStr, sizeof(sessionStr), "%d%%", sessionPercent);
  snprintf(weeklyStr, sizeof(weeklyStr), "%d%%", weeklyPercent);

  // Session percentage (upper half of inner ring)
  ui.tft.setTTFFont(Arial_24_Bold);
  ui.tft.setTextColor(MATRIX_BRIGHT, TFT_BLACK);
  int sw = ui.tft.TTFtextWidth(sessionStr);
  ui.tft.setCursor((240 - sw) / 2, 90);
  ui.tft.print(sessionStr);

  // Weekly percentage (lower half of inner ring)
  ui.tft.setTTFFont(Arial_20_Bold);
  ui.tft.setTextColor(MATRIX_GREEN, TFT_BLACK);
  int ww = ui.tft.TTFtextWidth(weeklyStr);
  ui.tft.setCursor((240 - ww) / 2, 118);
  ui.tft.print(weeklyStr);

  // Bottom labels with icons
  drawBoltIcon(40, 224, MATRIX_BRIGHT);
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(MATRIX_BRIGHT, TFT_BLACK);
  ui.tft.setCursor(52, 226);
  ui.tft.print("Session");

  drawCalendarIcon(148, 224, MATRIX_GREEN);
  ui.tft.setTextColor(MATRIX_GREEN, TFT_BLACK);
  ui.tft.setCursor(162, 226);
  ui.tft.print("Weekly");
}

void fetchUsageData() {
  if (serverUrl.length() == 0) {
    return;
  }

  http.begin(serverUrl + "/api/usage");
  int httpResponseCode = http.GET();

  if (httpResponseCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DeserializationError error = deserializeJson(json, payload);
    if (!error) {
      sessionPercent = constrain((int)(json["session"]["percent"] | 0), 0, 100);
      weeklyPercent = constrain((int)(json["weekly"]["percent"] | 0), 0, 100);
    }
  }

  http.end();
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run(); // DO NOT EDIT

  ui.init();
  ui.clear();

  preferences.begin("app", true);
  serverUrl = preferences.getString("server_url");
  preferences.end();
}

void loop() {
  if ((WiFi.status() == WL_CONNECTED)) {
    otaserver.handle(); // DO NOT EDIT

    if (((millis() - lastTime) > (unsigned long)refreshTimeInSeconds * 1000) || lastTime == 0) {
      lastTime = millis();
      fetchUsageData();
      drawUI();
    }
  }

  delay(1);
}
