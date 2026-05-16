#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <Preferences.h>

#define BUTTON_PIN 19

Preferences preferences;
OTAServer otaserver;
String serverUrl;
KGFX ui;
bool buttonWasPressed = false;

// --- Polling intervals ---
unsigned long lastMetadataFetch = 0;
const unsigned long METADATA_INTERVAL_MS = 2000;

// --- Track state ---
char trackTitle[128]  = "";
char trackArtist[128] = "";
char trackAlbum[128]  = "";
bool trackPlaying     = false;
int trackElapsed      = 0;
int trackDuration     = 0;
bool hasTrackData     = false;  // true once we've received valid metadata

// Previous track identity for detecting song changes
char prevTitle[128]  = "";
char prevArtist[128] = "";

// --- JPEG buffer ---
#define JPEG_BUF_SIZE 40000
uint8_t* jpegBuffer = nullptr;
size_t jpegSize = 0;
bool hasArtwork = false;

// --- View mode ---
enum ViewMode { VIEW_ARTWORK, VIEW_INFO };
ViewMode currentView = VIEW_ARTWORK;

// --- TJpg_Decoder callback: draw decoded MCU blocks to TFT ---
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= 240) return false;
  ui.tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// Format seconds as m:ss
void formatTime(int totalSeconds, char* buf, size_t bufSize) {
  if (totalSeconds < 0) totalSeconds = 0;
  int m = totalSeconds / 60;
  int s = totalSeconds % 60;
  snprintf(buf, bufSize, "%d:%02d", m, s);
}

// Draw the artwork view with overlay bar
void drawArtworkView() {
  if (hasArtwork && jpegSize > 0) {
    TJpgDec.drawJpg(0, 0, jpegBuffer, jpegSize);
  } else {
    ui.tft.fillScreen(TFT_BLACK);
    ui.tft.setTTFFont(Arial_14_Bold);
    ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    int w = ui.tft.TTFtextWidth("No artwork");
    ui.tft.setCursor((240 - w) / 2, 110);
    ui.tft.print("No artwork");
  }

  // Dark overlay bar at bottom for track info
  ui.tft.fillRect(0, 200, 240, 40, ui.tft.color565(20, 20, 20));

  // Title on first line
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(TFT_WHITE, ui.tft.color565(20, 20, 20));
  ui.tft.setCursor(6, 204);

  // Truncate title if too wide
  char displayTitle[64];
  strncpy(displayTitle, trackTitle, sizeof(displayTitle) - 1);
  displayTitle[sizeof(displayTitle) - 1] = '\0';
  while (strlen(displayTitle) > 1 && ui.tft.TTFtextWidth(displayTitle) > 228) {
    displayTitle[strlen(displayTitle) - 1] = '\0';
  }
  ui.tft.print(displayTitle);

  // Artist on second line
  ui.tft.setCursor(6, 222);
  char displayArtist[64];
  strncpy(displayArtist, trackArtist, sizeof(displayArtist) - 1);
  displayArtist[sizeof(displayArtist) - 1] = '\0';
  while (strlen(displayArtist) > 1 && ui.tft.TTFtextWidth(displayArtist) > 228) {
    displayArtist[strlen(displayArtist) - 1] = '\0';
  }
  ui.tft.setTextColor(0xBDF7, ui.tft.color565(20, 20, 20));  // light gray
  ui.tft.print(displayArtist);

  // Thin progress bar at very bottom (2px)
  if (trackDuration > 0) {
    int barW = (int)((float)trackElapsed / trackDuration * 240);
    if (barW > 240) barW = 240;
    if (barW > 0) ui.tft.fillRect(0, 238, barW, 2, TFT_WHITE);
    if (barW < 240) ui.tft.fillRect(barW, 238, 240 - barW, 2, ui.tft.color565(40, 40, 40));
  }
}

// Draw the info detail view
void drawInfoView() {
  ui.tft.fillScreen(TFT_BLACK);

  uint16_t gray = 0x7BEF;
  uint16_t darkGray = 0x4208;
  int y = 20;

  // Title
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  ui.tft.setCursor(12, y);
  ui.tft.print("Title");
  y += 22;

  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(gray, TFT_BLACK);
  ui.tft.setCursor(12, y);
  // Word-wrap title if needed (simple: just truncate for now)
  char buf[128];
  strncpy(buf, trackTitle, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  ui.tft.print(buf);
  y += 28;

  // Separator
  ui.tft.drawFastHLine(12, y, 216, darkGray);
  y += 10;

  // Artist
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  ui.tft.setCursor(12, y);
  ui.tft.print("Artist");
  y += 22;

  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(gray, TFT_BLACK);
  ui.tft.setCursor(12, y);
  ui.tft.print(trackArtist);
  y += 28;

  // Separator
  ui.tft.drawFastHLine(12, y, 216, darkGray);
  y += 10;

  // Album
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  ui.tft.setCursor(12, y);
  ui.tft.print("Album");
  y += 22;

  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(gray, TFT_BLACK);
  ui.tft.setCursor(12, y);
  ui.tft.print(trackAlbum);
  y += 28;

  // Separator
  ui.tft.drawFastHLine(12, y, 216, darkGray);
  y += 10;

  // Elapsed / Duration
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  ui.tft.setCursor(12, y);
  ui.tft.print("Time");
  y += 22;

  char elapsedStr[16], durationStr[16];
  formatTime(trackElapsed, elapsedStr, sizeof(elapsedStr));
  formatTime(trackDuration, durationStr, sizeof(durationStr));

  char timeStr[40];
  snprintf(timeStr, sizeof(timeStr), "%s / %s", elapsedStr, durationStr);

  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(gray, TFT_BLACK);
  ui.tft.setCursor(12, y);
  ui.tft.print(timeStr);
  y += 20;

  // Progress bar
  y += 6;
  ui.tft.drawRect(12, y, 216, 8, darkGray);
  if (trackDuration > 0) {
    int fillW = (int)((float)trackElapsed / trackDuration * 214);
    if (fillW > 214) fillW = 214;
    if (fillW > 0) {
      ui.tft.fillRect(13, y + 1, fillW, 6, 0xFC10);  // orange accent
    }
  }

  // Playing status
  y += 20;
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(trackPlaying ? 0x07E0 : 0xF800, TFT_BLACK);
  ui.tft.setCursor(12, y);
  ui.tft.print(trackPlaying ? "Playing" : "Paused");
}

// Update just the progress bar (no JPEG redecode)
void updateProgressBar() {
  if (trackDuration > 0) {
    int barW = (int)((float)trackElapsed / trackDuration * 240);
    if (barW > 240) barW = 240;
    if (barW > 0) ui.tft.fillRect(0, 238, barW, 2, TFT_WHITE);
    if (barW < 240) ui.tft.fillRect(barW, 238, 240 - barW, 2, ui.tft.color565(40, 40, 40));
  }
}

// Draw idle screen when no music server or no track data
void drawIdleView() {
  ui.tft.fillScreen(TFT_BLACK);
  ui.drawCentered("No music playing", Arial_14_Bold, 0x7BEF, 110);
}

// Draw current view
void drawUI() {
  if (!hasTrackData) {
    drawIdleView();
    return;
  }
  if (currentView == VIEW_ARTWORK) {
    drawArtworkView();
  } else {
    drawInfoView();
  }
}

// Fetch track metadata from server
bool fetchMetadata() {
  if (serverUrl.length() == 0) {
    Serial.println("No server_url configured");
    return false;
  }

  HTTPClient http;
  http.begin(serverUrl + "/api/music");
  http.setTimeout(5000);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Metadata fetch failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Metadata JSON error: %s\n", err.c_str());
    return false;
  }

  const char* title  = doc["title"]  | "";
  const char* artist = doc["artist"] | "";
  const char* album  = doc["album"]  | "";

  strncpy(trackTitle,  title,  sizeof(trackTitle)  - 1);
  strncpy(trackArtist, artist, sizeof(trackArtist) - 1);
  strncpy(trackAlbum,  album,  sizeof(trackAlbum)  - 1);
  trackTitle[sizeof(trackTitle)   - 1] = '\0';
  trackArtist[sizeof(trackArtist) - 1] = '\0';
  trackAlbum[sizeof(trackAlbum)   - 1] = '\0';

  trackPlaying  = doc["playing"]  | false;
  trackElapsed  = doc["elapsed"]  | 0;
  trackDuration = doc["duration"] | 0;

  hasTrackData = true;
  Serial.printf("Track: %s - %s [%s] %s %d/%d\n",
                trackArtist, trackTitle, trackAlbum,
                trackPlaying ? "playing" : "paused",
                trackElapsed, trackDuration);
  return true;
}

// Fetch artwork JPEG from server into buffer
bool fetchArtwork() {
  HTTPClient http;
  http.begin(serverUrl + "/api/music/artwork");
  http.setTimeout(10000);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Artwork fetch failed: HTTP %d\n", code);
    http.end();
    hasArtwork = false;
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0 || contentLength > (int)JPEG_BUF_SIZE) {
    Serial.printf("Artwork bad size: %d\n", contentLength);
    http.end();
    hasArtwork = false;
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t bytesRead = 0;
  unsigned long startMs = millis();

  while (bytesRead < (size_t)contentLength && (millis() - startMs) < 10000) {
    if (stream->available()) {
      int toRead = min((size_t)stream->available(), (size_t)contentLength - bytesRead);
      int read = stream->readBytes(jpegBuffer + bytesRead, toRead);
      if (read > 0) bytesRead += read;
    } else {
      delay(1);
    }
  }

  http.end();

  if (bytesRead != (size_t)contentLength) {
    Serial.printf("Artwork incomplete: got %d of %d\n", bytesRead, contentLength);
    hasArtwork = false;
    return false;
  }

  jpegSize = bytesRead;
  hasArtwork = true;
  Serial.printf("Artwork loaded: %d bytes\n", jpegSize);
  return true;
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting music app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  preferences.begin("app", true);
  serverUrl = preferences.getString("server_url");
  preferences.end();
  Serial.printf("Server URL: %s\n", serverUrl.c_str());

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Allocate JPEG buffer
  jpegBuffer = (uint8_t*)malloc(JPEG_BUF_SIZE);
  if (!jpegBuffer) {
    Serial.println("ERROR: Failed to allocate JPEG buffer!");
  }

  // Setup TJpg_Decoder
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);

  // Show loading screen
  ui.tft.fillScreen(TFT_BLACK);
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int w = ui.tft.TTFtextWidth("Connecting...");
  ui.tft.setCursor((240 - w) / 2, 110);
  ui.tft.print("Connecting...");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT

    // Poll metadata every 2 seconds
    if (lastMetadataFetch == 0 || (millis() - lastMetadataFetch) > METADATA_INTERVAL_MS) {
      lastMetadataFetch = millis();

      // Save previous track identity before fetching
      char oldTitle[128], oldArtist[128];
      strncpy(oldTitle,  trackTitle,  sizeof(oldTitle));
      strncpy(oldArtist, trackArtist, sizeof(oldArtist));
      oldTitle[sizeof(oldTitle)   - 1] = '\0';
      oldArtist[sizeof(oldArtist) - 1] = '\0';

      if (fetchMetadata()) {
        // Check if song changed
        bool songChanged = (strcmp(oldTitle, trackTitle) != 0 ||
                            strcmp(oldArtist, trackArtist) != 0);

        if (songChanged) {
          Serial.println("Song changed, fetching new artwork");
          fetchArtwork();
          drawUI(); // always redraw on song change
        } else if (currentView == VIEW_ARTWORK) {
          updateProgressBar(); // lightweight update, no JPEG redecode
        } else {
          drawUI(); // update elapsed time in info view
        }
      } else if (hasTrackData) {
        // Server went away — clear stale track and show idle screen
        hasTrackData = false;
        hasArtwork = false;
        trackTitle[0] = '\0';
        trackArtist[0] = '\0';
        trackAlbum[0] = '\0';
        drawUI();
      } else if (lastMetadataFetch != 0) {
        // First draw — replace "Connecting..." with idle screen
        static bool shownIdle = false;
        if (!shownIdle) {
          shownIdle = true;
          drawUI();
        }
      }
    }

    // Button press — toggle between artwork and info view
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);
    if (pressed && !buttonWasPressed) {
      delay(50);  // debounce
      if (digitalRead(BUTTON_PIN) == LOW) {
        currentView = (currentView == VIEW_ARTWORK) ? VIEW_INFO : VIEW_ARTWORK;
        Serial.printf("View: %s\n", currentView == VIEW_ARTWORK ? "artwork" : "info");
        drawUI();
      }
    }
    buttonWasPressed = pressed;
  }

  delay(1);
}
