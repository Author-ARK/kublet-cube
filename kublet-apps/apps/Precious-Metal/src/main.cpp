#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#define BUTTON_PIN 19
#define BL_PIN 15
#define BL_CHANNEL 0

// Per-device identifier shown in upper-right corner. Override via build flag
// -DKUBLET_NUM=2 (etc.) when flashing a different unit, or wire to NVS later.
#ifndef KUBLET_NUM
#define KUBLET_NUM 1
#endif

// ESP32 ledc PWM not available in the desktop emulator (EMU_APP_DIR is only
// defined when CMake builds against the SDL2 mocks). Provide no-op stubs so
// the same source compiles for both targets.
#ifdef EMU_APP_DIR
inline void ledcSetup(uint8_t, uint32_t, uint8_t) {}
inline void ledcAttachPin(uint8_t, uint8_t) {}
inline void ledcWrite(uint8_t, uint32_t) {}
#endif

OTAServer otaserver;
KGFX ui;
WiFiClientSecure secureClient;

// Refresh on every full-minute tick (XX:00, XX:01, …). The cube hits Stooq
// twice per cycle (XAGUSD + XAUUSD); each response is ~100 bytes of CSV so
// heap pressure is negligible.
static const uint32_t REFRESH_FALLBACK_MS = 65UL * 1000;  // safety net when NTP isn't synced yet

unsigned long lastFetchMs = 0;
unsigned long lastClockRedrawMs = 0;
float lastAgPrice = NAN;
float lastAuPrice = NAN;
// 1-minute direction tick per metal: +1 up, -1 down, 0 flat. Drives the
// green/red price-text tint in drawMetalRow().
int agDirection = 0;
int auDirection = 0;
time_t lastFetchEpoch = 0;
bool lastFetchOk = false;
bool ntpSynced = false;
uint8_t currentBrightness = 0;
int lastFetchMinute = -1;  // -1 means "never fetched"; otherwise the minute-of-hour we last fetched in

#define COLOR_DGRAY  0x2104
#define COLOR_GRAY   0x4208
#define COLOR_GREEN  0x07E0
#define COLOR_RED    0xF800
#define COLOR_SILVER 0xC618   // light gray-silver
#define COLOR_GOLD   0xFEA0   // warm gold
// 1-minute direction tints for the price text (white-with-a-hint).
#define COLOR_TINT_UP   0xC7FA
#define COLOR_TINT_DOWN 0xFED8

// Brightness schedule for Asuncion local time (UTC-3, no DST since Oct 2024)
uint8_t computeBrightness() {
  if (!ntpSynced) return 255;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) return 255;
  int h = timeinfo.tm_hour;
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

// Fetch the latest "Close" price from Stooq's CSV endpoint. The response is
// two lines (~100 bytes total):
//   Symbol,Date,Time,Open,High,Low,Close,Volume
//   XAGUSD,2026-05-15,19:52:28,33.377,33.88,32.812,33.495,
// We parse field index 6 (Close) of the data line. Returns NAN on any failure
// (HTTP error, malformed body, "N/D" placeholder), and Serial-prints what
// happened so the failure is visible in the webui's emulator log.
float fetchStooqPrice(const char* symbol) {
  secureClient.setInsecure();
  HTTPClient http;
  String url = String("https://stooq.com/q/l/?s=") + symbol + "&f=sd2t2ohlcv&h&e=csv";
  http.begin(secureClient, url);
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.setTimeout(10000);

  Serial.printf("Fetching %s (heap free %u)\n", symbol, ESP.getFreeHeap());
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("  HTTP %d\n", code);
    http.end();
    return NAN;
  }

  String body = http.getString();
  http.end();
  Serial.printf("  body %d bytes\n", body.length());

  int nl = body.indexOf('\n');
  if (nl < 0) {
    Serial.println("  no newline");
    return NAN;
  }
  int pos = nl + 1;
  for (int i = 0; i < 6; i++) {
    int c = body.indexOf(',', pos);
    if (c < 0) {
      Serial.printf("  missing field %d\n", i);
      return NAN;
    }
    pos = c + 1;
  }
  int endComma = body.indexOf(',', pos);
  String priceStr = (endComma > 0) ? body.substring(pos, endComma) : body.substring(pos);
  priceStr.trim();
  if (priceStr.length() == 0 || priceStr.equalsIgnoreCase("N/D")) {
    Serial.println("  no data (market closed / unknown symbol)");
    return NAN;
  }
  float price = priceStr.toFloat();
  Serial.printf("  %s = $%.4f / oz\n", symbol, price);
  return price;
}

float fetchSilverPrice() { return fetchStooqPrice("xagusd"); }
float fetchGoldPrice()   { return fetchStooqPrice("xauusd"); }

// Format `price` with European thousands dots ("1.234.567") and a comma as
// the decimal separator ("33,49"). No locale required — ESP32 newlib's
// locale support is patchy. Caller picks the number of decimals.
static void formatPriceEU(char *out, size_t cap, float price, int decimals) {
  char fmt[16], raw[32];
  snprintf(fmt, sizeof(fmt), "%%.%df", decimals);
  snprintf(raw, sizeof(raw), fmt, price);
  const char *dot = strchr(raw, '.');
  int rlen = (int)strlen(raw);
  int int_end = dot ? (int)(dot - raw) : rlen;
  int dec_off = dot ? int_end + 1 : rlen;
  int dec_len = dot ? rlen - dec_off : 0;
  int sign = (raw[0] == '-') ? 1 : 0;
  int digits = int_end - sign;
  int seps = digits > 0 ? (digits - 1) / 3 : 0;
  int oi = 0;
  if (sign && oi < (int)cap - 1) out[oi++] = '-';
  int first = digits - seps * 3;
  for (int i = 0; i < first && oi < (int)cap - 1; i++) out[oi++] = raw[sign + i];
  for (int g = 0; g < seps; g++) {
    if (oi < (int)cap - 1) out[oi++] = '.';
    for (int k = 0; k < 3 && oi < (int)cap - 1; k++)
      out[oi++] = raw[sign + first + g * 3 + k];
  }
  if (decimals > 0 && oi < (int)cap - 1) {
    out[oi++] = ',';
    for (int i = 0; i < dec_len && oi < (int)cap - 1; i++)
      out[oi++] = raw[dec_off + i];
  }
  out[oi] = '\0';
}

void drawDateTime(time_t now) {
  ui.tft.fillRect(0, 26, 240, 16, TFT_BLACK);

  if (now == 0 || !ntpSynced) {
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
    const char *msg = "syncing time...";
    int w = ui.tft.TTFtextWidth(msg);
    ui.tft.setCursor((240 - w) / 2, 28);
    ui.tft.print(msg);
    return;
  }

  struct tm tmBuf;
  localtime_r(&now, &tmBuf);
  char dt[40];
  snprintf(dt, sizeof(dt), "%04d-%02d-%02d  %02d:%02d:%02d",
           tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
           tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  int w = ui.tft.TTFtextWidth(dt);
  ui.tft.setCursor((240 - w) / 2, 28);
  ui.tft.print(dt);
}

void drawHeader() {
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  const char *title = "AG / AU  -  METALS";
  int w = ui.tft.TTFtextWidth(title);
  ui.tft.setCursor((240 - w) / 2, 6);
  ui.tft.print(title);

  char numStr[8];
  snprintf(numStr, sizeof(numStr), "#%d", KUBLET_NUM);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  int nw = ui.tft.TTFtextWidth(numStr);
  ui.tft.setCursor(238 - nw, 6);
  ui.tft.print(numStr);

  drawDateTime(time(nullptr));

  ui.tft.drawFastHLine(0, 46, 240, COLOR_DGRAY);
}

// Render one metal half: a label ("Ag"/"Au") in the metal's color on the
// left, the big price in that same color to its right (centered in the
// remaining area, with a 2-glyph gap so they never overlap), and a tiny
// "USD/oz" hint below. The whole region is cleared first so refreshes draw
// cleanly without ghosting.
void drawMetalRow(int y0, const char *label, uint16_t labelColor, float price, int direction) {
  ui.tft.fillRect(0, y0, 240, 82, TFT_BLACK);

  // Label on left, in the metal's color
  ui.tft.setTTFFont(Arial_24_Bold);
  ui.tft.setTextColor(labelColor, TFT_BLACK);
  ui.tft.setCursor(10, y0 + 18);
  ui.tft.print(label);

  int labelW = ui.tft.TTFtextWidth(label);
  // Reserve: 10 px left margin + label width + ~24 px gap (≈ 2 letters at
  // Arial_24_Bold). Price is centered in whatever's left, in the metal color.
  const int leftMargin = 10;
  const int gapAfterLabel = 24;
  int reservedLeft = leftMargin + labelW + gapAfterLabel;

  char buf[16];
  if (isnan(price))            strcpy(buf, "--.--");
  else if (price >= 1000.0f)  formatPriceEU(buf, sizeof(buf), price, 0);
  else                        formatPriceEU(buf, sizeof(buf), price, 2);

  // Default price color is plain white; the 1-min direction tint
  // overrides it on up/down ticks. Label stays in labelColor below
  // so the metal is identifiable.
  uint16_t priceColor = TFT_WHITE;
  if      (direction > 0) priceColor = COLOR_TINT_UP;
  else if (direction < 0) priceColor = COLOR_TINT_DOWN;
  ui.tft.setTTFFont(Arial_28_Bold);
  ui.tft.setTextColor(priceColor, TFT_BLACK);
  int pw = ui.tft.TTFtextWidth(buf);
  int rightArea = 240 - reservedLeft - 8;  // 8 px right margin
  int priceX = reservedLeft + (rightArea - pw) / 2;
  if (priceX < reservedLeft) priceX = reservedLeft;  // clamp if price is huge
  ui.tft.setCursor(priceX, y0 + 14);
  ui.tft.print(buf);

  // Small unit caption
  ui.tft.setTTFFont(Arial_11);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  const char *unit = "USD per troy ounce";
  int uw = ui.tft.TTFtextWidth(unit);
  ui.tft.setCursor((240 - uw) / 2, y0 + 58);
  ui.tft.print(unit);
}

void drawFooter(bool fetchOk, time_t lastEpoch) {
  ui.tft.fillRect(0, 210, 240, 30, TFT_BLACK);
  ui.tft.drawFastHLine(0, 212, 240, COLOR_DGRAY);

  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  // lastEpoch retained on the signature for source-compat with the dual-
  // panel apps, but no longer rendered — the red/green dot below already
  // ticks each minute as proof of life. We show the cube's LAN IP here
  // instead so you can find it without walking to the router page.
  (void)lastEpoch;
  String ipStr = (WiFi.status() == WL_CONNECTED)
    ? WiFi.localIP().toString()
    : String("(no wifi)");
  ui.tft.setCursor(8, 222);
  ui.tft.print(ipStr);

  // Green when last fetch hit both Stooq endpoints successfully, red otherwise.
  uint16_t dotColor = fetchOk ? COLOR_GREEN : COLOR_RED;
  ui.tft.fillCircle(228, 226, 4, dotColor);
}

void drawUI(float ag, float au, time_t lastFetch, bool fetchOk) {
  ui.tft.fillScreen(TFT_BLACK);
  drawHeader();
  // Pass per-metal direction so the price text tints on the last 1-min tick.
  drawMetalRow(48,  "Ag-USD", COLOR_SILVER, ag, agDirection);
  ui.tft.drawFastHLine(0, 130, 240, COLOR_DGRAY);
  drawMetalRow(132, "Au-USD", COLOR_GOLD,   au, auDirection);
  drawFooter(fetchOk, lastFetch);
}

void setup() {
  Serial.begin(460800);
  Serial.println("\n=== Metals (Ag/Au) app starting ===");

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

  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  struct tm tmBuf;
  if (getLocalTime(&tmBuf, 5000)) {
    ntpSynced = true;
    Serial.printf("NTP synced: %04d-%02d-%02d %02d:%02d (-03 Asuncion)\n",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min);
  } else {
    Serial.println("NTP sync failed");
  }

  drawUI(NAN, NAN, 0, false);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle();  // DO NOT EDIT

    unsigned long now = millis();

    // Trigger a fetch on first run, then once per even-minute mark (00, 02, …).
    // If NTP isn't synced we fall back to a millis-based heartbeat so the cube
    // still tries to fetch even with bad time data.
    bool shouldFetch = false;
    if (lastFetchMs == 0) {
      shouldFetch = true;
    } else if (ntpSynced) {
      struct tm tmBuf;
      if (getLocalTime(&tmBuf, 50)) {
        int currentMinute = tmBuf.tm_min;
        // Fire on every full-minute tick (within the first 20s window) so
        // the displayed price is at most ~60s stale.
        if (currentMinute != lastFetchMinute && tmBuf.tm_sec < 20) {
          shouldFetch = true;
          lastFetchMinute = currentMinute;
        }
      }
    } else if (now - lastFetchMs > REFRESH_FALLBACK_MS) {
      shouldFetch = true;
    }

    if (shouldFetch) {
      float ag = fetchSilverPrice();
      float au = fetchGoldPrice();
      bool ok = !isnan(ag) && !isnan(au);
      // Direction is computed before we overwrite the previous price.
      // Epsilon of 0.005 USD avoids flicker on float-noise no-change ticks.
      if (!isnan(ag)) {
        if (!isnan(lastAgPrice)) {
          if      (ag > lastAgPrice + 0.005f) agDirection = +1;
          else if (ag < lastAgPrice - 0.005f) agDirection = -1;
          else                                 agDirection = 0;
        }
        lastAgPrice = ag;
      }
      if (!isnan(au)) {
        if (!isnan(lastAuPrice)) {
          if      (au > lastAuPrice + 0.05f) auDirection = +1;
          else if (au < lastAuPrice - 0.05f) auDirection = -1;
          else                                auDirection = 0;
        }
        lastAuPrice = au;
      }
      lastFetchOk = ok;
      if (ok) time(&lastFetchEpoch);
      lastFetchMs = now;

      if (!ntpSynced) {
        struct tm tmBuf;
        if (getLocalTime(&tmBuf, 100)) ntpSynced = true;
      }
      applyBrightness();

      drawUI(lastAgPrice, lastAuPrice, lastFetchEpoch, lastFetchOk);
    }

    if (now - lastClockRedrawMs >= 1000) {
      lastClockRedrawMs = now;
      drawDateTime(time(nullptr));
      applyBrightness();
    }
  }

  delay(10);
}
