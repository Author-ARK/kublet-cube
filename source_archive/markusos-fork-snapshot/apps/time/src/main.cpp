#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <time.h>

#define BUTTON_PIN 19

// POSIX timezone string — change this for your timezone
// Examples:
//   ET (US Eastern):  "EST5EDT,M3.2.0,M11.1.0"
//   CT (US Central):  "CST6CDT,M3.2.0,M11.1.0"
//   PT (US Pacific):  "PST8PDT,M3.2.0,M11.1.0"
//   CET (Europe):     "CET-1CEST,M3.5.0,M10.5.0/3"
//   UTC:              "UTC0"
#define TIMEZONE "EST5EDT,M3.2.0,M11.1.0"

OTAServer otaserver;
KGFX ui;

// NTP sync interval: every 15 minutes
const unsigned long NTP_SYNC_INTERVAL_MS = 15 * 60 * 1000;
unsigned long lastNtpSync = 0;
bool timeValid = false;

// Display state
int dispHours = -1;
int dispMinutes = -1;
int dispDay = -1;

const char* DAY_NAMES[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday",
  "Thursday", "Friday", "Saturday"
};

const char* MONTH_NAMES[] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};

// Get current local time from the system RTC (set by NTP)
bool getTime(struct tm &tm) {
  return getLocalTime(&tm, 0);
}

bool syncNtp() {
  struct tm tm;
  int retries = 0;
  while (!getLocalTime(&tm) && retries < 10) {
    delay(500);
    retries++;
  }

  if (retries >= 10) {
    Serial.println("NTP sync failed");
    return false;
  }

  Serial.printf("NTP sync: %02d:%02d %s %s %d, %d\n",
                tm.tm_hour, tm.tm_min, DAY_NAMES[tm.tm_wday],
                MONTH_NAMES[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
  return true;
}

// ySpan: total vertical space allocated for this line (used to clear old pixels)
void drawCentered(const char* text, const tftfont_t& font, uint16_t color, int y, int ySpan = 0) {
  ui.tft.setTTFFont(font);
  if (ySpan > 0) ui.tft.fillRect(0, y - 2, 240, ySpan, TFT_BLACK);
  ui.tft.setTextColor(color, TFT_BLACK);
  int w = ui.tft.TTFtextWidth(text);
  ui.tft.setCursor((240 - w) / 2, y);
  ui.tft.print(text);
}

// Layout constants
#define Y_TIME  75
#define H_TIME  62
#define Y_DAY   155
#define H_DAY   22
#define Y_DATE  185
#define H_DATE  22

void updateDisplay() {
  struct tm tm;
  if (!getTime(tm)) return;

  // Time — only redraw when HH:MM changes
  if (tm.tm_hour != dispHours || tm.tm_min != dispMinutes) {
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    drawCentered(timeBuf, Arial_48_Bold, TFT_WHITE, Y_TIME, H_TIME);
    dispHours = tm.tm_hour;
    dispMinutes = tm.tm_min;
  }

  // Day + date — only redraw when day changes
  if (tm.tm_mday != dispDay) {
    drawCentered(DAY_NAMES[tm.tm_wday], Arial_14_Bold, 0xBDF7, Y_DAY, H_DAY);

    char dateBuf[32];
    snprintf(dateBuf, sizeof(dateBuf), "%s %d, %d",
             MONTH_NAMES[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
    drawCentered(dateBuf, Arial_14_Bold, 0x7BEF, Y_DATE, H_DATE);
    dispDay = tm.tm_mday;
  }
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting clock app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ui.tft.fillScreen(TFT_BLACK);
  drawCentered("Syncing...", Arial_14_Bold, 0x7BEF, 110);

  // Configure timezone once, then sync
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");

  if (syncNtp()) {
    timeValid = true;
    lastNtpSync = millis();
    ui.tft.fillScreen(TFT_BLACK);
    updateDisplay();
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT

    // Retry initial sync if it failed
    if (!timeValid) {
      if (syncNtp()) {
        timeValid = true;
        lastNtpSync = millis();
        ui.tft.fillScreen(TFT_BLACK);
        updateDisplay();
      }
      return;
    }

    // Periodic NTP resync
    if (millis() - lastNtpSync > NTP_SYNC_INTERVAL_MS) {
      syncNtp();
      lastNtpSync = millis();
    }

    // Update display every second using system RTC
    updateDisplay();
  }

  delay(200);
}
