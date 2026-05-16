#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <time.h>

#define BUTTON_PIN 19
#define BL_PIN 15
#define BL_CHANNEL 0

#ifndef KUBLET_NUM
#define KUBLET_NUM 1
#endif

// ESP32 ledc PWM stubs for the desktop emulator
#ifdef EMU_APP_DIR
inline void ledcSetup(uint8_t, uint32_t, uint8_t) {}
inline void ledcAttachPin(uint8_t, uint8_t) {}
inline void ledcWrite(uint8_t, uint32_t) {}
#endif

OTAServer otaserver;
KGFX ui;

unsigned long lastDrawMs = 0;
bool ntpSynced = false;
uint8_t currentBrightness = 0;

#define COLOR_DGRAY  0x2104
#define COLOR_GRAY   0x4208
#define COLOR_GREEN  0x07E0
#define COLOR_RED    0xF800

// Per-zone colors — picked to be readable on black and visually distinct.
// RGB565: high byte = RRRRRGGG, low byte = GGGBBBBB
#define COLOR_ASU   0xFD20   // amber-orange (warm South America)
#define COLOR_TPA   0x07FF   // cyan (cool US)
#define COLOR_FRA   0xFC9F   // light pink/rose (Frankfurt, DE)
#define COLOR_COL   0xFFE0   // yellow (Sri Lanka tea)
#define COLOR_UTC   0xC7FC   // pale blue-white (reference)

// POSIX TZ strings — these handle DST automatically via newlib's tzset().
// Format reference: https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
// Asuncion: no DST since Oct 2024. Tampa: US Eastern (DST). Berlin: CET (DST).
// Colombo: +5:30, no DST. UTC: reference.
struct Zone {
  const char *label;
  const char *tzSpec;
  uint16_t    color;
};

static const Zone ZONES[] = {
  {"ASU", "<-03>3",                     COLOR_ASU},  // Asuncion, PY — fixed UTC-3
  {"TPA", "EST5EDT,M3.2.0,M11.1.0",     COLOR_TPA},  // Tampa, FL — US DST
  {"FRA", "CET-1CEST,M3.5.0,M10.5.0/3", COLOR_FRA},  // Frankfurt, DE — EU DST
  {"COL", "<+0530>-5:30",               COLOR_COL},  // Colombo, LK — fixed +5:30
  {"UTC", "UTC0",                       COLOR_UTC},  // UTC reference
};
static const int N_ZONES = sizeof(ZONES) / sizeof(ZONES[0]);

// Brightness schedule keyed off Asuncion local time (-3, no DST).
uint8_t computeBrightness() {
  if (!ntpSynced) return 255;
  time_t now;
  time(&now);
  setenv("TZ", "<-03>3", 1);
  tzset();
  struct tm tmBuf;
  localtime_r(&now, &tmBuf);
  int h = tmBuf.tm_hour;
  if (h >= 8 && h < 18) return 255;
  if (h >= 6 && h < 22) return 128;
  return 40;
}

void applyBrightness() {
  uint8_t b = computeBrightness();
  if (b != currentBrightness) {
    ledcWrite(BL_CHANNEL, b);
    currentBrightness = b;
  }
}

// Header: "TIME" left, today's full date (dd/mm/yyyy in Asuncion local)
// next to it, and the #N badge top-right. The per-zone rows below now
// only need to carry HH:MM:SS, since the absolute date lives up here.
void drawHeader() {
  ui.tft.fillRect(0, 0, 240, 30, TFT_BLACK);

  // Title
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  ui.tft.setCursor(8, 6);
  ui.tft.print("TIME");

  // Kublet number, right edge
  char numStr[8];
  snprintf(numStr, sizeof(numStr), "#%d", KUBLET_NUM);
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  int nw = ui.tft.TTFtextWidth(numStr);
  int nx = 238 - nw;
  ui.tft.setCursor(nx, 7);
  ui.tft.print(numStr);

  // Today's date (Asuncion local) — full dd/mm/yyyy, dead-centered on the
  // screen midline regardless of what TIME/#N take on either side.
  if (ntpSynced) {
    setenv("TZ", "<-03>3", 1);
    tzset();
    time_t now;
    time(&now);
    struct tm tmBuf;
    localtime_r(&now, &tmBuf);
    char dateStr[16];
    snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d",
             tmBuf.tm_mday, tmBuf.tm_mon + 1, tmBuf.tm_year + 1900);
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
    int dw = ui.tft.TTFtextWidth(dateStr);
    ui.tft.setCursor((240 - dw) / 2, 7);
    ui.tft.print(dateStr);
  }

  ui.tft.drawFastHLine(0, 28, 240, COLOR_DGRAY);
}

// Returns 0 if zone is on the same calendar date as Asuncion, +1 if a day
// ahead, -1 if a day behind. Day-of-year compare handles the year wrap
// at New Year's automatically.
static int dayDeltaVsAsu(const struct tm &zone, const struct tm &asu) {
  if (zone.tm_year == asu.tm_year && zone.tm_yday == asu.tm_yday) return 0;
  if (zone.tm_year > asu.tm_year ||
      (zone.tm_year == asu.tm_year && zone.tm_yday > asu.tm_yday))  return +1;
  return -1;
}

// Render a single zone row at y0..y0+34. Picks the right TZ, asks newlib
// to do the timezone math (including DST), prints "Lbl  HH:MM:SS", and —
// only when the zone falls on a different calendar date than Asuncion —
// tags the row with "+1d" or "-1d" on the right. A thin divider is drawn
// at the bottom unless this is the last row in the table.
void drawZoneRow(int y0, const Zone &zone, time_t utc,
                 const struct tm &asuTm, bool drawDivider) {
  setenv("TZ", zone.tzSpec, 1);
  tzset();
  struct tm tmBuf;
  localtime_r(&utc, &tmBuf);

  ui.tft.fillRect(0, y0, 240, 34, TFT_BLACK);

  // Build all three pieces' widths up-front so we can center the whole
  // group together (label + gap + time + optional gap + ±1d tag). This
  // keeps each row visually balanced regardless of how many pieces it
  // happens to carry today.
  char timeStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
           tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
  int delta = dayDeltaVsAsu(tmBuf, asuTm);
  char tag[6] = "";
  if (delta != 0) snprintf(tag, sizeof(tag), "%+dd", delta);

  ui.tft.setTTFFont(Arial_16_Bold);
  int labelW = ui.tft.TTFtextWidth(zone.label);
  int timeW  = ui.tft.TTFtextWidth(timeStr);
  ui.tft.setTTFFont(Arial_12);
  int tagW   = tag[0] ? ui.tft.TTFtextWidth(tag) : 0;

  const int gap1 = 14;          // label → time
  const int gap2 = 14;          // time  → tag
  int totalW = labelW + gap1 + timeW + (tag[0] ? gap2 + tagW : 0);
  int x = (240 - totalW) / 2;
  if (x < 4) x = 4;

  // Label
  ui.tft.setTTFFont(Arial_16_Bold);
  ui.tft.setTextColor(zone.color, TFT_BLACK);
  ui.tft.setCursor(x, y0 + 8);
  ui.tft.print(zone.label);

  // Time (same font as label, white) — placed gap1 px to the right
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  ui.tft.setCursor(x + labelW + gap1, y0 + 8);
  ui.tft.print(timeStr);

  // ±1d tag — only rendered when the day differs from Asuncion
  if (tag[0]) {
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(zone.color, TFT_BLACK);
    ui.tft.setCursor(x + labelW + gap1 + timeW + gap2, y0 + 10);
    ui.tft.print(tag);
  }

  if (drawDivider) {
    ui.tft.drawFastHLine(8, y0 + 33, 224, COLOR_DGRAY);
  }
}

void drawFooter() {
  ui.tft.fillRect(0, 210, 240, 30, TFT_BLACK);
  ui.tft.drawFastHLine(0, 212, 240, COLOR_DGRAY);

  // Show LAN IP so you can find this cube on the network. The red/green
  // dot still reflects NTP-sync state — once synced it goes green and the
  // clock ticks every second from there.
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  String ipStr = (WiFi.status() == WL_CONNECTED)
    ? WiFi.localIP().toString()
    : String("(no wifi)");
  ui.tft.setCursor(8, 222);
  ui.tft.print(ipStr);

  uint16_t dotColor = ntpSynced ? COLOR_GREEN : COLOR_RED;
  ui.tft.fillCircle(228, 226, 4, dotColor);
}

void drawAll(time_t utc) {
  drawHeader();
  // Compute Asuncion's struct tm once so each row can compare against it
  // for the +1d / -1d marker without re-doing the TZ dance per row.
  setenv("TZ", "<-03>3", 1);
  tzset();
  struct tm asuTm;
  localtime_r(&utc, &asuTm);
  // 5 rows of 34px starting at y=32: 32, 66, 100, 134, 168 — bottom at 202
  for (int i = 0; i < N_ZONES; i++) {
    drawZoneRow(32 + i * 34, ZONES[i], utc, asuTm,
                /*drawDivider=*/ i < N_ZONES - 1);
  }
  drawFooter();
}

void setup() {
  Serial.begin(460800);
  Serial.println("\n=== World-Times app starting ===");

  pinMode(BL_PIN, OUTPUT);
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(BL_PIN, BL_CHANNEL);
  ledcWrite(BL_CHANNEL, 255);
  currentBrightness = 255;

  otaserver.connectWiFi();  // DO NOT EDIT.
  otaserver.run();          // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // NTP at UTC — every per-zone draw flips TZ via setenv/tzset to get local time.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  struct tm tmBuf;
  if (getLocalTime(&tmBuf, 5000)) {
    ntpSynced = true;
    Serial.printf("NTP synced (UTC base): %04d-%02d-%02d %02d:%02d:%02d\n",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
  } else {
    Serial.println("NTP sync failed — will retry");
  }

  ui.tft.fillScreen(TFT_BLACK);
  time_t now;
  time(&now);
  drawAll(now);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle();  // DO NOT EDIT

    unsigned long now = millis();

    // Redraw at 1 Hz so the seconds tick visibly. The whole render is
    // ~200 SPI writes; well within budget.
    if (now - lastDrawMs >= 1000) {
      lastDrawMs = now;

      if (!ntpSynced) {
        struct tm tmBuf;
        if (getLocalTime(&tmBuf, 100)) ntpSynced = true;
      }

      time_t utc;
      time(&utc);
      drawAll(utc);
      applyBrightness();
    }
  }

  delay(50);
}
