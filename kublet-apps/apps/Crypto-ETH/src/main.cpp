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

// === Per-app symbol/branding =================================================
// To clone this app for ETH/XRP/SOL: change these three constants and the
// COIN_COLOR define. Everything else is shared.
static const char *COIN_TICKER = "ETH-USD";
static const char *COIN_SYMBOL = "ETHUSDT";
#define COIN_COLOR 0x649F  // ethereum cool blue/violet
// =============================================================================

OTAServer otaserver;
KGFX ui;
WiFiClientSecure secureClient;

static const uint32_t REFRESH_FALLBACK_MS = 65UL * 1000;
static const int N_CANDLES = 60;  // 60 × 1-minute candles = 1 hour

unsigned long lastFetchMs = 0;
unsigned long lastClockRedrawMs = 0;
float closes[N_CANDLES];  // close prices, oldest at [0], newest at [N_CANDLES-1]
int filled = 0;           // how many of N_CANDLES are populated (after a fresh boot we have 0 until the first fetch)
time_t lastFetchEpoch = 0;
bool lastFetchOk = false;
bool ntpSynced = false;
uint8_t currentBrightness = 0;
int lastFetchMinute = -1;
// Direction of the most recent 1-minute close vs the previous close.
// Used by drawPrice() to tint the font green/red.
int priceDir = 0;

#define COLOR_DGRAY 0x2104
#define COLOR_GRAY  0x4208
#define COLOR_GREEN 0x07E0
#define COLOR_RED   0xF800
#define COLOR_DGREEN 0x0320  // dim green for chart fill
#define COLOR_DRED   0x6000  // dim red for chart fill
// 1-minute direction tints for the price text. Bright (near-white)
// luminance with a subtle green/red bias so the last-minute move is
// readable at a glance without competing with the coin color elsewhere.
#define COLOR_TINT_UP   0xC7FA  // bright white with a green hint
#define COLOR_TINT_DOWN 0xFED8  // bright white with a red hint

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

// Fetch 60 1-min klines from Binance. Response shape:
//   [
//     [openTime, "open", "high", "low", "close", ...],
//     ...
//   ]
// We only care about the close value (index 4) of each candle. Body is
// ~5-7 KB which is comfortably within ESP32 heap budget alongside TLS.
// Returns true on success and writes prices into `closes[]` left-to-right.
bool fetchKlines() {
  secureClient.setInsecure();
  HTTPClient http;
  String url = String("https://api.binance.com/api/v3/klines?symbol=")
             + COIN_SYMBOL + "&interval=1m&limit=" + String(N_CANDLES);
  http.begin(secureClient, url);
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.setTimeout(10000);

  Serial.printf("Fetching klines %s (heap free %u)\n", COIN_SYMBOL, ESP.getFreeHeap());
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("  HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  Serial.printf("  body %d bytes\n", body.length());

  // Walk the body candle-by-candle. Each candle starts with '[' and the
  // close price is the 5th comma-separated field, quoted:
  //   [1700000000000,"43100.20","43150.00","43050.00","43120.50", ...]
  int pos = 0;
  int written = 0;
  while (written < N_CANDLES) {
    int candleStart = body.indexOf('[', pos);
    if (candleStart < 0) break;
    if (candleStart == 0 || body[candleStart - 1] != ',') {
      // first '[' is the outer array — skip it
      if (written == 0 && candleStart == 0) { pos = candleStart + 1; continue; }
    }
    // Skip 4 commas to get to the close field
    int p = candleStart + 1;
    for (int i = 0; i < 4; i++) {
      int c = body.indexOf(',', p);
      if (c < 0) { p = -1; break; }
      p = c + 1;
    }
    if (p < 0) break;
    // Skip optional whitespace after the comma (real Binance has
    // none, but a pretty-printed JSON fixture would).
    while (p < (int)body.length() &&
           (body[p] == ' ' || body[p] == '\t')) p++;
    // p now points at `"close"` — strip the opening quote
    if (body[p] == '"') p++;
    int endQuote = body.indexOf('"', p);
    if (endQuote < 0) break;
    closes[written++] = body.substring(p, endQuote).toFloat();
    pos = endQuote + 1;
  }

  if (written < N_CANDLES) {
    Serial.printf("  parsed only %d candles, bailing\n", written);
    return false;
  }
  Serial.printf("  parsed %d candles, last=$%.2f\n", written, closes[N_CANDLES - 1]);
  return true;
}

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
  ui.tft.fillRect(0, 26, 240, 14, TFT_BLACK);
  if (now == 0 || !ntpSynced) {
    ui.tft.setTTFFont(Arial_11);
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
  ui.tft.setTTFFont(Arial_11);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  int w = ui.tft.TTFtextWidth(dt);
  ui.tft.setCursor((240 - w) / 2, 28);
  ui.tft.print(dt);
}

void drawHeader(float deltaPct) {
  ui.tft.fillRect(0, 0, 240, 24, TFT_BLACK);

  // Ticker on left in coin color
  ui.tft.setTTFFont(Arial_18_Bold);
  ui.tft.setTextColor(COIN_COLOR, TFT_BLACK);
  ui.tft.setCursor(8, 4);
  ui.tft.print(COIN_TICKER);

  // Kublet number first — right-anchored at the screen edge — so we
  // know its left edge before placing the 1h delta % beside it.
  char numStr[8];
  snprintf(numStr, sizeof(numStr), "#%d", KUBLET_NUM);
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  int nw = ui.tft.TTFtextWidth(numStr);
  int nx = 238 - nw;
  ui.tft.setCursor(nx, 6);
  ui.tft.print(numStr);

  // 1h delta % right-anchored against #N, with ~one Arial_14_Bold
  // letter of breathing room so the two never visually touch.
  if (!isnan(deltaPct)) {
    char dbuf[16];
    snprintf(dbuf, sizeof(dbuf), "%+0.2f%%", (double)deltaPct);
    ui.tft.setTTFFont(Arial_14_Bold);
    ui.tft.setTextColor(deltaPct >= 0 ? COLOR_GREEN : COLOR_RED, TFT_BLACK);
    int dw = ui.tft.TTFtextWidth(dbuf);
    const int letterGap = 12;  // ≈ one Arial_14_Bold glyph wide
    int dx = nx - letterGap - dw;
    if (dx < 8) dx = 8;  // never overlap the ticker on the left
    ui.tft.setCursor(dx, 6);
    ui.tft.print(dbuf);
  }

  drawDateTime(time(nullptr));
  ui.tft.drawFastHLine(0, 42, 240, COLOR_DGRAY);
}

// Big centered price in the coin color, always two decimals. Fixed
// Arial_48_Bold across BTC/ETH/XRP/SOL for consistent layout — sub-$10
// coins render with whitespace around, multi-thousand prices fill near
// the full width.
void drawPrice(float price, int direction) {
  // Price sits just above the IP-line footer (y=210). With Arial_48_Bold
  // glyphs at ~48 px tall and cursor.y=158, the price spans y=158..206 —
  // leaving the upper portion of the screen free for the 1h chart.
  ui.tft.fillRect(0, 152, 240, 58, TFT_BLACK);

  char buf[20];
  if (isnan(price)) strcpy(buf, "--.--");
  else              formatPriceEU(buf, sizeof(buf), price, 2);

  // Default price color is now plain white; tint only deviates on
  // a 1-min direction signal so the last tick stands out.
  uint16_t color = TFT_WHITE;
  if      (direction > 0) color = COLOR_TINT_UP;
  else if (direction < 0) color = COLOR_TINT_DOWN;

  ui.tft.setTTFFont(Arial_48_Bold);
  ui.tft.setTextColor(color, TFT_BLACK);
  int pw = ui.tft.TTFtextWidth(buf);
  int x = (pw < 240) ? (240 - pw) / 2 : 0;
  ui.tft.setCursor(x, 158);
  ui.tft.print(buf);
}

// Delta-from-baseline filled chart in the area y=120..205. Baseline is
// the first kline's close (i.e., the price 1h ago). For each minute
// we draw a vertical line from baseline to the close — green when above,
// red when below. Y-scale is symmetric: largest excursion (either side)
// becomes the half-height of the chart so a tiny range still looks lively.
void drawChart() {
  // Big 3-letter ticker badge on the left, sits 5 px above the
  // chart's baseline so the row reads as that asset at a glance.
  // Chart_x is shifted right past the badge so bars never collide.
  const int chart_x = 8;
  const int chart_y = 48;
  const int chart_w = 224;
  const int chart_h = 104;
  const int baseline_y = chart_y + chart_h / 2;

  ui.tft.fillRect(0, chart_y - 2, 240, chart_h + 4, TFT_BLACK);
  if (filled < 2) {
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
    const char *msg = "1h chart loading...";
    int w = ui.tft.TTFtextWidth(msg);
    ui.tft.setCursor((240 - w) / 2, baseline_y - 6);
    ui.tft.print(msg);
    return;
  }

  float baseline = closes[0];
  float maxAbs = 0.0f;
  for (int i = 1; i < filled; i++) {
    float d = closes[i] - baseline;
    if (fabsf(d) > maxAbs) maxAbs = fabsf(d);
  }
  if (maxAbs <= 0.0f) maxAbs = 1.0f;  // flat market — render baseline only

  // Baseline reference line
  ui.tft.drawFastHLine(chart_x, baseline_y, chart_w, COLOR_DGRAY);

  // Each candle gets chart_w / N_CANDLES px wide (rounded down). For 224 / 60
  // that's 3 px, leaving 44 px of slack which we waste as a right margin —
  // acceptable for v1.
  int bar_w = chart_w / N_CANDLES;
  if (bar_w < 1) bar_w = 1;

  for (int i = 0; i < filled; i++) {
    float delta = closes[i] - baseline;
    int pixels = (int)((fabsf(delta) / maxAbs) * (chart_h / 2));
    if (pixels < 1 && delta != 0.0f) pixels = 1;
    int x = chart_x + i * bar_w;
    // Bright tick LINE at the candle's edge (where the value lands);
    // the area between the tick and the baseline is filled with a
    // sparse dotted raster so the bar reads as a 'cloud' of the same
    // color without dominating the screen.
    if (delta >= 0) {
      int top_y = baseline_y - pixels;
      ui.tft.drawFastHLine(x, top_y, bar_w - 1, COLOR_GREEN);
      for (int yy = top_y + 1; yy < baseline_y; yy++) {
        for (int xx = x; xx < x + bar_w - 1; xx++) {
          if (((xx + yy) & 1) == 0) ui.tft.drawPixel(xx, yy, COLOR_DGREEN);
        }
      }
    } else {
      int bot_y = baseline_y + pixels;
      ui.tft.drawFastHLine(x, bot_y, bar_w - 1, COLOR_RED);
      for (int yy = baseline_y + 1; yy < bot_y; yy++) {
        for (int xx = x; xx < x + bar_w - 1; xx++) {
          if (((xx + yy) & 1) == 0) ui.tft.drawPixel(xx, yy, COLOR_DRED);
        }
      }
    }
  }

  // Big ticker badge — drawn AFTER the bars so it overlays them at
  // the upper-left. cursor.y=47 puts the glyph's top edge 5 px below
  // the header divider line at y=42.
  // Badge font dropped from Arial_48_Bold to Arial_32_Bold — exactly
  // 2/3 of the original height so the chart breathes more behind it.
  ui.tft.setTTFFont(Arial_32_Bold);
  // Fixed gold for every solo app's badge — visual signature shared
  // across BTC/ETH/XRP/SOL/Au/Ag rather than per-coin tint.
  ui.tft.setTextColor(0xFEA0, TFT_BLACK);
  ui.tft.setCursor(4, 47);
  ui.tft.print("ETH");
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

void drawUI(float deltaPct, bool fetchOk, time_t lastEpoch) {
  ui.tft.fillScreen(TFT_BLACK);
  drawHeader(deltaPct);
  drawPrice(filled > 0 ? closes[filled - 1] : NAN, priceDir);
  drawChart();
  drawFooter(fetchOk, lastEpoch);
}

void setup() {
  Serial.begin(460800);
  Serial.print("\n=== ");
  Serial.print(COIN_TICKER);
  Serial.println(" solo app starting ===");

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

  for (int i = 0; i < N_CANDLES; i++) closes[i] = NAN;

  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm tmBuf;
  if (getLocalTime(&tmBuf, 5000)) {
    ntpSynced = true;
    Serial.printf("NTP synced: %04d-%02d-%02d %02d:%02d\n",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min);
  } else {
    Serial.println("NTP sync failed");
  }

  drawUI(NAN, false, 0);
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
        if (currentMinute != lastFetchMinute && tmBuf.tm_sec < 20) {
          shouldFetch = true;
          lastFetchMinute = currentMinute;
        }
      }
    } else if (now - lastFetchMs > REFRESH_FALLBACK_MS) {
      shouldFetch = true;
    }

    if (shouldFetch) {
      bool ok = fetchKlines();
      lastFetchOk = ok;
      if (ok) {
        filled = N_CANDLES;
        time(&lastFetchEpoch);
        // Direction tint from the last two 1-min closes
        float a = closes[N_CANDLES - 2], b = closes[N_CANDLES - 1];
        if      (b > a + 0.00005f * b) priceDir = +1;
        else if (b < a - 0.00005f * b) priceDir = -1;
        else                            priceDir = 0;
      }
      lastFetchMs = now;

      if (!ntpSynced) {
        struct tm tmBuf;
        if (getLocalTime(&tmBuf, 100)) ntpSynced = true;
      }
      applyBrightness();

      float deltaPct = NAN;
      if (filled >= 2 && closes[0] > 0.0f) {
        deltaPct = (closes[filled - 1] - closes[0]) / closes[0] * 100.0f;
      }
      drawUI(deltaPct, lastFetchOk, lastFetchEpoch);
    }

    if (now - lastClockRedrawMs >= 1000) {
      lastClockRedrawMs = now;
      drawDateTime(time(nullptr));
      applyBrightness();
    }
  }

  delay(10);
}
