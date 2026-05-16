#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>
#include <time.h>

#define BUTTON_PIN 19
#define JPEG_BUF_SIZE 80000   // 80KB — plenty for proxy-resized images (~20-50KB)
#define CYCLE_MS      60000   // 1 minute between images
#define MAX_DAYS      30      // cycle through last 30 days

OTAServer otaserver;
KGFX ui;

// State
uint8_t* jpegBuf = nullptr;
size_t   jpegLen = 0;
int      dayOffset = 0;
unsigned long lastCycle = 0;
bool     btnPrev = true;
char     apodTitle[120] = "";
char     apodDate[12] = "";

// --- JPEG DECODER CALLBACK ---
bool jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= 240) return false;
  ui.tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// --- DATE HELPER ---
void getDateInfo(int daysBack, int& year2, int& month, int& day) {
  time_t now;
  time(&now);
  now -= (time_t)daysBack * 86400L;
  struct tm* t = localtime(&now);
  year2 = t->tm_year % 100;
  month = t->tm_mon + 1;
  day   = t->tm_mday;

  const char* months[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
  };
  snprintf(apodDate, sizeof(apodDate), "%s %d", months[t->tm_mon], t->tm_mday);
}

// --- FETCH APOD HTML PAGE, EXTRACT IMAGE URL + TITLE ---
bool getAPODInfo(int daysBack, String& imageUrl) {
  int y2, m, d;
  getDateInfo(daysBack, y2, m, d);

  char pageUrl[100];
  snprintf(pageUrl, sizeof(pageUrl),
    "https://apod.nasa.gov/apod/ap%02d%02d%02d.html", y2, m, d);
  Serial.printf("APOD page: %s\n", pageUrl);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, pageUrl);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Page HTTP %d\n", code);
    http.end();
    return false;
  }

  String html = http.getString();
  http.end();

  // Case-insensitive search (APOD uses mixed case HTML tags)
  String lower = html;
  lower.toLowerCase();

  // Verify it's an image day (not video/iframe)
  int hrefIdx = lower.indexOf("href=\"image/");
  if (hrefIdx < 0) {
    Serial.println("Not an image day (video?)");
    return false;
  }

  // Find <img src="..."> after the href
  int srcIdx = lower.indexOf("src=\"", hrefIdx);
  if (srcIdx < 0) return false;
  srcIdx += 5;
  int srcEnd = html.indexOf('"', srcIdx);
  if (srcEnd < 0) return false;

  String imgPath = html.substring(srcIdx, srcEnd);

  // Only handle JPEG images (skip PNG, GIF, etc.)
  String extCheck = imgPath;
  extCheck.toLowerCase();
  if (!extCheck.endsWith(".jpg") && !extCheck.endsWith(".jpeg")) {
    Serial.printf("Not JPEG: %s\n", imgPath.c_str());
    return false;
  }

  // Route through wsrv.nl proxy to:
  //  - Convert progressive JPEGs to baseline (tjpgd only supports baseline)
  //  - Resize to 480px max (reduces ~300KB originals to ~30KB)
  String originalUrl = "https://apod.nasa.gov/apod/" + imgPath;
  imageUrl = "https://wsrv.nl/?url=" + originalUrl
    + "&w=480&h=480&fit=inside&output=jpg&q=85";

  // Extract title from <b>...</b> after the image
  apodTitle[0] = '\0';
  int bIdx = lower.indexOf("<b>", srcEnd);
  if (bIdx >= 0) {
    bIdx += 3;
    int bEnd = lower.indexOf("</b>", bIdx);
    if (bEnd >= 0 && bEnd - bIdx < 300) {
      String raw = html.substring(bIdx, bEnd);
      int j = 0;
      bool inTag = false, sp = false;
      for (unsigned int i = 0; i < raw.length() && j < (int)sizeof(apodTitle) - 1; i++) {
        char c = raw[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (inTag) continue;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && sp) continue;
        apodTitle[j++] = c;
        sp = (c == ' ');
      }
      apodTitle[j] = '\0';
      while (j > 0 && apodTitle[j - 1] == ' ') apodTitle[--j] = '\0';
    }
  }

  Serial.printf("Title: %s\nImage: %s\n", apodTitle, imageUrl.c_str());
  return true;
}

// --- FETCH JPEG INTO BUFFER ---
bool fetchJPEG(const String& url) {
  Serial.printf("Fetching JPEG: %s\n", url.c_str());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(30000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Image HTTP %d\n", code);
    http.end();
    return false;
  }

  int contentLen = http.getSize();
  if (contentLen > (int)JPEG_BUF_SIZE) {
    Serial.printf("Image too large: %d bytes (max %d)\n", contentLen, JPEG_BUF_SIZE);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  jpegLen = 0;
  unsigned long t0 = millis();
  while (http.connected() && jpegLen < JPEG_BUF_SIZE && millis() - t0 < 30000) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t toRead = min(avail, (size_t)(JPEG_BUF_SIZE - jpegLen));
      size_t n = stream->readBytes(jpegBuf + jpegLen, toRead);
      jpegLen += n;
      if (contentLen > 0 && (int)jpegLen >= contentLen) break;
    } else {
      delay(10);
    }
  }
  http.end();

  Serial.printf("JPEG loaded: %zu bytes\n", jpegLen);
  return jpegLen > 100;
}

// --- DISPLAY IMAGE WITH TITLE ---
void displayAPOD() {
  ui.tft.fillScreen(TFT_BLACK);

  if (jpegLen == 0) {
    ui.drawTextCenter("No Image", Arial_14_Bold, TFT_WHITE, 110);
    return;
  }

  uint16_t w, h;
  TJpgDec.getJpgSize(&w, &h, jpegBuf, jpegLen);
  Serial.printf("Image: %dx%d\n", w, h);

  // Scale to fit 240x240 — proxy already resizes to max 480px
  int scale = 1;
  if (max(w, h) > 400) scale = 2;

  TJpgDec.setJpgScale(scale);

  int sw = w / scale;
  int sh = h / scale;
  int ox = (240 - sw) / 2;
  int oy = (240 - sh) / 2;

  Serial.printf("Scale: 1/%d -> %dx%d, offset: (%d, %d)\n", scale, sw, sh, ox, oy);
  TJpgDec.drawJpg(ox, oy, jpegBuf, jpegLen);

  // Title bar at bottom
  if (apodTitle[0]) {
    ui.tft.fillRect(0, 218, 240, 22, TFT_BLACK);

    char label[160];
    if (apodDate[0]) {
      snprintf(label, sizeof(label), "%s - %s", apodDate, apodTitle);
    } else {
      strncpy(label, apodTitle, sizeof(label));
      label[sizeof(label) - 1] = '\0';
    }

    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(ui.tft.color565(210, 210, 220), TFT_BLACK);
    int tw = ui.tft.TTFtextWidth(label);
    int tx = max(4, (240 - tw) / 2);
    ui.tft.setCursor(tx, 223);
    ui.tft.print(label);
  }
}

// --- LOAD AND DISPLAY AN APOD ---
bool loadAPOD(int daysBack) {
  String imgUrl;
  if (!getAPODInfo(daysBack, imgUrl)) return false;
  if (!fetchJPEG(imgUrl)) return false;
  displayAPOD();
  return true;
}

// Advance to next image, skipping failures
void advanceImage() {
  ui.tft.fillRect(0, 218, 240, 22, TFT_BLACK);
  ui.drawTextCenter("Loading...", Arial_12, ui.tft.color565(120, 120, 140), 223);

  for (int tries = 0; tries < 5; tries++) {
    if (loadAPOD(dayOffset)) {
      lastCycle = millis();
      return;
    }
    dayOffset = (dayOffset + 1) % MAX_DAYS;
  }

  ui.tft.fillScreen(TFT_BLACK);
  ui.drawTextCenter("Could not load", Arial_14_Bold, TFT_WHITE, 105);
  ui.drawTextCenter("any images", Arial_12, ui.tft.color565(150, 150, 160), 130);
  lastCycle = millis();
}

// --- SETUP ---
void setup() {
  Serial.begin(460800);
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Allocate JPEG buffer
  jpegBuf = (uint8_t*)malloc(JPEG_BUF_SIZE);
  if (!jpegBuf) {
    Serial.println("JPEG buffer alloc failed!");
  }

  // Setup JPEG decoder
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpgOutput);

  // Wait for NTP time sync
  configTime(0, 0, "pool.ntp.org");
  Serial.print("NTP sync");
  time_t now = 0;
  for (int i = 0; i < 30 && now < 100000; i++) {
    delay(500);
    time(&now);
    Serial.print(".");
  }
  Serial.println(now > 100000 ? " OK" : " FAILED");

  // Show splash
  ui.tft.fillScreen(TFT_BLACK);
  ui.drawTextCenter("APOD", Arial_14_Bold, TFT_WHITE, 105);
  ui.drawTextCenter("Loading...", Arial_12, ui.tft.color565(150, 150, 160), 130);

  advanceImage();
}

// --- LOOP ---
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  bool btn = digitalRead(BUTTON_PIN);
  if (!btn && btnPrev) {
    dayOffset = (dayOffset + 1) % MAX_DAYS;
    advanceImage();
  }
  btnPrev = btn;

  if (millis() - lastCycle >= CYCLE_MS) {
    dayOffset = (dayOffset + 1) % MAX_DAYS;
    advanceImage();
  }
}
