#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <time.h>

#define BUTTON_PIN 19

// POSIX timezone string
#define TIMEZONE "EST5EDT,M3.2.0,M11.1.0"

OTAServer otaserver;
KGFX ui;

const unsigned long NTP_SYNC_INTERVAL_MS = 15 * 60 * 1000;
unsigned long lastNtpSync = 0;
bool timeValid = false;

// Display state for partial redraws
int prevSec = -1;
int prevMin = -1;
int prevHour = -1;

// Clock geometry
#define CX 120
#define CY 120
#define FACE_R 115

// Colors
#define CLR_BG       TFT_BLACK
#define CLR_FACE     TFT_BLACK
#define CLR_TICK     0xBDF7   // light gray
#define CLR_HOUR_MAJ TFT_WHITE
#define CLR_HOUR     TFT_WHITE
#define CLR_MIN      TFT_WHITE
#define CLR_SEC      0xF800   // red
#define CLR_CENTER   TFT_WHITE
#define CLR_RIM      0x4208   // dark gray

// Precomputed sin/cos lookup (360 entries, scaled by 1000)
static int16_t sinTable[360];
static int16_t cosTable[360];

void buildTrigTables() {
  for (int i = 0; i < 360; i++) {
    float rad = i * DEG_TO_RAD;
    sinTable[i] = (int16_t)(sinf(rad) * 1000);
    cosTable[i] = (int16_t)(cosf(rad) * 1000);
  }
}

// Get x,y position on clock face from angle (0=12 o'clock, clockwise) and radius
void clockPos(int angle, int radius, int &x, int &y) {
  int a = ((angle % 360) + 360) % 360;
  x = CX + (radius * sinTable[a]) / 1000;
  y = CY - (radius * cosTable[a]) / 1000;
}

// Draw a thick hand by drawing a narrow triangle
void drawHand(int angle, int length, int baseWidth, uint16_t color) {
  int tipX, tipY, lx, ly, rx, ry;
  clockPos(angle, length, tipX, tipY);
  clockPos(angle - 90, baseWidth, lx, ly);
  clockPos(angle + 90, baseWidth, rx, ry);
  ui.tft.fillTriangle(tipX, tipY, lx, ly, rx, ry, color);
}

// Draw a thin line hand
void drawLineHand(int angle, int length, uint16_t color) {
  int x, y;
  clockPos(angle, length, x, y);
  ui.tft.drawLine(CX, CY, x, y, color);
}

void drawFace() {
  ui.tft.fillScreen(CLR_BG);

  // Outer rim
  ui.tft.drawSmoothCircle(CX, CY, FACE_R, CLR_RIM, CLR_BG);
  ui.tft.drawSmoothCircle(CX, CY, FACE_R - 1, CLR_RIM, CLR_BG);

  // Hour markers
  for (int i = 0; i < 12; i++) {
    int angle = i * 30;
    int x1, y1, x2, y2;
    clockPos(angle, FACE_R - 6, x1, y1);
    clockPos(angle, FACE_R - 18, x2, y2);
    // Thick lines for hour markers
    ui.tft.drawLine(x1, y1, x2, y2, CLR_HOUR_MAJ);
    // Slightly offset for thickness
    ui.tft.drawLine(x1 + 1, y1, x2 + 1, y2, CLR_HOUR_MAJ);
    ui.tft.drawLine(x1, y1 + 1, x2, y2 + 1, CLR_HOUR_MAJ);
  }

  // Minute tick marks
  for (int i = 0; i < 60; i++) {
    if (i % 5 == 0) continue; // skip hour positions
    int angle = i * 6;
    int x1, y1, x2, y2;
    clockPos(angle, FACE_R - 6, x1, y1);
    clockPos(angle, FACE_R - 10, x2, y2);
    ui.tft.drawLine(x1, y1, x2, y2, CLR_TICK);
  }
}

void drawHands(int h, int m, int s) {
  // Hour hand: moves smoothly with minutes
  int hourAngle = (h % 12) * 30 + m / 2;
  drawHand(hourAngle, 58, 5, CLR_HOUR);

  // Minute hand: moves smoothly with seconds
  int minAngle = m * 6 + s / 10;
  drawHand(minAngle, 82, 3, CLR_MIN);

  // Second hand: thin line
  int secAngle = s * 6;
  drawLineHand(secAngle, 90, CLR_SEC);
  // Small tail
  drawLineHand(secAngle + 180, 16, CLR_SEC);

  // Center dot
  ui.tft.fillSmoothCircle(CX, CY, 4, CLR_CENTER, CLR_FACE);
  ui.tft.fillSmoothCircle(CX, CY, 2, CLR_SEC, CLR_CENTER);
}

// Erase old hands by redrawing them in background color, then redraw face details they covered
void eraseHands(int h, int m, int s) {
  int secAngle = s * 6;
  drawLineHand(secAngle, 90, CLR_BG);
  drawLineHand(secAngle + 180, 16, CLR_BG);

  int minAngle = m * 6 + s / 10;
  drawHand(minAngle, 82, 3, CLR_BG);

  int hourAngle = (h % 12) * 30 + m / 2;
  drawHand(hourAngle, 58, 5, CLR_BG);
}

// Redraw tick marks that may have been overwritten by erased hands
void repairTicks() {
  for (int i = 0; i < 60; i++) {
    int angle = i * 6;
    int x1, y1, x2, y2;
    if (i % 5 == 0) {
      clockPos(angle, FACE_R - 6, x1, y1);
      clockPos(angle, FACE_R - 18, x2, y2);
      ui.tft.drawLine(x1, y1, x2, y2, CLR_HOUR_MAJ);
      ui.tft.drawLine(x1 + 1, y1, x2 + 1, y2, CLR_HOUR_MAJ);
      ui.tft.drawLine(x1, y1 + 1, x2, y2 + 1, CLR_HOUR_MAJ);
    } else {
      clockPos(angle, FACE_R - 6, x1, y1);
      clockPos(angle, FACE_R - 10, x2, y2);
      ui.tft.drawLine(x1, y1, x2, y2, CLR_TICK);
    }
  }
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
  Serial.printf("NTP sync: %02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
  return true;
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting analog clock");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  buildTrigTables();

  ui.tft.fillScreen(CLR_BG);

  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");

  if (syncNtp()) {
    timeValid = true;
    lastNtpSync = millis();
  }

  drawFace();

  if (timeValid) {
    struct tm tm;
    if (getLocalTime(&tm, 0)) {
      drawHands(tm.tm_hour, tm.tm_min, tm.tm_sec);
      prevHour = tm.tm_hour;
      prevMin = tm.tm_min;
      prevSec = tm.tm_sec;
    }
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT

    if (!timeValid) {
      if (syncNtp()) {
        timeValid = true;
        lastNtpSync = millis();
        drawFace();
      }
      return;
    }

    // Periodic NTP resync
    if (millis() - lastNtpSync > NTP_SYNC_INTERVAL_MS) {
      syncNtp();
      lastNtpSync = millis();
    }

    // Update hands every second
    struct tm tm;
    if (getLocalTime(&tm, 0) && tm.tm_sec != prevSec) {
      // Erase old hands
      if (prevSec >= 0) {
        eraseHands(prevHour, prevMin, prevSec);
        repairTicks();
      }

      // Draw new hands
      drawHands(tm.tm_hour, tm.tm_min, tm.tm_sec);

      prevHour = tm.tm_hour;
      prevMin = tm.tm_min;
      prevSec = tm.tm_sec;
    }
  }

  delay(50);
}
