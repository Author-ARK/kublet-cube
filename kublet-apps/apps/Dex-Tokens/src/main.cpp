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

#define COLOR_DGRAY 0x2104
#define COLOR_GRAY  0x4208
#define COLOR_GREEN 0x07E0
#define COLOR_RED   0xF800
// 1-minute direction tints for the price text. Bright (near-white)
// luminance with a subtle green/red bias — see the tick at a glance
// without losing the per-coin label color.
#define COLOR_TINT_UP   0xC7FA  // bright white with a green hint
#define COLOR_TINT_DOWN 0xFED8  // bright white with a red hint

// Tokens to track. Edit this array and redeploy to change which tokens the
// cube watches. Each entry is { display, chain, address, color }:
//   display  — 3-5 char label shown on screen (truncated if longer)
//   chain    — Dexscreener chain id (ethereum, solana, base, arbitrum, …)
//   address  — token contract address (lowercase, with 0x prefix on EVM chains)
//   color    — RGB565 color used for the label + price text
//
// We use the chain-scoped /tokens/v1/{chain}/{address} endpoint per the
// Dexscreener API docs — it returns ~1.2 KB JSON (one pair object) which
// the ESP32 can swallow safely. The full-token endpoint returns 30+ pairs
// (~95 KB) and would blow the heap.
struct Token {
  const char *display;
  const char *chain;
  const char *address;
  uint16_t    color;
};

// Four configured tokens — the cube fetches all four every minute and the
// button (GPIO 19) flips between pages: page 0 shows tokens 0+1, page 1
// shows tokens 2+3. Tap the button to cycle.
//
// NOTE: PHNIX and Lambo were both supplied with the same XRPL address
// (`PHNIX.rDFXbW2…`). That's almost certainly a copy-paste — Lambo should
// have its own currency-code + issuer pair. Update the second entry once
// you have the real Lambo identifier or both rows will show the same price.
static const Token TOKENS[] = {
  // Page 0
  {"SOLX",  "solana",   "AMjzRn1TBQwQfNAjHFeBb7uGbbqbJB7FzXAnGgdFPk6K",                                  0xFD20},  // Solcex (truncated to 4 chars), orange
  {"PEPEb", "base",     "0x52b492a33E447Cdb854c7FC19F1e57E8BfA1777D",                                    0x07E6},  // Pepe on Base, pepe green
  // Page 1
  {"PHNIX", "xrpl",     "50484E4958000000000000000000000000000000.rDFXbW2ZZCG5WgPtqwNiA2xZokLMm9ivmN",  0xFFE0},  // PHNIX on XRP Ledger, yellow
  {"LAMBO", "xrpl",     "50484E4958000000000000000000000000000000.rDFXbW2ZZCG5WgPtqwNiA2xZokLMm9ivmN",  0xC81F},  // Lambo — placeholder address, magenta
};
static const int N_TOKENS = sizeof(TOKENS) / sizeof(TOKENS[0]);
static const int N_PAGES  = (N_TOKENS + 1) / 2;
int currentPage = 0;

unsigned long lastFetchMs = 0;
unsigned long lastClockRedrawMs = 0;
float lastPrice[N_TOKENS];
int   direction[N_TOKENS];  // +1 up, -1 down, 0 flat
time_t lastFetchEpoch = 0;
bool lastFetchOk = false;
bool ntpSynced = false;
uint8_t currentBrightness = 0;
int lastFetchMinute = -1;

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

// Pull `priceUsd` for one token. Response shape:
//   [ { "chainId": "...", ..., "priceUsd": "2227.83", ... } ]
// We don't need a JSON parser — substring search for the first
// `"priceUsd":"<value>"` is sufficient and keeps heap pressure minimal.
float fetchDexPrice(const Token &t) {
  secureClient.setInsecure();
  HTTPClient http;
  String url = String("https://api.dexscreener.com/tokens/v1/") + t.chain + "/" + t.address;
  http.begin(secureClient, url);
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.setTimeout(10000);

  Serial.printf("Fetching %s on %s (heap free %u)\n", t.display, t.chain, ESP.getFreeHeap());
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("  HTTP %d\n", code);
    http.end();
    return NAN;
  }

  String body = http.getString();
  http.end();
  Serial.printf("  body %d bytes\n", body.length());

  int p = body.indexOf("\"priceUsd\":\"");
  if (p < 0) {
    Serial.println("  no priceUsd field (no liquid pair?)");
    return NAN;
  }
  p += 12;  // strlen("\"priceUsd\":\"")
  int e = body.indexOf('"', p);
  if (e < 0) return NAN;
  String priceStr = body.substring(p, e);
  float price = priceStr.toFloat();
  Serial.printf("  %s = $%.8f\n", t.display, price);
  return price;
}

// Format USD price for the tiny screen. Token prices span eight orders of
// magnitude — memecoins live at $0.0000001, blue chips at thousands — so
// we pick a format that always shows at least 3 significant digits.
void formatPrice(char *out, size_t cap, float price) {
  if (isnan(price))           { snprintf(out, cap, "--");        return; }
  if (price >= 1000.0f)       { snprintf(out, cap, "%.0f", price); return; }
  if (price >= 1.0f)          { snprintf(out, cap, "%.2f", price); return; }
  if (price >= 0.01f)         { snprintf(out, cap, "%.4f", price); return; }
  if (price >= 0.000001f)     { snprintf(out, cap, "%.6f", price); return; }
  // Truly tiny memecoin range — exponential form so the value isn't just "$0".
  snprintf(out, cap, "%.2e", price);
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
  const char *title = "DEX SCREENER";
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

static void drawDirectionGlyph(int x, int y, int dir) {
  if (dir > 0)      ui.tft.fillTriangle(x, y + 12, x + 14, y + 12, x + 7, y, COLOR_GREEN);
  else if (dir < 0) ui.tft.fillTriangle(x, y, x + 14, y, x + 7, y + 12, COLOR_RED);
  else              ui.tft.fillRect(x + 1, y + 5, 12, 2, COLOR_GRAY);
}

void drawTokenRow(int y0, const Token &tok, float price, int dir) {
  ui.tft.fillRect(0, y0, 240, 82, TFT_BLACK);

  ui.tft.setTTFFont(Arial_20_Bold);
  ui.tft.setTextColor(tok.color, TFT_BLACK);
  ui.tft.setCursor(10, y0 + 20);
  // Truncate excessively long tickers so they don't run into the price field.
  char lbl[8];
  snprintf(lbl, sizeof(lbl), "%.5s", tok.display);
  ui.tft.print(lbl);

  int labelW = ui.tft.TTFtextWidth(lbl);
  const int leftMargin = 10, gapAfterLabel = 20, rightMargin = 8, arrowAreaW = 18;
  int reservedLeft = leftMargin + labelW + gapAfterLabel;

  char buf[24];
  formatPrice(buf, sizeof(buf), price);

  ui.tft.setTTFFont(Arial_24_Bold);
  // Last-minute direction tints the PRICE only — the label
  // stays in tok.color so the coin/metal is still identifiable.
  uint16_t priceColor = TFT_WHITE;
  if      (dir > 0) priceColor = COLOR_TINT_UP;
  else if (dir < 0) priceColor = COLOR_TINT_DOWN;
  ui.tft.setTextColor(priceColor, TFT_BLACK);
  int pw = ui.tft.TTFtextWidth(buf);
  int rightArea = 240 - reservedLeft - rightMargin - arrowAreaW;
  int priceX = reservedLeft + (rightArea - pw) / 2;
  if (priceX < reservedLeft) priceX = reservedLeft;
  ui.tft.setCursor(priceX, y0 + 16);
  ui.tft.print(buf);

  int arrowX = 240 - rightMargin - 14;
  int arrowY = y0 + 20;
  drawDirectionGlyph(arrowX, arrowY, dir);

  ui.tft.setTTFFont(Arial_11);
  ui.tft.setTextColor(COLOR_GRAY, TFT_BLACK);
  char unit[32];
  snprintf(unit, sizeof(unit), "USD · %s", tok.chain);
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

void drawUI(time_t lastFetch, bool fetchOk) {
  ui.tft.fillScreen(TFT_BLACK);
  drawHeader();
  int i0 = currentPage * 2;
  int i1 = i0 + 1;
  if (i0 < N_TOKENS) drawTokenRow(48,  TOKENS[i0], lastPrice[i0], direction[i0]);
  ui.tft.drawFastHLine(0, 130, 240, COLOR_DGRAY);
  if (i1 < N_TOKENS) drawTokenRow(132, TOKENS[i1], lastPrice[i1], direction[i1]);
  drawFooter(fetchOk, lastFetch);
}

void setup() {
  Serial.begin(460800);
  Serial.println("\n=== Dex-Tokens app starting ===");

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

  for (int i = 0; i < N_TOKENS; i++) {
    lastPrice[i] = NAN;
    direction[i] = 0;
  }

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

  drawUI(0, false);
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

    // Button cycles the displayed page (0 = first pair, 1 = second pair).
    // Active-LOW pulled-up input; debounce by edge-detect.
    static bool buttonPrev = HIGH;
    bool buttonNow = digitalRead(BUTTON_PIN);
    if (buttonPrev == HIGH && buttonNow == LOW) {
      currentPage = (currentPage + 1) % N_PAGES;
      drawUI(lastFetchEpoch, lastFetchOk);
      delay(40);  // tiny debounce — the loop already runs ~100 Hz
    }
    buttonPrev = buttonNow;

    if (shouldFetch) {
      bool allOk = true;
      for (int i = 0; i < N_TOKENS; i++) {
        float p = fetchDexPrice(TOKENS[i]);
        if (isnan(p)) {
          allOk = false;
        } else {
          if (!isnan(lastPrice[i])) {
            if      (p > lastPrice[i] * 1.00005f) direction[i] = +1;
            else if (p < lastPrice[i] * 0.99995f) direction[i] = -1;
            else                                  direction[i] = 0;
          }
          lastPrice[i] = p;
        }
      }
      lastFetchOk = allOk;
      if (allOk) time(&lastFetchEpoch);
      lastFetchMs = now;

      if (!ntpSynced) {
        struct tm tmBuf;
        if (getLocalTime(&tmBuf, 100)) ntpSynced = true;
      }
      applyBrightness();
      drawUI(lastFetchEpoch, lastFetchOk);
    }

    if (now - lastClockRedrawMs >= 1000) {
      lastClockRedrawMs = now;
      drawDateTime(time(nullptr));
      applyBrightness();
    }
  }

  delay(10);
}
