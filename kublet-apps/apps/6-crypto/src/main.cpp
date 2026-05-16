#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
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

#define COLOR_DGRAY  0x2104
#define COLOR_GRAY   0x4208
#define COLOR_GREEN  0x07E0
#define COLOR_RED    0xF800
// 1-minute direction tints for the price column. White-tinted with a hint
// of green/red so the freshest tick stands out against the per-coin label
// color on the left of the same row.
#define COLOR_TINT_UP   0xC7FA
#define COLOR_TINT_DOWN 0xFED8

// Six coins, in display order. Each entry pairs a Binance ticker with the
// 4-character label drawn on screen and a per-coin "brand" color used for
// the label glyph. Add/remove rows here to change the board — keep N_COINS
// in sync if you do.
struct Coin {
  const char *symbol;   // Binance market, e.g., "BTCUSDT"
  const char *label;    // What renders on screen, e.g., "BTC"
  uint16_t    color;    // RGB565 label color
};

static const Coin COINS[] = {
  {"BTCUSDT",  "BTC",  0xFD20},   // bitcoin orange
  {"ETHUSDT",  "ETH",  0x649F},   // ethereum cool blue/violet
  {"XRPUSDT",  "XRP",  0xC7FC},   // ripple pale blue/white
  {"SOLUSDT",  "SOL",  0xC81F},   // solana magenta
  {"XLMUSDT",  "XLM",  0x07FF},   // stellar cyan
  {"HBARUSDT", "HBAR", 0xA81F},   // hedera violet
};
static const int N_COINS = sizeof(COINS) / sizeof(COINS[0]);

// Per-coin live state — populated from Binance's batched windowSize=1h
// ticker. priceDir is computed locally each fetch by comparing the new
// lastPrice against the previously-stored one.
float lastPrice[N_COINS];
float chgPct1h[N_COINS];
int   priceDir[N_COINS];

unsigned long lastFetchMs = 0;
time_t lastFetchEpoch = 0;
bool lastFetchOk = false;
bool ntpSynced = false;
uint8_t currentBrightness = 0;
int lastFetchMinute = -1;

// ---------------------------------------------------------------------------
// Brightness — same Asuncion-local schedule as the other apps so a fleet of
// cubes dims in unison overnight.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// European number formatter — "94.218" / "1,42" / "0,09098". Borrowed from
// the solo-app variant. ESP32 newlib lacks reliable locale support so we
// hand-roll the digit grouping.
// ---------------------------------------------------------------------------
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

// Picks the number of decimals to show based on magnitude. Same tiers as
// the Dex-Tokens app — keeps tiny memecoin prices readable while big-cap
// coins like BTC render as round-dollar values.
static int decimalsForPrice(float price) {
  if (price >= 1000.0f)   return 0;
  if (price >= 1.0f)      return 2;
  if (price >= 0.01f)     return 4;
  if (price >= 0.0001f)   return 6;
  return 8;
}

// ---------------------------------------------------------------------------
// Fetch — single Binance call that returns lastPrice + 1h priceChangePercent
// for all six symbols in one go. Body is ~2.4 KB.
// ---------------------------------------------------------------------------
bool fetchAllCoins() {
  secureClient.setInsecure();
  HTTPClient http;
  // Symbols= URL parameter is a JSON array of strings — URL-encoded brackets
  // and quotes. Keep it hand-built so we don't have to depend on URL helpers.
  const char *url =
    "https://api.binance.com/api/v3/ticker"
    "?symbols=%5B%22BTCUSDT%22,%22ETHUSDT%22,%22XRPUSDT%22,%22SOLUSDT%22,"
    "%22XLMUSDT%22,%22HBARUSDT%22%5D&windowSize=1h";
  http.begin(secureClient, url);
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.setTimeout(10000);

  Serial.printf("Fetching 6-crypto ticker (heap free %u)\n", ESP.getFreeHeap());
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("  HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  Serial.printf("  body %d bytes\n", body.length());

  // ArduinoJson filter: index 0 acts as a template applied to every array
  // element — we keep symbol, lastPrice, priceChangePercent and discard
  // the other ~20 fields each entry carries.
  JsonDocument filter;
  JsonObject f0 = filter[0].to<JsonObject>();
  f0["symbol"] = true;
  f0["lastPrice"] = true;
  f0["priceChangePercent"] = true;

  JsonDocument doc;
  DeserializationError err =
    deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("  JSON err: %s\n", err.c_str());
    return false;
  }

  // Binance can return entries in a different order than we requested
  // (especially with batched calls), so match by symbol.
  int matched = 0;
  for (JsonObject entry : doc.as<JsonArray>()) {
    const char *sym = entry["symbol"].as<const char *>();
    if (!sym) continue;
    for (int i = 0; i < N_COINS; i++) {
      if (strcmp(sym, COINS[i].symbol) == 0) {
        const char *p = entry["lastPrice"].as<const char *>();
        const char *c = entry["priceChangePercent"].as<const char *>();
        if (!p) break;
        float newPrice = atof(p);
        float pct = c ? atof(c) : NAN;
        // Direction tint from previous fetched price. Epsilon scales with
        // price so flat noise doesn't flicker the arrow for low-priced
        // coins.
        if (!isnan(lastPrice[i])) {
          float eps = lastPrice[i] * 0.00005f;
          if      (newPrice > lastPrice[i] + eps) priceDir[i] = +1;
          else if (newPrice < lastPrice[i] - eps) priceDir[i] = -1;
          else                                     priceDir[i] = 0;
        }
        lastPrice[i] = newPrice;
        chgPct1h[i]  = pct;
        matched++;
        break;
      }
    }
  }
  Serial.printf("  parsed %d/%d coins\n", matched, N_COINS);
  return matched == N_COINS;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void drawHeader() {
  ui.tft.fillRect(0, 0, 240, 30, TFT_BLACK);

  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  const char *title = "6 CRYPTO";
  int w = ui.tft.TTFtextWidth(title);
  ui.tft.setCursor((240 - w) / 2, 6);
  ui.tft.print(title);

  char numStr[8];
  snprintf(numStr, sizeof(numStr), "#%d", KUBLET_NUM);
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  int nw = ui.tft.TTFtextWidth(numStr);
  ui.tft.setCursor(238 - nw, 6);
  ui.tft.print(numStr);

  ui.tft.drawFastHLine(0, 28, 240, COLOR_DGRAY);
}

// Small tick glyph that drives the per-row direction arrow on the far left.
// Sized for a 30-px row band (y0 + 7..23 visually).
static void drawTickArrow(int x, int y_top, int dir) {
  if (dir > 0) {
    ui.tft.fillTriangle(x, y_top + 10, x + 10, y_top + 10, x + 5, y_top + 2, COLOR_GREEN);
  } else if (dir < 0) {
    ui.tft.fillTriangle(x, y_top + 2, x + 10, y_top + 2, x + 5, y_top + 10, COLOR_RED);
  } else {
    ui.tft.fillRect(x + 1, y_top + 5, 8, 2, COLOR_GRAY);
  }
}

// One coin row at y0..y0+30. Layout left-to-right:
//   [▲/▼] [ticker] [price]                [+0.23%]
// Tick arrow + ticker hug the left; price right-aligns at x=156; 1h % change
// right-aligns at x=232 so the column edges stay flush regardless of
// individual string widths.
void drawCoinRow(int y0, const Coin &coin, float price, float pct1h, int dir,
                 bool drawDivider) {
  ui.tft.fillRect(0, y0, 240, 30, TFT_BLACK);

  // Tick glyph
  drawTickArrow(8, y0 + 8, dir);

  // Ticker label in coin color
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(coin.color, TFT_BLACK);
  ui.tft.setCursor(24, y0 + 8);
  ui.tft.print(coin.label);

  // Price (right-aligned at x=156), in white by default, tinted on tick
  char priceStr[20];
  if (isnan(price)) {
    snprintf(priceStr, sizeof(priceStr), "--.--");
  } else {
    formatPriceEU(priceStr, sizeof(priceStr), price, decimalsForPrice(price));
  }
  uint16_t priceColor = TFT_WHITE;
  if      (dir > 0) priceColor = COLOR_TINT_UP;
  else if (dir < 0) priceColor = COLOR_TINT_DOWN;
  ui.tft.setTextColor(priceColor, TFT_BLACK);
  int pw = ui.tft.TTFtextWidth(priceStr);
  ui.tft.setCursor(156 - pw, y0 + 8);
  ui.tft.print(priceStr);

  // 1h % change (right-aligned at x=232), green/red by sign
  if (!isnan(pct1h)) {
    char chgStr[12];
    snprintf(chgStr, sizeof(chgStr), "%+0.2f%%", pct1h);
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(pct1h >= 0 ? COLOR_GREEN : COLOR_RED, TFT_BLACK);
    int cw = ui.tft.TTFtextWidth(chgStr);
    ui.tft.setCursor(232 - cw, y0 + 10);
    ui.tft.print(chgStr);
  }

  if (drawDivider) {
    ui.tft.drawFastHLine(8, y0 + 29, 224, COLOR_DGRAY);
  }
}

void drawFooter() {
  ui.tft.fillRect(0, 212, 240, 28, TFT_BLACK);
  ui.tft.drawFastHLine(0, 214, 240, COLOR_DGRAY);

  ui.tft.setTTFFont(Arial_10);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  String ipStr = (WiFi.status() == WL_CONNECTED)
    ? WiFi.localIP().toString()
    : String("(no wifi)");
  ui.tft.setCursor(8, 224);
  ui.tft.print(ipStr);

  uint16_t dotColor = lastFetchOk ? COLOR_GREEN : COLOR_RED;
  ui.tft.fillCircle(228, 226, 4, dotColor);
}

void drawUI() {
  ui.tft.fillScreen(TFT_BLACK);
  drawHeader();
  // 6 rows of 30 px starting at y=32 (just below the header divider). Last
  // row's divider is suppressed; the footer line at y=214 closes the table.
  for (int i = 0; i < N_COINS; i++) {
    int y0 = 32 + i * 30;
    drawCoinRow(y0, COINS[i], lastPrice[i], chgPct1h[i], priceDir[i],
                /*drawDivider=*/ i < N_COINS - 1);
  }
  drawFooter();
}

void setup() {
  Serial.begin(460800);
  Serial.println("\n=== 6-crypto app starting ===");

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

  for (int i = 0; i < N_COINS; i++) {
    lastPrice[i] = NAN;
    chgPct1h[i]  = NAN;
    priceDir[i]  = 0;
  }

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

  drawUI();
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
      bool ok = fetchAllCoins();
      lastFetchOk = ok;
      if (ok) time(&lastFetchEpoch);
      lastFetchMs = now;

      if (!ntpSynced) {
        struct tm tmBuf;
        if (getLocalTime(&tmBuf, 100)) ntpSynced = true;
      }
      applyBrightness();
      drawUI();
    }
  }

  delay(10);
}
