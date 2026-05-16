#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
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
WiFiClientSecure secureClient;

static const uint32_t REFRESH_FALLBACK_MS = 65UL * 1000;

unsigned long lastFetchMs = 0;
unsigned long lastClockRedrawMs = 0;
float lastBtcPrice = NAN;
float lastEthPrice = NAN;
// Direction over the last 1-min tick: +1 up, -1 down, 0 unchanged. Set by
// comparing each new fetch against the previously displayed price; arrows
// in the UI use these (green up / red down / gray flat).
int btcDirection = 0;
int ethDirection = 0;
time_t lastFetchEpoch = 0;
bool lastFetchOk = false;
bool ntpSynced = false;
uint8_t currentBrightness = 0;
int lastFetchMinute = -1;

#define COLOR_DGRAY 0x2104
#define COLOR_GRAY  0x4208
#define COLOR_GREEN 0x07E0
#define COLOR_RED   0xF800
// 1-minute direction tints for the price text. Bright (near-white)
// luminance with a subtle green/red bias — see the tick at a glance
// without losing the per-coin label color.
#define COLOR_TINT_UP   0xC7FA  // bright white with a green hint
#define COLOR_TINT_DOWN 0xFED8  // bright white with a red hint
#define COLOR_BTC   0xFD20   // bitcoin orange
#define COLOR_ETH   0x649F   // ethereum cool blue/violet

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

// Binance public ticker. Response is ~50 bytes JSON:
//   {"symbol":"BTCUSDT","price":"43210.50000000"}
// No auth, no headers needed. Rate-limit headroom is huge at one call per
// 60s. We parse with substring rather than ArduinoJson to keep heap pressure
// trivial and have a tighter error path on partial bodies.
float fetchBinancePrice(const char *symbol) {
  secureClient.setInsecure();
  HTTPClient http;
  String url = String("https://api.binance.com/api/v3/ticker/price?symbol=") + symbol;
  http.begin(secureClient, url);
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.setTimeout(8000);

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

  int p = body.indexOf("\"price\":\"");
  if (p < 0) {
    Serial.println("  no price field");
    return NAN;
  }
  p += 9;
  int e = body.indexOf('"', p);
  if (e < 0) return NAN;
  String priceStr = body.substring(p, e);
  float price = priceStr.toFloat();
  Serial.printf("  %s = $%.4f\n", symbol, price);
  return price;
}

float fetchBtcPrice() { return fetchBinancePrice("BTCUSDT"); }
float fetchEthPrice() { return fetchBinancePrice("ETHUSDT"); }

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
  const char *title = "BTC / ETH  -  CRYPTO";
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

// Small arrow at the right end of a coin row, indicating last-minute price
// direction. Up = green, down = red, flat = small gray bar.
static void drawDirectionGlyph(int x, int y, int dir) {
  if (dir > 0) {
    ui.tft.fillTriangle(x, y + 12, x + 14, y + 12, x + 7, y, COLOR_GREEN);
  } else if (dir < 0) {
    ui.tft.fillTriangle(x, y, x + 14, y, x + 7, y + 12, COLOR_RED);
  } else {
    ui.tft.fillRect(x + 1, y + 5, 12, 2, COLOR_GRAY);
  }
}

void drawCoinRow(int y0, const char *label, uint16_t labelColor, float price, int direction) {
  ui.tft.fillRect(0, y0, 240, 82, TFT_BLACK);

  // Label ~10% smaller than the metals app (Arial_20_Bold vs 24_Bold).
  ui.tft.setTTFFont(Arial_20_Bold);
  ui.tft.setTextColor(labelColor, TFT_BLACK);
  ui.tft.setCursor(10, y0 + 20);
  ui.tft.print(label);

  int labelW = ui.tft.TTFtextWidth(label);
  const int leftMargin   = 10;
  const int gapAfterLabel = 20;  // ~2 glyphs at Arial_20_Bold
  const int rightMargin  = 8;
  const int arrowAreaW   = 18;   // reserved at the far right for the tick arrow
  int reservedLeft = leftMargin + labelW + gapAfterLabel;

  // No fractional cents for big numbers — BTC and ETH always sit above $1k,
  // so this branch shows whole dollars there. Sub-$1k falls back to two
  // decimals so the row still reads usefully for any coin you swap in.
  char buf[20];
  if (isnan(price))           strcpy(buf, "--");
  else if (price >= 1000.0f)  formatPriceEU(buf, sizeof(buf), price, 0);
  else                        formatPriceEU(buf, sizeof(buf), price, 2);

  // Price ~14% smaller (Arial_24_Bold vs 28_Bold) — close enough to the
  // requested ~10%, and keeps a sane visual gap from the smaller label.
  ui.tft.setTTFFont(Arial_24_Bold);
  // Last-minute direction tints the PRICE only — the label
  // stays in labelColor so the coin/metal is still identifiable.
  // Default price color is now plain white; tint only deviates on
  // a 1-min direction signal so the last tick stands out.
  uint16_t priceColor = TFT_WHITE;
  if      (direction > 0) priceColor = COLOR_TINT_UP;
  else if (direction < 0) priceColor = COLOR_TINT_DOWN;
  ui.tft.setTextColor(priceColor, TFT_BLACK);
  int pw = ui.tft.TTFtextWidth(buf);
  int rightArea = 240 - reservedLeft - rightMargin - arrowAreaW;
  int priceX = reservedLeft + (rightArea - pw) / 2;
  if (priceX < reservedLeft) priceX = reservedLeft;
  ui.tft.setCursor(priceX, y0 + 16);
  ui.tft.print(buf);

  // 1-min tick direction arrow, far right, vertically centered with the price.
  int arrowX = 240 - rightMargin - 14;
  int arrowY = y0 + 20;
  drawDirectionGlyph(arrowX, arrowY, direction);

  ui.tft.setTTFFont(Arial_11);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  const char *unit = "USD (Binance spot)";
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

  uint16_t dotColor = fetchOk ? COLOR_GREEN : COLOR_RED;
  ui.tft.fillCircle(228, 226, 4, dotColor);
}

void drawUI(float btc, float eth, time_t lastFetch, bool fetchOk) {
  ui.tft.fillScreen(TFT_BLACK);
  drawHeader();
  drawCoinRow(48,  "BTC-USD", COLOR_BTC, btc, btcDirection);
  ui.tft.drawFastHLine(0, 130, 240, COLOR_DGRAY);
  drawCoinRow(132, "ETH-USD", COLOR_ETH, eth, ethDirection);
  drawFooter(fetchOk, lastFetch);
}

void setup() {
  Serial.begin(460800);
  Serial.println("\n=== crypto BTC/ETH app starting ===");

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
      float btc = fetchBtcPrice();
      float eth = fetchEthPrice();
      bool ok = !isnan(btc) && !isnan(eth);
      // Update direction *before* overwriting the previous price. Use a
      // small epsilon so floating-point noise on identical successive
      // prices doesn't flicker the arrow.
      if (!isnan(btc)) {
        if (!isnan(lastBtcPrice)) {
          if      (btc > lastBtcPrice + 0.005f) btcDirection = +1;
          else if (btc < lastBtcPrice - 0.005f) btcDirection = -1;
          else                                  btcDirection = 0;
        }
        lastBtcPrice = btc;
      }
      if (!isnan(eth)) {
        if (!isnan(lastEthPrice)) {
          if      (eth > lastEthPrice + 0.005f) ethDirection = +1;
          else if (eth < lastEthPrice - 0.005f) ethDirection = -1;
          else                                  ethDirection = 0;
        }
        lastEthPrice = eth;
      }
      lastFetchOk = ok;
      if (ok) time(&lastFetchEpoch);
      lastFetchMs = now;

      if (!ntpSynced) {
        struct tm tmBuf;
        if (getLocalTime(&tmBuf, 100)) ntpSynced = true;
      }
      applyBrightness();

      drawUI(lastBtcPrice, lastEthPrice, lastFetchEpoch, lastFetchOk);
    }

    if (now - lastClockRedrawMs >= 1000) {
      lastClockRedrawMs = now;
      drawDateTime(time(nullptr));
      applyBrightness();
    }
  }

  delay(10);
}
