#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;
WiFiClientSecure secureClient;
bool buttonWasPressed = false;

int refreshTimeInSeconds = 300;
unsigned long lastTime = 0;
bool ntpSynced = false;

// --- Ticker watchlist ---
static const char *TICKERS[] = {"VOO", "VGT", "VTI", "VXUS", "AAPL", "NVDA", "GOOG", "AMZN", "^VIX", "BTC-USD"};
#define NUM_TICKERS (sizeof(TICKERS) / sizeof(TICKERS[0]))
int currentTicker = 0;

// Chart data per ticker
#define MAX_POINTS 78

struct TickerData {
  float points[MAX_POINTS];
  bool pointValid[MAX_POINTS];
  int numPoints;
  float prevClose;
  float lastPrice;
  float pctChange;
  bool fetched;    // has data been fetched at least once
  bool isCrypto;   // true for 24/7 assets like BTC-USD
  char startLabel[6]; // first x-axis time label (e.g. "9:30" or "20:00")
  char endLabel[6];   // last x-axis time label
};

TickerData tickers[NUM_TICKERS];
bool marketOpen = false;

// Layout constants
#define CHART_X      15
#define CHART_Y      38
#define CHART_W      210
#define CHART_H      168
#define CHART_BOTTOM (CHART_Y + CHART_H)

#define COLOR_GREEN  0x07E0
#define COLOR_RED    0xF800
#define COLOR_GRAY   0x4208
#define COLOR_DGRAY  0x2104

void drawUI() {
  TickerData &td = tickers[currentTicker];

  ui.tft.fillScreen(TFT_BLACK);

  // --- Header ---
  uint16_t pctColor = (td.pctChange >= 0) ? COLOR_GREEN : COLOR_RED;

  // Ticker name (left)
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  ui.tft.setCursor(8, 6);
  ui.tft.print(TICKERS[currentTicker]);

  // Last price (center) — compact format for large values
  char priceStr[16];
  if (td.lastPrice >= 10000)
    snprintf(priceStr, sizeof(priceStr), "$%.0f", td.lastPrice);
  else if (td.lastPrice >= 1000)
    snprintf(priceStr, sizeof(priceStr), "$%.1f", td.lastPrice);
  else
    snprintf(priceStr, sizeof(priceStr), "$%.2f", td.lastPrice);
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int pw = ui.tft.TTFtextWidth(priceStr);
  ui.tft.setCursor((240 - pw) / 2, 6);
  ui.tft.print(priceStr);

  // Percent change (right)
  char pctStr[16];
  snprintf(pctStr, sizeof(pctStr), "%+.2f%%", td.pctChange);
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(pctColor, TFT_BLACK);
  int pcw = ui.tft.TTFtextWidth(pctStr);
  ui.tft.setCursor(232 - pcw, 6);
  ui.tft.print(pctStr);

  // --- Separator line ---
  ui.tft.drawFastHLine(0, 30, 240, COLOR_DGRAY);

  // --- Ticker indicator dots ---
  int dotSpacing = 10;
  int dotsWidth = ((int)NUM_TICKERS - 1) * dotSpacing;
  int dotStartX = (240 - dotsWidth) / 2;
  for (int i = 0; i < (int)NUM_TICKERS; i++) {
    uint16_t dotColor = (i == currentTicker) ? TFT_WHITE : COLOR_DGRAY;
    ui.tft.fillCircle(dotStartX + i * dotSpacing, 35, 2, dotColor);
  }

  // --- Chart ---
  if (td.numPoints < 2) {
    ui.tft.setTTFFont(Arial_14_Bold);
    ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
    const char* msg = td.fetched ? "No data" : "Loading...";
    int mw = ui.tft.TTFtextWidth(msg);
    ui.tft.setCursor((240 - mw) / 2, 120);
    ui.tft.print(msg);
    return;
  }

  // Find min/max delta for Y scaling
  float minDelta = 0, maxDelta = 0;
  for (int i = 0; i < MAX_POINTS; i++) {
    if (!td.pointValid[i]) continue;
    if (td.points[i] < minDelta) minDelta = td.points[i];
    if (td.points[i] > maxDelta) maxDelta = td.points[i];
  }

  // Ensure zero line is always visible
  if (minDelta > 0) minDelta = 0;
  if (maxDelta < 0) maxDelta = 0;

  // Add 10% padding
  float range = maxDelta - minDelta;
  if (range < 0.01f) range = 1.0f;
  minDelta -= range * 0.1f;
  maxDelta += range * 0.1f;
  range = maxDelta - minDelta;

  // Draw zero (previous close) reference line - dotted
  int zeroY = CHART_BOTTOM - (int)((0.0f - minDelta) / range * CHART_H);
  if (zeroY >= CHART_Y && zeroY <= CHART_BOTTOM) {
    for (int x = CHART_X; x < CHART_X + CHART_W; x += 4) {
      ui.tft.drawPixel(x, zeroY, COLOR_GRAY);
    }
  }

  // Draw chart fill + line
  // Gradient fill colors per side — green above zero, red below
  static const uint16_t greenFill[] = {0x0300, 0x01C0, 0x00C0};
  static const uint16_t redFill[]   = {0x5800, 0x3000, 0x1800};

  int zeroYClamped = constrain(zeroY, CHART_Y, CHART_BOTTOM);

  // First pass: filled gradient area between line and zero line
  int prevX = -1, prevY = -1;
  for (int i = 0; i < MAX_POINTS; i++) {
    if (!td.pointValid[i]) continue;

    int x = CHART_X + i * CHART_W / 77;
    int y = CHART_BOTTOM - (int)((td.points[i] - minDelta) / range * CHART_H);
    y = constrain(y, CHART_Y, CHART_BOTTOM);

    if (prevX >= 0) {
      for (int fx = prevX; fx <= x; fx++) {
        float t = (x == prevX) ? 0.0f : (float)(fx - prevX) / (x - prevX);
        int fy = prevY + (int)(t * (y - prevY));

        int top = min(fy, zeroYClamped);
        int bot = max(fy, zeroYClamped);
        int h = bot - top;
        if (h < 1) continue;

        const uint16_t *fill = (fy <= zeroYClamped) ? greenFill : redFill;

        int b1 = max(h / 3, 1);
        int b2 = max(h / 3, 1);
        int b3 = h - b1 - b2;

        if (fy <= zeroYClamped) {
          ui.tft.drawFastVLine(fx, top, b1, fill[0]);
          if (b2 > 0) ui.tft.drawFastVLine(fx, top + b1, b2, fill[1]);
          if (b3 > 0) ui.tft.drawFastVLine(fx, top + b1 + b2, b3, fill[2]);
        } else {
          if (b3 > 0) ui.tft.drawFastVLine(fx, top, b3, fill[2]);
          if (b2 > 0) ui.tft.drawFastVLine(fx, top + b3, b2, fill[1]);
          ui.tft.drawFastVLine(fx, top + b3 + b2, b1, fill[0]);
        }
      }
    }
    prevX = x;
    prevY = y;
  }

  // Redraw the zero line on top of fill
  if (zeroY >= CHART_Y && zeroY <= CHART_BOTTOM) {
    for (int x = CHART_X; x < CHART_X + CHART_W; x += 4) {
      ui.tft.drawPixel(x, zeroY, COLOR_GRAY);
    }
  }

  // Second pass: draw the chart line on top
  prevX = -1;
  prevY = -1;
  for (int i = 0; i < MAX_POINTS; i++) {
    if (!td.pointValid[i]) continue;

    int x = CHART_X + i * CHART_W / 77;
    int y = CHART_BOTTOM - (int)((td.points[i] - minDelta) / range * CHART_H);
    y = constrain(y, CHART_Y, CHART_BOTTOM);

    if (prevX >= 0) {
      bool prevAbove = prevY <= zeroYClamped;
      bool currAbove = y <= zeroYClamped;

      if (prevAbove != currAbove && prevY != y) {
        float crossT = (float)(zeroYClamped - prevY) / (y - prevY);
        int crossX = prevX + (int)(crossT * (x - prevX));
        ui.tft.drawLine(prevX, prevY, crossX, zeroYClamped, prevAbove ? COLOR_GREEN : COLOR_RED);
        ui.tft.drawLine(crossX, zeroYClamped, x, y, currAbove ? COLOR_GREEN : COLOR_RED);
      } else {
        ui.tft.drawLine(prevX, prevY, x, y, currAbove ? COLOR_GREEN : COLOR_RED);
      }
    }
    prevX = x;
    prevY = y;
  }

  // --- X-axis time labels ---
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);

  // Start label (left)
  ui.tft.setCursor(CHART_X, CHART_BOTTOM + 8);
  ui.tft.print(td.startLabel);

  // End label (right)
  int rw = ui.tft.TTFtextWidth(td.endLabel);
  ui.tft.setCursor(CHART_X + CHART_W - rw, CHART_BOTTOM + 8);
  ui.tft.print(td.endLabel);
}

// Check if US stock market is currently open using NTP time
bool isMarketOpen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100))
    return false;

  if (timeinfo.tm_wday == 0 || timeinfo.tm_wday == 6)
    return false;

  int minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return minutes >= (9 * 60 + 30) && minutes <= (16 * 60);
}

// Fetch chart data for a single ticker into its TickerData slot
void fetchTickerData(int idx) {
  TickerData &td = tickers[idx];
  const char *ticker = TICKERS[idx];

  secureClient.setInsecure();

  // URL-encode ticker (handle ^ -> %5E, etc.)
  char encoded[24];
  int ei = 0;
  for (int i = 0; ticker[i] && ei < (int)sizeof(encoded) - 4; i++) {
    if (ticker[i] == '^') {
      encoded[ei++] = '%'; encoded[ei++] = '5'; encoded[ei++] = 'E';
    } else {
      encoded[ei++] = ticker[i];
    }
  }
  encoded[ei] = '\0';

  HTTPClient http;
  char url[192];
  if (td.isCrypto) {
    // Already known to be crypto — use 24h rolling window directly
    time_t epoch;
    time(&epoch);
    unsigned long now = (epoch > 1700000000) ? (unsigned long)epoch : (unsigned long)(millis() / 1000) + 1774396800UL;
    unsigned long period1 = now - 86400;
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s"
             "?interval=5m&period1=%lu&period2=%lu&includePrePost=false",
             encoded, period1, now);
  } else {
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s"
             "?interval=5m&range=1d&includePrePost=false",
             encoded);
  }

  http.begin(secureClient, url);
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.addHeader("Accept", "application/json");
  http.setTimeout(15000);

  Serial.printf("Fetching: %s (heap: %u free, %u largest)\n",
                ticker, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("  %s: HTTP %d\n", ticker, code);
    http.end();
    return;
  }

  // Read response body, then parse with filter
  String body = http.getString();
  http.end();
  Serial.printf("  %s: body %d bytes\n", ticker, body.length());

  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  filter["chart"]["result"][0]["meta"]["instrumentType"] = true;
  filter["chart"]["result"][0]["timestamp"] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;
  filter["chart"]["error"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body,
                                             DeserializationOption::Filter(filter));
  body = String(); // free memory immediately

  if (err) {
    Serial.printf("  %s: JSON error: %s\n", ticker, err.c_str());
    return;
  }

  JsonObject result = doc["chart"]["result"][0];
  if (result.isNull()) {
    // Yahoo may return an error (e.g. rate limiting)
    const char *errMsg = doc["chart"]["error"]["description"];
    Serial.printf("  %s: no result (%s)\n", ticker, errMsg ? errMsg : "unknown");
    return;
  }

  JsonArray timestamps = result["timestamp"];
  JsonArray closes = result["indicators"]["quote"][0]["close"];

  if (timestamps.size() == 0 || closes.size() == 0) {
    Serial.printf("  %s: no data points\n", ticker);
    return;
  }

  // Previous close is the reference for percentage and chart zero line
  float openPrice = closes[0].as<float>();
  td.prevClose = result["meta"]["chartPreviousClose"] | openPrice;
  td.lastPrice = result["meta"]["regularMarketPrice"] | openPrice;
  td.pctChange = (td.prevClose > 0) ? (td.lastPrice - td.prevClose) / td.prevClose * 100.0f : 0.0f;

  // Detect crypto by instrumentType — re-fetch with 24h rolling window if needed
  const char* instrType = result["meta"]["instrumentType"] | "";
  td.isCrypto = (strcmp(instrType, "CRYPTOCURRENCY") == 0);

  if (td.isCrypto) {
    time_t epoch;
    time(&epoch);
    if (epoch > 1700000000) {
      unsigned long now = (unsigned long)epoch;
      unsigned long period1 = now - 86400;
      snprintf(url, sizeof(url),
               "https://query1.finance.yahoo.com/v8/finance/chart/%s"
               "?interval=5m&period1=%lu&period2=%lu&includePrePost=false",
               encoded, period1, now);
      http.begin(secureClient, url);
      http.addHeader("User-Agent", "Mozilla/5.0");
      http.addHeader("Accept", "application/json");
      http.setTimeout(15000);
      Serial.printf("  %s: re-fetching 24h window for crypto\n", ticker);
      code = http.GET();
      if (code == HTTP_CODE_OK) {
        body = http.getString();
        http.end();
        doc.clear();
        err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
        body = String();
        if (!err) {
          result = doc["chart"]["result"][0];
          if (!result.isNull()) {
            timestamps = result["timestamp"];
            closes = result["indicators"]["quote"][0]["close"];
            td.prevClose = result["meta"]["chartPreviousClose"] | td.prevClose;
            td.lastPrice = result["meta"]["regularMarketPrice"] | td.lastPrice;
            td.pctChange = (td.prevClose > 0) ? (td.lastPrice - td.prevClose) / td.prevClose * 100.0f : 0.0f;
          }
        }
      } else {
        http.end();
      }
    }
  }

  // Build points array: map Yahoo data proportionally into MAX_POINTS slots
  td.numPoints = 0;
  memset(td.pointValid, 0, sizeof(td.pointValid));

  size_t total = min(timestamps.size(), closes.size());

  // Generate x-axis time labels from first/last timestamps
  long firstTs = timestamps[0].as<long>();
  long lastTs = timestamps[total - 1].as<long>();

  // Convert to ET (UTC-4 during EDT, UTC-5 during EST)
  // configTime already set the TZ, so use localtime
  struct tm tmBuf;
  time_t ft = (time_t)firstTs;
  localtime_r(&ft, &tmBuf);
  snprintf(td.startLabel, sizeof(td.startLabel), "%d:%02d", tmBuf.tm_hour, tmBuf.tm_min);

  time_t lt = (time_t)lastTs;
  localtime_r(&lt, &tmBuf);
  snprintf(td.endLabel, sizeof(td.endLabel), "%d:%02d", tmBuf.tm_hour, tmBuf.tm_min);

  if (!td.isCrypto) {
    // Stocks: map by 5-min intervals from market open
    for (size_t i = 0; i < total; i++) {
      if (closes[i].isNull())
        continue;
      long ts = timestamps[i].as<long>();
      int ptIdx = (int)((ts - firstTs) / 300);
      if (ptIdx >= 0 && ptIdx < MAX_POINTS) {
        td.points[ptIdx] = closes[i].as<float>() - td.prevClose;
        td.pointValid[ptIdx] = true;
        if (ptIdx + 1 > td.numPoints)
          td.numPoints = ptIdx + 1;
      }
    }
  } else {
    // Crypto: downsample proportionally into MAX_POINTS slots
    for (size_t i = 0; i < total; i++) {
      if (closes[i].isNull())
        continue;
      int ptIdx = (total <= 1) ? 0 : (int)(i * (MAX_POINTS - 1) / (total - 1));
      if (ptIdx >= 0 && ptIdx < MAX_POINTS) {
        td.points[ptIdx] = closes[i].as<float>() - td.prevClose;
        td.pointValid[ptIdx] = true;
        if (ptIdx + 1 > td.numPoints)
          td.numPoints = ptIdx + 1;
      }
    }
  }

  td.fetched = true;

  Serial.printf("  %s: prev=%.2f last=%.2f %+.2f%% pts=%d\n",
                ticker, td.prevClose, td.lastPrice, td.pctChange, td.numPoints);
}

// Per-ticker fetch timestamps for staleness check
unsigned long tickerFetchTime[NUM_TICKERS] = {0};

void fetchCurrentTicker() {
  marketOpen = ntpSynced && isMarketOpen();
  fetchTickerData(currentTicker);
  tickerFetchTime[currentTicker] = millis();
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting stock app");

  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Sync NTP time — US Eastern (UTC-5, DST UTC-4)
  configTime(-5 * 3600, 3600, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    ntpSynced = true;
    Serial.printf("NTP synced: %04d-%02d-%02d %02d:%02d ET\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                  timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    Serial.println("NTP sync failed");
  }

  // Initialize all ticker data
  memset(tickers, 0, sizeof(tickers));
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT

    // Fetch current ticker if stale or never fetched
    unsigned long now = millis();
    unsigned long age = now - tickerFetchTime[currentTicker];
    if (tickerFetchTime[currentTicker] == 0 || age > (unsigned long)refreshTimeInSeconds * 1000) {
      fetchCurrentTicker();
      drawUI();
    }

    // Button press — cycle to next ticker
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);
    if (pressed && !buttonWasPressed) {
      delay(50);
      if (digitalRead(BUTTON_PIN) == LOW) {
        currentTicker = (currentTicker + 1) % NUM_TICKERS;
        Serial.printf("Switched to %s\n", TICKERS[currentTicker]);
        // Show cached data (or "Loading...") immediately, then fetch if stale
        drawUI();
        unsigned long tickerAge = now - tickerFetchTime[currentTicker];
        if (tickerFetchTime[currentTicker] == 0 || tickerAge > (unsigned long)refreshTimeInSeconds * 1000) {
          fetchCurrentTicker();
          drawUI();
        }
      }
    }
    buttonWasPressed = pressed;
  }

  delay(1);
}
