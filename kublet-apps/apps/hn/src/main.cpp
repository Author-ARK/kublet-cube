#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;
bool buttonWasPressed = false;

// HN API endpoints
const char* TOP_STORIES_URL = "https://hacker-news.firebaseio.com/v0/topstories.json";
const char* ITEM_URL_FMT = "https://hacker-news.firebaseio.com/v0/item/%ld.json";

// Story data
#define NUM_STORIES 10
struct Story {
  long id;
  char title[200];
  int score;
  int comments;
  bool valid;
};
Story stories[NUM_STORIES];
int currentStory = 0;

// Timing
unsigned long lastFetch = 0;
const unsigned long FETCH_INTERVAL = 5 * 60 * 1000; // refresh from API every 5 min
unsigned long lastCycle = 0;
const unsigned long CYCLE_INTERVAL = 30 * 1000; // auto-cycle every 30s

// Title sprite for word-wrapped text
TFT_eSprite titleSpr = TFT_eSprite(&ui.t);

// Colors
#define CLR_BG      TFT_BLACK
#define CLR_TITLE   TFT_WHITE
#define CLR_SCORE   0xEB85  // HN orange (237, 112, 46)
#define CLR_COMMENTS 0x7BEF // light gray
#define CLR_INDEX   0x4208  // dim gray
#define CLR_DIVIDER 0x2104  // subtle gray

// --- HTTP helper ---

WiFiClientSecure secureClient;

String httpGet(const char* url) {
  HTTPClient http;
  http.begin(secureClient, url);
  http.setTimeout(10000);
  int code = http.GET();
  String payload = "";
  if (code > 0) {
    payload = http.getString();
  } else {
    Serial.printf("HTTP error: %d for %s\n", code, url);
  }
  http.end();
  return payload;
}

// --- Fetch stories ---

bool fetchStories() {
  Serial.println("Fetching top stories...");

  // Step 1: get top story IDs
  String payload = httpGet(TOP_STORIES_URL);
  if (payload.length() == 0) return false;

  JsonDocument idDoc;
  DeserializationError err = deserializeJson(idDoc, payload);
  payload = String(); // free memory
  if (err) {
    Serial.printf("JSON error (ids): %s\n", err.c_str());
    return false;
  }

  JsonArray ids = idDoc.as<JsonArray>();
  if (ids.size() < NUM_STORIES) return false;

  // Step 2: fetch each story's details
  for (int i = 0; i < NUM_STORIES; i++) {
    long id = ids[i].as<long>();
    stories[i].id = id;
    stories[i].valid = false;

    char url[128];
    snprintf(url, sizeof(url), ITEM_URL_FMT, id);

    String itemPayload = httpGet(url);
    if (itemPayload.length() == 0) continue;

    // Use filter to save memory
    JsonDocument filter;
    filter["title"] = true;
    filter["score"] = true;
    filter["descendants"] = true;

    JsonDocument itemDoc;
    err = deserializeJson(itemDoc, itemPayload,
                          DeserializationOption::Filter(filter));
    itemPayload = String();
    if (err) {
      Serial.printf("JSON error (item %ld): %s\n", id, err.c_str());
      continue;
    }

    const char* title = itemDoc["title"].as<const char*>();
    if (title) {
      strncpy(stories[i].title, title, sizeof(stories[i].title) - 1);
      stories[i].title[sizeof(stories[i].title) - 1] = '\0';
    } else {
      strcpy(stories[i].title, "(no title)");
    }
    stories[i].score = itemDoc["score"] | 0;
    stories[i].comments = itemDoc["descendants"] | 0;
    stories[i].valid = true;

    Serial.printf("  #%d: %s (%d pts, %d comments)\n",
                  i + 1, stories[i].title, stories[i].score, stories[i].comments);
  }

  return true;
}

// --- Drawing ---

// Draw HN icon: orange square with white "Y" inside, using a sprite to clip
TFT_eSprite iconSpr = TFT_eSprite(&ui.t);

void drawHNIcon(int x, int y) {
  #define ICON_SZ 24
  iconSpr.fillSprite(CLR_BG);
  // Orange fill with white border
  iconSpr.fillRect(0, 0, ICON_SZ, ICON_SZ, CLR_SCORE);
  iconSpr.drawRect(0, 0, ICON_SZ, ICON_SZ, TFT_WHITE);
  // Render "Y" into sprite — clips any overflow
  ui.tft.TTFdestination(&iconSpr);
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, CLR_SCORE);
  int yw = ui.tft.TTFtextWidth("Y");
  ui.tft.setCursor((ICON_SZ - yw) / 2, 4);
  ui.tft.print("Y");
  ui.tft.TTFdestination(&ui.t);
  // Redraw border in case text overwrote edge pixels
  iconSpr.drawRect(0, 0, ICON_SZ, ICON_SZ, TFT_WHITE);
  iconSpr.pushSprite(x, y);
}

// Word-wrap text into sprite, respecting word boundaries
void drawWrappedText(const char* text, int sprW) {
  titleSpr.fillSprite(CLR_BG);
  ui.tft.TTFdestination(&titleSpr);
  ui.tft.setTTFFont(Arial_16_Bold);
  ui.tft.setTextColor(CLR_TITLE, CLR_BG);

  int lineH = 22; // line height for Arial_16_Bold
  int cx = 0, cy = 0;
  char word[64];
  int wi = 0;
  const char* p = text;

  while (true) {
    char c = *p;
    bool endOfWord = (c == ' ' || c == '\0' || c == '-');

    if (endOfWord && wi > 0) {
      // Include the hyphen in the word
      if (c == '-') word[wi++] = '-';
      word[wi] = '\0';

      int ww = ui.tft.TTFtextWidth(word);
      int spaceW = (cx > 0) ? ui.tft.TTFtextWidth(" ") : 0;

      if (cx + spaceW + ww > sprW && cx > 0) {
        // Wrap to next line
        cx = 0;
        cy += lineH;
      } else if (cx > 0) {
        // Add space before word
        ui.tft.setCursor(cx, cy);
        ui.tft.print(" ");
        cx += spaceW;
      }

      ui.tft.setCursor(cx, cy);
      ui.tft.print(word);
      cx += ww;
      wi = 0;
    } else if (c != ' ' && c != '\0') {
      if (wi < 63) word[wi++] = c;
    }

    if (c == '\0') break;
    p++;
  }

  ui.tft.TTFdestination(&ui.t);
}

void drawStory() {
  ui.tft.fillScreen(CLR_BG);

  Story& s = stories[currentStory];

  if (!s.valid) {
    ui.drawTextCenter("Loading...", Arial_14_Bold, CLR_TITLE, 110);
    return;
  }

  // HN icon top-left + "Hacker News" label
  drawHNIcon(6, 2);
  // Vertically center text next to 24px icon (icon at y=2, so center = 2 + 12 = 14)
  ui.tft.setTTFFont(Arial_13_Bold);
  ui.tft.setTextColor(CLR_SCORE, CLR_BG);
  int hnY = 2 + (ICON_SZ - 16) / 2; // 16 ~= Arial_13_Bold cap height
  ui.tft.setCursor(34, hnY);
  ui.tft.print("Hacker News");

  // Story index indicator at top-right, same vertical alignment
  char indexBuf[8];
  snprintf(indexBuf, sizeof(indexBuf), "%d/%d", currentStory + 1, NUM_STORIES);
  ui.tft.setTTFFont(Arial_12_Bold);
  ui.tft.setTextColor(CLR_INDEX, CLR_BG);
  int iw = ui.tft.TTFtextWidth(indexBuf);
  ui.tft.setCursor(240 - iw - 6, hnY + 1);
  ui.tft.print(indexBuf);

  // Divider line
  ui.tft.drawFastHLine(0, 28, 240, CLR_DIVIDER);

  // Title — word-wrapped (no mid-word splits)
  drawWrappedText(s.title, 220);
  titleSpr.pushSprite(10, 36);

  // Bottom section: score and comments
  // Divider
  ui.tft.drawFastHLine(0, 200, 240, CLR_DIVIDER);

  // Score
  char scoreBuf[32];
  snprintf(scoreBuf, sizeof(scoreBuf), "%d points", s.score);
  ui.tft.setTTFFont(Arial_12_Bold);
  ui.tft.setTextColor(CLR_SCORE, CLR_BG);
  ui.tft.setCursor(10, 211);
  ui.tft.print(scoreBuf);

  // Comments
  char commentBuf[32];
  snprintf(commentBuf, sizeof(commentBuf), "%d comments", s.comments);
  ui.tft.setTTFFont(Arial_12_Bold);
  ui.tft.setTextColor(CLR_COMMENTS, CLR_BG);
  int cw = ui.tft.TTFtextWidth(commentBuf);
  ui.tft.setCursor(240 - cw - 10, 211);
  ui.tft.print(commentBuf);
}

void showLoading() {
  ui.tft.fillScreen(CLR_BG);
  ui.drawTextCenter("Fetching HN...", Arial_14_Bold, CLR_SCORE, 110);
}

// --- Arduino ---

void setup() {
  Serial.begin(460800);
  Serial.println("Starting hn app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Init HTTPS client
  secureClient.setInsecure();

  // Create sprites
  titleSpr.setColorDepth(16);
  titleSpr.createSprite(220, 160);
  iconSpr.setColorDepth(16);
  iconSpr.createSprite(ICON_SZ, ICON_SZ);

  // Init stories
  for (int i = 0; i < NUM_STORIES; i++) {
    stories[i].valid = false;
  }

  showLoading();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  unsigned long now = millis();

  // Fetch stories on startup and every FETCH_INTERVAL
  if (lastFetch == 0 || (now - lastFetch > FETCH_INTERVAL)) {
    if (WiFi.status() == WL_CONNECTED) {
      fetchStories();
      lastFetch = now;
      lastCycle = now;
      currentStory = 0;
      drawStory();
    }
  }

  // Auto-cycle through stories every 30s
  if (now - lastCycle > CYCLE_INTERVAL) {
    lastCycle = now;
    currentStory = (currentStory + 1) % NUM_STORIES;
    drawStory();
  }

  // Button: manually cycle to next story
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      currentStory = (currentStory + 1) % NUM_STORIES;
      lastCycle = now; // reset auto-cycle timer
      drawStory();
      Serial.printf("Button: story %d/%d\n", currentStory + 1, NUM_STORIES);
    }
  }
  buttonWasPressed = pressed;

  delay(10);
}
