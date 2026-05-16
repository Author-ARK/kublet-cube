#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;
WiFiClient netClient;
HTTPClient http;

bool buttonWasPressed = false;

float pingMs = 0;
float downloadMbps = 0;
float uploadMbps = 0;

// Hacker green palette
#define CLR_GREEN 0x07E0 // Bright green — primary accent
#define CLR_MID   0x04A0 // Medium green — labels
#define CLR_DIM   0x0280 // Dark green — track, subtle
#define CLR_VDIM  0x0120 // Very dark green — hints
#define CLR_VAL   TFT_WHITE
#define CLR_BG    TFT_BLACK

// Gauge geometry — arc fills left to right, gap at bottom
#define GX      120
#define GY      118
#define GOR     72
#define GIR     56
#define G_START 45
#define G_SPAN  270
#define G_END   ((G_START + G_SPAN) % 360)

// Fixed gauge scale — ESP32 WiFi+HTTP tops out around 2-3 Mbps
#define GAUGE_MAX 3.0f

// Reusable upload buffer — allocated during test, freed after
#define UL_BUF_SIZE 32768
static uint8_t *ulBuf = nullptr;

// Shared URL buffer to avoid heap String allocations
static char urlBuf[80];

// Forward declarations
void drawResultScreen();

// --- Drawing helpers ---

void drawCentered(const char *text, const tftfont_t &font, uint16_t color, int y) {
  ui.tft.setTTFFont(font);
  ui.tft.setTextColor(color, CLR_BG);
  int w = ui.tft.TTFtextWidth(text);
  ui.tft.setCursor((240 - w) / 2, y);
  ui.tft.print(text);
}

void drawGauge(float value, uint16_t color) {
  float maxVal = GAUGE_MAX;
  // Dark green track
  ui.tft.drawSmoothArc(GX, GY, GOR, GIR, G_START, G_END, CLR_DIM, CLR_BG, false);

  // Bright filled arc
  if (value > 0 && maxVal > 0) {
    float pct = constrain(value / maxVal, 0.0f, 1.0f);
    uint32_t span = (uint32_t)(G_SPAN * pct);
    if (span > 1) {
      uint32_t endAngle = (G_START + span) % 360;
      ui.tft.drawSmoothArc(GX, GY, GOR, GIR, G_START, endAngle, color, CLR_BG, true);
    }
  }

  // Speed value in center of arc — keep within inner radius
  ui.tft.fillRect(GX - 50, GY - 18, 100, 42, CLR_BG);
  char buf[16];
  if (value >= 100)
    snprintf(buf, sizeof(buf), "%.0f", value);
  else if (value >= 10)
    snprintf(buf, sizeof(buf), "%.1f", value);
  else
    snprintf(buf, sizeof(buf), "%.2f", value);
  drawCentered(buf, Arial_24_Bold, CLR_VAL, GY - 16);
  drawCentered("Mbps", Arial_12, CLR_MID, GY + 10);
}

void drawPhaseLabel(const char *label) {
  ui.tft.fillRect(0, 6, 240, 24, CLR_BG);
  char buf[32];
  snprintf(buf, sizeof(buf), ">> %s", label);
  drawCentered(buf, Arial_12, CLR_GREEN, 10);
  ui.tft.drawFastHLine(30, 30, 180, CLR_DIM);
}

void drawBottomResults() {
  ui.tft.fillRect(0, 224, 240, 16, CLR_BG);
  ui.tft.setTTFFont(Arial_12);
  char buf[16];

  if (pingMs > 0) {
    snprintf(buf, sizeof(buf), "%.0fms", pingMs);
    ui.tft.setTextColor(CLR_MID, CLR_BG);
    ui.tft.setCursor(10, 226);
    ui.tft.print("P:");
    ui.tft.setTextColor(CLR_GREEN, CLR_BG);
    ui.tft.print(buf);
  }
  if (downloadMbps > 0) {
    snprintf(buf, sizeof(buf), "%.2f", downloadMbps);
    ui.tft.setTextColor(CLR_MID, CLR_BG);
    ui.tft.setCursor(88, 226);
    ui.tft.print("D:");
    ui.tft.setTextColor(CLR_GREEN, CLR_BG);
    ui.tft.print(buf);
  }
  if (uploadMbps > 0) {
    snprintf(buf, sizeof(buf), "%.2f", uploadMbps);
    ui.tft.setTextColor(CLR_MID, CLR_BG);
    ui.tft.setCursor(168, 226);
    ui.tft.print("U:");
    ui.tft.setTextColor(CLR_GREEN, CLR_BG);
    ui.tft.print(buf);
  }
}

// --- Measurements ---

// Drain HTTP response body without heap allocation
static void drainResponse() {
  WiFiClient *stream = http.getStreamPtr();
  if (!stream)
    return;
  uint8_t discard[256];
  while (stream->available() > 0) {
    stream->read(discard, min((int)sizeof(discard), stream->available()));
  }
}

float measurePing() {
  http.begin(netClient, "http://speed.cloudflare.com/__down?bytes=1");
  http.setTimeout(10000);
  unsigned long start = millis();
  int code = http.GET();
  unsigned long elapsed = millis() - start;
  if (code == HTTP_CODE_OK)
    drainResponse();
  http.end();
  return (code == HTTP_CODE_OK) ? (float)elapsed : -1;
}

float measureDownload(int bytes) {
  snprintf(urlBuf, sizeof(urlBuf),
           "http://speed.cloudflare.com/__down?bytes=%d", bytes);
  http.begin(netClient, urlBuf);
  http.setTimeout(15000);
  http.addHeader("Accept-Encoding", "identity");

  unsigned long start = millis();
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("DL %d: HTTP %d\n", bytes, code);
    http.end();
    return -1;
  }

  int contentLen = http.getSize();
  unsigned long headerMs = millis() - start;

  WiFiClient *stream = http.getStreamPtr();
  stream->setTimeout(1);
  uint8_t buf[4096];
  int total = 0;
  unsigned long dataStart = millis();
  unsigned long lastData = dataStart;

  while (true) {
    unsigned long now = millis();
    if (now - start > 15000)
      break;
    if (total > 0 && now - lastData > 2000)
      break;
    if (contentLen > 0 && total >= contentLen)
      break;

    int avail = stream->available();
    if (avail > 0) {
      int n = stream->read(buf, min((int)sizeof(buf), avail));
      if (n > 0) {
        total += n;
        lastData = millis();
      }
    } else if (!http.connected()) {
      break;
    } else {
      delay(1);
    }
  }

  unsigned long dataElapsed = millis() - dataStart;
  Serial.printf("DL %d: %d bytes in %lu ms (data: %lu ms, hdr: %lu ms)\n",
                bytes, total, millis() - start, dataElapsed, headerMs);

  http.end();

  if (dataElapsed == 0 || total == 0)
    return 0;
  return (total * 8.0f) / (dataElapsed * 1000.0f);
}

float measureUpload(int bytes) {
  if (!ulBuf) {
    ulBuf = (uint8_t *)malloc(UL_BUF_SIZE);
    if (!ulBuf)
      return -1;
    memset(ulBuf, '0', UL_BUF_SIZE);
  }

  if (!netClient.connect("speed.cloudflare.com", 80)) {
    Serial.println("UL: connect failed");
    return -1;
  }

  netClient.printf("POST /__up HTTP/1.1\r\n"
                   "Host: speed.cloudflare.com\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: %d\r\n"
                   "Connection: close\r\n\r\n",
                   bytes);

  unsigned long start = millis();
  int sent = 0;
  while (sent < bytes) {
    int chunk = min(UL_BUF_SIZE, bytes - sent);
    int n = netClient.write(ulBuf, chunk);
    if (n <= 0)
      break;
    sent += n;
  }

  unsigned long elapsed = millis() - start;
  bool ok = false;

  // Read response with timeout to avoid hanging
  netClient.setTimeout(5);
  if (netClient.connected()) {
    String status = netClient.readStringUntil('\n');
    ok = status.indexOf("200") >= 0;
  }
  netClient.stop();

  Serial.printf("UL %d: sent %d bytes in %lu ms\n", bytes, sent, elapsed);

  if (!ok || elapsed == 0 || sent == 0)
    return -1;
  return (sent * 8.0f) / (elapsed * 1000.0f);
}

// --- Test sequence ---

void runSpeedTest() {
  // Ensure clean connection state from any previous run
  netClient.stop();
  http.end();

  ui.tft.fillScreen(CLR_BG);
  pingMs = 0;
  downloadMbps = 0;
  uploadMbps = 0;

  // ---- Phase 1: Ping ----
  drawPhaseLabel("PING");

  float pings[5];
  int validPings = 0;
  for (int i = 0; i < 5; i++) {
    // Dot progress indicators
    for (int j = 0; j < 5; j++) {
      uint16_t dotClr = (j < i) ? CLR_GREEN : CLR_DIM;
      ui.tft.fillCircle(96 + j * 12, GY + 50, 3, dotClr);
    }

    float p = measurePing();
    if (p >= 0) {
      pings[validPings++] = p;
      float sum = 0;
      for (int j = 0; j < validPings; j++)
        sum += pings[j];

      ui.tft.fillRect(GX - 50, GY - 18, 100, 42, CLR_BG);
      char buf[16];
      snprintf(buf, sizeof(buf), "%.0f", sum / validPings);
      drawCentered(buf, Arial_24_Bold, CLR_VAL, GY - 16);
      drawCentered("ms", Arial_12, CLR_MID, GY + 10);
    }
    ui.tft.fillCircle(96 + i * 12, GY + 50, 3, CLR_GREEN);
  }

  if (validPings > 0) {
    float worst = 0, sum = 0;
    for (int i = 0; i < validPings; i++) {
      sum += pings[i];
      if (pings[i] > worst)
        worst = pings[i];
    }
    pingMs = (validPings > 1) ? (sum - worst) / (validPings - 1) : sum;
  }
  Serial.printf("Ping: %.0f ms\n", pingMs);
  drawBottomResults();
  delay(300);

  // ---- Phase 2: Download ----
  ui.tft.fillRect(0, 0, 240, 222, CLR_BG);
  drawPhaseLabel("DOWNLOAD");
  drawGauge(0, CLR_GREEN);

  int dlSizes[] = {500000, 1000000, 2000000, 2000000};
  float bestDl = 0;
  for (int i = 0; i < 4; i++) {
    float dl = measureDownload(dlSizes[i]);
    if (dl > bestDl)
      bestDl = dl;
    if (dl > 0)
      drawGauge(dl, CLR_GREEN);
  }
  downloadMbps = bestDl;
  Serial.printf("Download: %.2f Mbps\n", downloadMbps);
  drawBottomResults();
  delay(300);

  // ---- Phase 3: Upload ----
  ui.tft.fillRect(0, 0, 240, 222, CLR_BG);
  drawPhaseLabel("UPLOAD");
  drawGauge(0, CLR_GREEN);

  float bestUl = 0;
  int ulSizes[] = {250000, 500000, 1000000, 2000000};
  for (int i = 0; i < 4; i++) {
    float ul = measureUpload(ulSizes[i]);
    if (ul > bestUl)
      bestUl = ul;
    if (ul > 0)
      drawGauge(ul, CLR_GREEN);
  }
  uploadMbps = bestUl;
  Serial.printf("Upload: %.2f Mbps\n", uploadMbps);
  drawBottomResults();
  delay(300);

  // Free upload buffer — not needed until next test
  if (ulBuf) {
    free(ulBuf);
    ulBuf = nullptr;
  }

  drawResultScreen();
}

void drawResultScreen() {
  ui.tft.fillScreen(CLR_BG);

  // Header
  drawCentered("SPEEDTEST", Arial_24_Bold, CLR_GREEN, 8);
  ui.tft.drawFastHLine(30, 36, 180, CLR_DIM);

  char buf[32];

  // Ping
  drawCentered("PING", Arial_12, CLR_DIM, 48);
  snprintf(buf, sizeof(buf), "%.0f ms", pingMs);
  drawCentered(buf, Arial_20_Bold, CLR_GREEN, 64);

  ui.tft.drawFastHLine(60, 90, 120, CLR_VDIM);

  // Download
  drawCentered("DOWNLOAD", Arial_12, CLR_DIM, 100);
  snprintf(buf, sizeof(buf), "%.2f Mbps", downloadMbps);
  drawCentered(buf, Arial_20_Bold, CLR_GREEN, 116);

  ui.tft.drawFastHLine(60, 142, 120, CLR_VDIM);

  // Upload
  drawCentered("UPLOAD", Arial_12, CLR_DIM, 152);
  snprintf(buf, sizeof(buf), "%.2f Mbps", uploadMbps);
  drawCentered(buf, Arial_20_Bold, CLR_GREEN, 168);

  ui.tft.drawFastHLine(60, 194, 120, CLR_VDIM);

  drawCentered(">> press button to retest", Arial_12, CLR_VDIM, 216);
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting speed test app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ui.tft.fillScreen(CLR_BG);
  drawCentered("SPEEDTEST", Arial_24_Bold, CLR_GREEN, 80);
  ui.tft.drawFastHLine(30, 108, 180, CLR_DIM);
  drawCentered(">> press button to start", Arial_12, CLR_MID, 125);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT

    bool pressed = (digitalRead(BUTTON_PIN) == LOW);
    if (pressed && !buttonWasPressed) {
      delay(50);
      if (digitalRead(BUTTON_PIN) == LOW) {
        Serial.println("Button pressed - starting speed test");
        runSpeedTest();
      }
    }
    buttonWasPressed = pressed;
  }
  delay(1);
}
