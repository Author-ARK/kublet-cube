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

#ifdef EMU_APP_DIR
inline void ledcSetup(uint8_t, uint32_t, uint8_t) {}
inline void ledcAttachPin(uint8_t, uint8_t) {}
inline void ledcWrite(uint8_t, uint32_t) {}
#endif

OTAServer otaserver;
KGFX ui;

#define COLOR_DGRAY 0x2104
#define COLOR_GRAY  0x4208
#define COLOR_GREEN 0x07E0
#define COLOR_RED   0xF800

// Three cities, rotated through on every minute rollover. Frankfurt and
// Berlin share the same TZ (CET/CEST), so we use the standard EU rule;
// Asuncion has been UTC-3 with no DST since Oct 2024; Colombo is +5:30
// year-round. POSIX TZ strings carry the DST rules — newlib handles it.
struct Zone {
  const char *label;
  const char *tzSpec;
  uint16_t    color;
};
static const Zone ZONES[] = {
  {"Asuncion",  "<-03>3",                       0xFD20},  // warm amber
  {"Frankfurt", "CET-1CEST,M3.5.0,M10.5.0/3",   0xFC9F},  // rose/pink
  {"Colombo",   "<+0530>-5:30",                 0xFFE0},  // soft yellow
};
static const int N_ZONES = sizeof(ZONES) / sizeof(ZONES[0]);

int currentZone = 0;
// 30-second tick counter — `utc_seconds / 30`. We rotate to the next zone
// every time this value changes, so each city stays on screen for ~30 s
// and the trio cycles through ~90 s.
long lastSeenHalfTick = -1;
bool ntpSynced = false;
uint8_t currentBrightness = 0;

// ---------------------------------------------------------------------------
// Brightness — Asuncion local schedule, matches the rest of the fleet.
// ---------------------------------------------------------------------------
uint8_t computeBrightness() {
  if (!ntpSynced) return 255;
  setenv("TZ", "<-03>3", 1);
  tzset();
  time_t now; time(&now);
  struct tm tmBuf; localtime_r(&now, &tmBuf);
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

// ---------------------------------------------------------------------------
// Full redraw for the currently-selected zone. Top bar carries that zone's
// date so the date follows whichever city is on screen.
// ---------------------------------------------------------------------------
void drawAll(time_t utc) {
  const Zone &z = ZONES[currentZone];
  setenv("TZ", z.tzSpec, 1);
  tzset();
  struct tm tmBuf;
  localtime_r(&utc, &tmBuf);

  ui.tft.fillScreen(TFT_BLACK);

  // ── Top bar: zone-local date, centered. #N badge at the right edge. ──
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  char dateStr[16];
  if (ntpSynced) {
    snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d",
             tmBuf.tm_mday, tmBuf.tm_mon + 1, tmBuf.tm_year + 1900);
  } else {
    strcpy(dateStr, "--/--/----");
  }
  int dw = ui.tft.TTFtextWidth(dateStr);
  ui.tft.setCursor((240 - dw) / 2, 6);
  ui.tft.print(dateStr);

  char numStr[8];
  snprintf(numStr, sizeof(numStr), "#%d", KUBLET_NUM);
  ui.tft.setTTFFont(Arial_12);
  int nw = ui.tft.TTFtextWidth(numStr);
  ui.tft.setCursor(238 - nw, 8);
  ui.tft.print(numStr);

  ui.tft.drawFastHLine(0, 28, 240, COLOR_DGRAY);

  // ── City name in the zone color, centered above the clock. Bumped
  //    one bold tier (Arial_18_Bold → 20_Bold, ~11% larger glyph) so
  //    the city reads as confidently as the clock below it. ───────────
  ui.tft.setTTFFont(Arial_20_Bold);
  ui.tft.setTextColor(z.color, TFT_BLACK);
  int cw = ui.tft.TTFtextWidth(z.label);
  ui.tft.setCursor((240 - cw) / 2, 40);
  ui.tft.print(z.label);

  // ── Big HH:MM clock, Arial_60_Bold, centered. Dropped one font tier
  //    from 72 because the 5-char "HH:MM" string was overflowing the
  //    240 px screen width with the larger glyphs and the trailing digit
  //    landed on the next line. 60_Bold fits ~160 px wide for 5 chars.
  ui.tft.setTTFFont(Arial_60_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  char timeStr[8];
  if (ntpSynced) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", tmBuf.tm_hour, tmBuf.tm_min);
  } else {
    strcpy(timeStr, "--:--");
  }
  int tw = ui.tft.TTFtextWidth(timeStr);
  int tx = (tw < 240) ? (240 - tw) / 2 : 0;
  // cursor.y=96 → glyph spans y≈96..156, geometric center at y=126 which
  // is just below the screen midline so the city label above breathes.
  ui.tft.setCursor(tx, 96);
  ui.tft.print(timeStr);

  // ── Footer band ────────────────────────────────────────────────────
  // Two flavours depending on which city is on screen:
  //  - ASU on top: classic footer with divider (LAN IP + green/red dot).
  //  - FRA / COL on top: divider dropped; "ASU HH:MM" rendered as large
  //    as physically possible so the home-time fills the bottom band
  //    edge-to-edge. Arial_48_Bold is the largest tier where the 9-char
  //    "ASU HH:MM" string still fits inside 240 px.
  if (currentZone == 0) {
    // ASU screen → familiar IP + dot footer
    ui.tft.drawFastHLine(0, 208, 240, COLOR_DGRAY);
    ui.tft.setTTFFont(Arial_11);
    ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
    String ipStr = (WiFi.status() == WL_CONNECTED)
      ? WiFi.localIP().toString()
      : String("(no wifi)");
    ui.tft.setCursor(8, 222);
    ui.tft.print(ipStr);
    uint16_t dotColor = ntpSynced ? COLOR_GREEN : COLOR_RED;
    ui.tft.fillCircle(228, 226, 4, dotColor);
  } else {
    // FRA / COL screen → maximum-size Asuncion home time.
    // Drop the "ASU" prefix and render just HH:MM at Arial_60_Bold —
    // matches the main clock's size and makes the home time impossible
    // to miss. The amber color (ZONES[0].color) plus the foreign-city
    // label up top is enough context that this is the Asunción time.
    char asuStr[8];
    if (ntpSynced) {
      setenv("TZ", "<-03>3", 1);
      tzset();
      struct tm asuTm;
      localtime_r(&utc, &asuTm);
      snprintf(asuStr, sizeof(asuStr), "%02d:%02d",
               asuTm.tm_hour, asuTm.tm_min);
    } else {
      strcpy(asuStr, "--:--");
    }
    // cursor.y=180 anchors the glyph's bottom edge at y≈240 (screen edge).
    // No divider — the giant amber clock owns the bottom band.
    ui.tft.setTTFFont(Arial_60_Bold);
    ui.tft.setTextColor(ZONES[0].color, TFT_BLACK);  // ASU amber
    int aw = ui.tft.TTFtextWidth(asuStr);
    int ax = (aw < 240) ? (240 - aw) / 2 : 0;
    ui.tft.setCursor(ax, 180);
    ui.tft.print(asuStr);
  }
}

void setup() {
  Serial.begin(460800);
  Serial.println("\n=== Digital-Clock starting ===");

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

  // NTP at UTC — each render flips TZ for whichever zone is displayed.
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

  time_t utc; time(&utc);
  drawAll(utc);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle();  // DO NOT EDIT
  }

  static unsigned long lastTickMs = 0;
  unsigned long now = millis();

  // Polling cadence: 250 ms is enough to catch the :00 second boundary
  // within a quarter-second of when it actually flips. Display itself
  // only redraws on minute change (HH:MM precision) or initial NTP sync.
  if (now - lastTickMs >= 250) {
    lastTickMs = now;

    if (!ntpSynced) {
      struct tm tmBuf;
      if (getLocalTime(&tmBuf, 100)) {
        ntpSynced = true;
        time_t utc; time(&utc);
        drawAll(utc);
      }
      return;
    }

    // Detect a 30-second rollover. utc_seconds / 30 gives a monotonic
    // tick that advances on every :00 and :30 mark — the cube rotates
    // to the next city on each tick change. UTC-based so we don't need
    // to set the TZ env first.
    time_t utc; time(&utc);
    long curHalfTick = (long)(utc / 30);
    if (curHalfTick != lastSeenHalfTick) {
      // First time through, just record the tick. On subsequent ticks,
      // advance to the next city.
      if (lastSeenHalfTick != -1) {
        currentZone = (currentZone + 1) % N_ZONES;
      }
      lastSeenHalfTick = curHalfTick;
      drawAll(utc);
      applyBrightness();
    }
  }

  delay(20);
}
