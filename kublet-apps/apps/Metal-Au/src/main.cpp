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
// To clone this app for the other metal, change these three constants
// (e.g., "Au" → "Ag", "xauusd" → "xagusd", COIN_COLOR → silver).
static const char *COIN_TICKER = "Au-USD";
static const char *COIN_SYMBOL = "xauusd";  // Stooq symbol (lowercase)
#define COIN_COLOR 0xFEA0  // warm gold
// =============================================================================

OTAServer otaserver;
KGFX ui;
WiFiClientSecure secureClient;

static const uint32_t REFRESH_FALLBACK_MS = 65UL * 1000;
static const int N_SAMPLES = 60;  // 60-min ring buffer

unsigned long lastFetchMs = 0;
unsigned long lastClockRedrawMs = 0;
// Ring buffer of last 60 minute samples. `head` is the index of the slot the
// next sample will overwrite. `filled` saturates at N_SAMPLES.
float samples[N_SAMPLES];
int head = 0;
int filled = 0;
time_t lastFetchEpoch = 0;
bool lastFetchOk = false;
bool ntpSynced = false;
uint8_t currentBrightness = 0;
int lastFetchMinute = -1;
int priceDir = 0;  // last-minute direction (+1 up / -1 down / 0 flat)

#define COLOR_DGRAY 0x2104
#define COLOR_GRAY  0x4208
#define COLOR_GREEN 0x07E0
#define COLOR_RED   0xF800
#define COLOR_DGREEN 0x0320
#define COLOR_DRED   0x6000
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

void recordSample(float price) {
  samples[head] = price;
  head = (head + 1) % N_SAMPLES;
  if (filled < N_SAMPLES) filled++;
}

// Read the i-th sample in chronological order (0 = oldest still in the buffer,
// filled-1 = newest). Used so the chart renders left→right in time order
// independent of where `head` currently points.
float sampleAt(int i) {
  if (i < 0 || i >= filled) return NAN;
  int start = (head - filled + N_SAMPLES) % N_SAMPLES;
  return samples[(start + i) % N_SAMPLES];
}

// Fetch close price from Stooq's CSV endpoint. Two-line ~100-byte response:
//   Symbol,Date,Time,Open,High,Low,Close,Volume
//   XAUUSD,2026-05-15,…,…,…,…,2390.55,
// Returns NAN on any failure (HTTP, malformed body, "N/D" for closed market).
float fetchStooq() {
  secureClient.setInsecure();
  HTTPClient http;
  String url = String("https://stooq.com/q/l/?s=") + COIN_SYMBOL + "&f=sd2t2ohlcv&h&e=csv";
  http.begin(secureClient, url);
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.setTimeout(10000);

  Serial.printf("Fetching %s (heap free %u)\n", COIN_SYMBOL, ESP.getFreeHeap());
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
  if (nl < 0) return NAN;
  int pos = nl + 1;
  for (int i = 0; i < 6; i++) {
    int c = body.indexOf(',', pos);
    if (c < 0) return NAN;
    pos = c + 1;
  }
  int endComma = body.indexOf(',', pos);
  String priceStr = (endComma > 0) ? body.substring(pos, endComma) : body.substring(pos);
  priceStr.trim();
  if (priceStr.length() == 0 || priceStr.equalsIgnoreCase("N/D")) {
    Serial.println("  no data (market closed?)");
    return NAN;
  }
  float price = priceStr.toFloat();
  Serial.printf("  %s = $%.4f / oz\n", COIN_SYMBOL, price);
  return price;
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
    if (dx < 8) dx = 8;
    ui.tft.setCursor(dx, 6);
    ui.tft.print(dbuf);
  }

  drawDateTime(time(nullptr));
  ui.tft.drawFastHLine(0, 42, 240, COLOR_DGRAY);
}

void drawPrice(float price, int direction) {
  // Price sits just above the IP-line footer (y=210). With Arial_48_Bold
  // glyphs at ~48 px tall and cursor.y=158, the price spans y=158..206 —
  // leaving the upper portion of the screen free for the 1h chart.
  ui.tft.fillRect(0, 152, 240, 58, TFT_BLACK);

  char buf[20];
  if (isnan(price)) strcpy(buf, "--.--");
  else              formatPriceEU(buf, sizeof(buf), price, 0);

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

// Same delta-from-baseline chart shape as the crypto solo apps, but the
// baseline is the oldest sample we currently have (may be only a few
// minutes old when the cube just booted).
void drawChart() {
  // Big "Au" badge sits on the left edge of the chart band, vertically
  // aligned with the baseline. Chart_x is shifted to the right of it so
  // the data bars never collide with the label.
  const int chart_x = 8;
  const int chart_y = 48;
  const int chart_w = 224;
  const int chart_h = 104;
  const int baseline_y = chart_y + chart_h / 2;

  ui.tft.fillRect(0, chart_y - 2, 240, chart_h + 4, TFT_BLACK);
  if (filled < 2) {
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
    char msg[40];
    snprintf(msg, sizeof(msg), "chart filling (%d/60)...", filled);
    int w = ui.tft.TTFtextWidth(msg);
    ui.tft.setCursor((240 - w) / 2, baseline_y - 6);
    ui.tft.print(msg);
    return;
  }

  float baseline = sampleAt(0);
  float maxAbs = 0.0f;
  for (int i = 1; i < filled; i++) {
    float d = sampleAt(i) - baseline;
    if (fabsf(d) > maxAbs) maxAbs = fabsf(d);
  }
  if (maxAbs <= 0.0f) maxAbs = 1.0f;

  ui.tft.drawFastHLine(chart_x, baseline_y, chart_w, COLOR_DGRAY);

  int bar_w = chart_w / N_SAMPLES;
  if (bar_w < 1) bar_w = 1;

  for (int i = 0; i < filled; i++) {
    float delta = sampleAt(i) - baseline;
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
  ui.tft.print("Au");
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
  drawPrice(filled > 0 ? sampleAt(filled - 1) : NAN, priceDir);
  drawChart();
  drawFooter(fetchOk, lastEpoch);
}

void setup() {
  Serial.begin(460800);
  Serial.print("\n=== ");
  Serial.print(COIN_TICKER);
  Serial.println(" solo (Stooq) app starting ===");

  pinMode(BL_PIN, OUTPUT);
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(BL_PIN, BL_CHANNEL);
  ledcWrite(BL_CHANNEL, 255);
  currentBrightness = 255;

  otaserver.connectWiFi();
  otaserver.run();

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  for (int i = 0; i < N_SAMPLES; i++) samples[i] = NAN;

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
    otaserver.handle();

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
      float p = fetchStooq();
      bool ok = !isnan(p);
      lastFetchOk = ok;
      if (ok) {
        if (filled > 0) {
          float prev = sampleAt(filled - 1);
          if      (p > prev + 0.00005f * prev) priceDir = +1;
          else if (p < prev - 0.00005f * prev) priceDir = -1;
          else                                  priceDir = 0;
        }
        recordSample(p);
        time(&lastFetchEpoch);
      }
      lastFetchMs = now;

      if (!ntpSynced) {
        struct tm tmBuf;
        if (getLocalTime(&tmBuf, 100)) ntpSynced = true;
      }
      applyBrightness();

      float deltaPct = NAN;
      if (filled >= 2) {
        float first = sampleAt(0);
        if (first > 0.0f) {
          deltaPct = (sampleAt(filled - 1) - first) / first * 100.0f;
        }
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
