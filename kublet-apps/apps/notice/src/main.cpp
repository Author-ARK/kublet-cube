#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>

// Access the global WebServer from OTAServer (defined in otaserver.cpp)
extern WebServer server;

Preferences preferences;
OTAServer otaserver;
KGFX ui;
String serverUrl;

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------
#define BUTTON_PIN 19
bool buttonWasPressed = false;

// ---------------------------------------------------------------------------
// Notification storage
// ---------------------------------------------------------------------------
#define MAX_NOTIFS 10
#define MAX_LINES 24       // wrap full text (more than visible)
#define MAX_VISIBLE_LINES 5 // lines that fit in the bubble
#define MAX_LINE_LEN 48

struct Notification {
  char source[16];    // "imessage", "slack", "test"
  char sender[64];
  char text[254];
  unsigned long arrivedAt; // millis() when notification was received
};

Notification notifs[MAX_NOTIFS];
int notifCount = 0;
int currentIdx = 0;
bool needsRedraw = true;

// Scroll state
int scrollOffset = 0;          // pixel offset for text scrolling
int scrollMaxOffset = 0;       // max scroll based on text height
unsigned long scrollStartTime = 0;  // when to start scrolling (after pause)
unsigned long lastScrollTime = 0;
bool scrollActive = false;
bool scrollPausedAtBottom = false;   // true while pausing at the bottom
unsigned long scrollBottomPauseEnd = 0;
#define SCROLL_PAUSE_MS 2000   // pause before scrolling starts
#define SCROLL_SPEED_MS 40     // ms per pixel of scroll (slower = smoother)
#define SCROLL_BOTTOM_PAUSE_MS 3000  // pause at bottom before resetting

// Cached wrap results for current notification
char cachedLines[MAX_LINES][MAX_LINE_LEN];
int cachedNumLines = 0;
int cachedIdx = -1;

// Timestamp display
unsigned long lastTimeUpdate = 0;
#define TIME_UPDATE_INTERVAL 15000  // refresh timestamp every 15s

void formatTimeAgo(unsigned long arrivedAt, char* buf, int bufLen) {
  unsigned long elapsed = (millis() - arrivedAt) / 1000;  // seconds
  if (elapsed < 60) {
    snprintf(buf, bufLen, "now");
  } else {
    int mins = elapsed / 60;
    snprintf(buf, bufLen, "%dm ago", mins);
  }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
unsigned long lastRegister = 0;
bool registered = false;
#define REGISTER_INTERVAL 900000       // 15 minutes
#define REGISTER_RETRY_INTERVAL 30000  // 30 seconds on failure

void registerWithServer() {
  if (serverUrl.length() == 0) return;

  HTTPClient http;
  String url = serverUrl + "/api/notice/register?ip=" + WiFi.localIP().toString();
  http.begin(url);
  int code = http.GET();
  http.end();
  lastRegister = millis();

  if (code == 200) {
    if (!registered) Serial.println("Register: connected to server");
    registered = true;
  } else {
    Serial.printf("Register: %s -> %d (retrying in 30s)\n", url.c_str(), code);
    registered = false;
  }
}

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------
#define COL_IMESSAGE    0x2D7F  // iOS blue  (#34AAFC → approx)
#define COL_IMESSAGE_BG 0x0A2A  // dark blue tint
#define COL_SLACK       0x4C0A  // Slack aubergine (#4A154B → approx)
#define COL_SLACK_BG    0x2085  // dark purple tint
#define COL_BUBBLE      0x2104  // dark gray bubble bg (#202020)
#define COL_SENDER      0xFFFF  // white
#define COL_TEXT         0xC618  // light gray
#define COL_SUBTEXT     0x7BCF  // medium gray

// ---------------------------------------------------------------------------
// Source icons (large, drawn into colored circles)
// ---------------------------------------------------------------------------

// Draw a large speech bubble icon centered at (cx, cy)
// Uses three overlapping circles for a smooth rounded shape + a small tail
void drawSpeechBubbleIcon(int cx, int cy, uint16_t color) {
  // Three overlapping circles form a wide rounded rectangle
  int r = 7;
  ui.tft.fillCircle(cx - 5, cy - 2, r, color);
  ui.tft.fillCircle(cx + 5, cy - 2, r, color);
  ui.tft.fillCircle(cx, cy - 2, r, color);
  // Fill center gap
  ui.tft.fillRect(cx - 5, cy - 9, 10, 14, color);
  // Small tail at bottom-left
  ui.tft.fillTriangle(cx - 5, cy + 3, cx, cy + 3, cx - 7, cy + 9, color);
}

// Draw a hash/pound icon centered at (cx, cy)
void drawSlackIcon(int cx, int cy, uint16_t color) {
  // Two vertical bars
  for (int d = -4; d <= 4; d += 8) {
    ui.tft.fillRect(cx + d - 1, cy - 8, 3, 16, color);
  }
  // Two horizontal bars
  for (int d = -3; d <= 3; d += 6) {
    ui.tft.fillRect(cx - 8, cy + d - 1, 16, 3, color);
  }
}

// Draw a bell icon centered at (cx, cy) — generic/default
void drawBellIcon(int cx, int cy, uint16_t color) {
  ui.tft.fillCircle(cx, cy - 6, 4, color);
  ui.tft.fillTriangle(cx - 8, cy + 3, cx + 8, cy + 3, cx, cy - 8, color);
  ui.tft.fillRect(cx - 8, cy + 1, 17, 3, color);
  ui.tft.fillCircle(cx, cy + 6, 2, color);
}

// Draw a game controller / Discord icon centered at (cx, cy)
void drawDiscordIcon(int cx, int cy, uint16_t color) {
  // Rounded body
  ui.tft.fillRoundRect(cx - 9, cy - 5, 18, 12, 4, color);
  // Two "eyes"
  ui.tft.fillCircle(cx - 4, cy - 1, 2, TFT_BLACK);
  ui.tft.fillCircle(cx + 4, cy - 1, 2, TFT_BLACK);
  // Antenna/horns
  ui.tft.fillRect(cx - 7, cy - 8, 3, 5, color);
  ui.tft.fillRect(cx + 4, cy - 8, 3, 5, color);
}

// Draw an envelope icon centered at (cx, cy)
void drawEnvelopeIcon(int cx, int cy, uint16_t color) {
  ui.tft.fillRect(cx - 9, cy - 5, 18, 13, color);
  // Flap (two triangles forming a V)
  ui.tft.fillTriangle(cx - 9, cy - 5, cx, cy + 2, cx + 9, cy - 5, TFT_BLACK);
  ui.tft.fillTriangle(cx - 9, cy - 6, cx, cy + 1, cx + 9, cy - 6, color);
}

// Draw a calendar icon centered at (cx, cy)
void drawCalendarIcon(int cx, int cy, uint16_t color) {
  // Body
  ui.tft.fillRoundRect(cx - 8, cy - 6, 16, 15, 2, color);
  // Top bar (darker)
  ui.tft.fillRect(cx - 8, cy - 6, 16, 5, color);
  // Two ring hooks
  ui.tft.fillRect(cx - 4, cy - 9, 2, 5, color);
  ui.tft.fillRect(cx + 3, cy - 9, 2, 5, color);
  // Date area (dark inset)
  ui.tft.fillRect(cx - 6, cy + 1, 12, 6, TFT_BLACK);
}

// Draw a phone icon centered at (cx, cy) — FaceTime
void drawPhoneIcon(int cx, int cy, uint16_t color) {
  // Phone body
  ui.tft.fillRoundRect(cx - 4, cy - 8, 8, 16, 3, color);
  // Screen (dark inset)
  ui.tft.fillRect(cx - 2, cy - 5, 4, 9, TFT_BLACK);
  // Home button
  ui.tft.fillCircle(cx, cy + 5, 1, TFT_BLACK);
}

// Draw a wallet/card icon centered at (cx, cy)
void drawWalletIcon(int cx, int cy, uint16_t color) {
  ui.tft.fillRoundRect(cx - 9, cy - 5, 18, 13, 2, color);
  // Card stripe
  ui.tft.fillRect(cx - 9, cy - 1, 18, 3, TFT_BLACK);
  // Clasp
  ui.tft.fillCircle(cx + 6, cy + 3, 2, TFT_BLACK);
}

// Draw a globe icon centered at (cx, cy) — Chrome/web
void drawGlobeIcon(int cx, int cy, uint16_t color) {
  ui.tft.drawCircle(cx, cy, 8, color);
  ui.tft.drawCircle(cx, cy, 4, color);
  // Horizontal line
  ui.tft.drawFastHLine(cx - 8, cy, 17, color);
  // Vertical line
  ui.tft.drawFastVLine(cx, cy - 8, 17, color);
}

// Draw a note/document icon centered at (cx, cy)
void drawNoteIcon(int cx, int cy, uint16_t color) {
  ui.tft.fillRect(cx - 6, cy - 8, 12, 16, color);
  // Lines of text
  for (int i = 0; i < 3; i++) {
    ui.tft.fillRect(cx - 4, cy - 5 + i * 4, 8 - i * 2, 2, TFT_BLACK);
  }
}

// ---------------------------------------------------------------------------
// Word wrapping
// ---------------------------------------------------------------------------

// Break a long word character-by-character across lines
int breakLongWord(const char* word, int wordLen, char lines[][MAX_LINE_LEN],
                  int lineCount, int maxLines, int maxWidth) {
  char temp[MAX_LINE_LEN];
  int pos = 0;
  while (pos < wordLen && lineCount < maxLines) {
    int fit = 0;
    for (int c = 1; c <= wordLen - pos && c < MAX_LINE_LEN - 1; c++) {
      snprintf(temp, sizeof(temp), "%.*s", c, word + pos);
      if (ui.tft.TTFtextWidth(temp) > maxWidth) break;
      fit = c;
    }
    if (fit == 0) fit = 1;  // always take at least one char
    snprintf(lines[lineCount], MAX_LINE_LEN, "%.*s", fit, word + pos);
    pos += fit;
    if (pos < wordLen) {
      lineCount++;
      if (lineCount >= maxLines) return maxLines;
    }
  }
  return lineCount;
}

int wrapText(const char* text, char lines[][MAX_LINE_LEN], int maxLines, int maxWidth) {
  int lineCount = 0;
  int linePos = 0;
  int wordStart = 0;
  int len = strlen(text);

  lines[0][0] = '\0';

  for (int i = 0; i <= len; i++) {
    if (text[i] == ' ' || text[i] == '\0' || text[i] == '\n') {
      char temp[MAX_LINE_LEN];
      int wordLen = i - wordStart;
      if (wordLen <= 0 && text[i] != '\n') {
        wordStart = i + 1;
        continue;
      }

      if (linePos > 0) {
        snprintf(temp, sizeof(temp), "%s %.*s", lines[lineCount], wordLen, text + wordStart);
      } else {
        snprintf(temp, sizeof(temp), "%.*s", wordLen, text + wordStart);
      }

      int tw = ui.tft.TTFtextWidth(temp);
      if (tw <= maxWidth) {
        strncpy(lines[lineCount], temp, MAX_LINE_LEN - 1);
        lines[lineCount][MAX_LINE_LEN - 1] = '\0';
        linePos += wordLen + 1;
      } else if (linePos == 0) {
        // Single word too wide — break it character by character
        lineCount = breakLongWord(text + wordStart, wordLen, lines, lineCount, maxLines, maxWidth);
        linePos = strlen(lines[lineCount]);
      } else {
        lineCount++;
        if (lineCount >= maxLines) return maxLines;
        // Check if the word itself fits on a new line
        snprintf(temp, sizeof(temp), "%.*s", wordLen, text + wordStart);
        if (ui.tft.TTFtextWidth(temp) <= maxWidth) {
          snprintf(lines[lineCount], MAX_LINE_LEN, "%.*s", wordLen, text + wordStart);
          linePos = wordLen;
        } else {
          // Word too wide even alone — break it
          lines[lineCount][0] = '\0';
          lineCount = breakLongWord(text + wordStart, wordLen, lines, lineCount, maxLines, maxWidth);
          linePos = strlen(lines[lineCount]);
        }
      }

      if (text[i] == '\n' && lineCount < maxLines - 1) {
        lineCount++;
        lines[lineCount][0] = '\0';
        linePos = 0;
      }

      wordStart = i + 1;
    }
  }

  return lineCount + 1;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void drawIdleScreen() {
  ui.tft.fillScreen(TFT_BLACK);

  // Large bell icon in center
  drawBellIcon(120, 100, 0x4228);

  ui.tft.setTTFFont(Arial_16);
  ui.tft.setTextColor(0x4228, TFT_BLACK);
  const char* msg = "No notifications";
  int mw = ui.tft.TTFtextWidth(msg);
  ui.tft.setCursor((240 - mw) / 2, 130);
  ui.tft.print(msg);
}

// Layout constants
#define BUBBLE_X 8
#define BUBBLE_Y 58
#define BUBBLE_W 224
#define BUBBLE_H 150
#define TEXT_INSET 12
#define LINE_HEIGHT 22
#define SENDER_H 24
#define TEXT_TOP (BUBBLE_Y + 10 + SENDER_H)
#define TEXT_BOTTOM (BUBBLE_Y + BUBBLE_H - 10)

void drawNotificationFrame(int idx, bool fullRedraw, int scrollPx);

void drawNotification(int idx) {
  // Cache wrapped text BEFORE drawing so the frame uses correct data
  if (idx >= 0 && idx < notifCount) {
    Notification& n = notifs[idx];
    ui.tft.setTTFFont(Arial_14);
    cachedNumLines = wrapText(n.text, cachedLines, MAX_LINES, BUBBLE_W - TEXT_INSET * 2);
    cachedIdx = idx;

    int visibleH = TEXT_BOTTOM - TEXT_TOP;
    int totalTextH = cachedNumLines * LINE_HEIGHT;
    scrollMaxOffset = totalTextH - visibleH;
    if (scrollMaxOffset < 0) scrollMaxOffset = 0;
    scrollOffset = 0;
    scrollActive = (scrollMaxOffset > 0);
    scrollPausedAtBottom = false;
    scrollStartTime = millis() + SCROLL_PAUSE_MS;
    lastScrollTime = 0;
  }

  drawNotificationFrame(idx, true, 0);
}

void updateTimestamp() {
  if (currentIdx < 0 || currentIdx >= notifCount) return;
  Notification& n = notifs[currentIdx];
  int iconCx = 28, iconR = 20;
  int textX = iconCx + iconR + 12;
  // Clear the timestamp area and redraw
  ui.tft.fillRect(textX, 32, 120, 18, TFT_BLACK);
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(COL_SUBTEXT, TFT_BLACK);
  char timeBuf[16];
  formatTimeAgo(n.arrivedAt, timeBuf, sizeof(timeBuf));
  ui.tft.setCursor(textX, 34);
  ui.tft.print(timeBuf);
}

void drawNotificationFrame(int idx, bool fullRedraw, int scrollPx) {
  if (idx < 0 || idx >= notifCount) {
    drawIdleScreen();
    return;
  }

  Notification& n = notifs[idx];

  // Determine source colors
  const char* src = n.source;
  enum SourceType { SRC_IMESSAGE, SRC_SLACK, SRC_DISCORD, SRC_EMAIL,
                    SRC_CALENDAR, SRC_CHROME, SRC_FACETIME, SRC_WALLET,
                    SRC_NOTES, SRC_OTHER };
  SourceType srcType = SRC_OTHER;
  if (strcmp(src, "imessage") == 0)     srcType = SRC_IMESSAGE;
  else if (strcmp(src, "slack") == 0)   srcType = SRC_SLACK;
  else if (strcmp(src, "discord") == 0) srcType = SRC_DISCORD;
  else if (strcmp(src, "email") == 0)   srcType = SRC_EMAIL;
  else if (strcmp(src, "calendar") == 0) srcType = SRC_CALENDAR;
  else if (strcmp(src, "chrome") == 0)  srcType = SRC_CHROME;
  else if (strcmp(src, "facetime") == 0) srcType = SRC_FACETIME;
  else if (strcmp(src, "wallet") == 0)  srcType = SRC_WALLET;
  else if (strcmp(src, "notes") == 0)   srcType = SRC_NOTES;

  uint16_t accentColor;
  switch (srcType) {
    case SRC_IMESSAGE: accentColor = COL_IMESSAGE; break;
    case SRC_SLACK:    accentColor = 0xFDA0; break;  // Yellow-orange
    case SRC_DISCORD:  accentColor = 0x5A9F; break;  // Blurple
    case SRC_EMAIL:    accentColor = 0x2C9F; break;  // Blue
    case SRC_CALENDAR: accentColor = 0xF800; break;  // Red
    case SRC_CHROME:   accentColor = 0x4E89; break;  // Google blue
    case SRC_FACETIME: accentColor = 0x3666; break;  // FaceTime green
    case SRC_WALLET:   accentColor = 0xFE20; break;  // Orange
    case SRC_NOTES:    accentColor = 0xFEA0; break;  // Yellow
    default:           accentColor = 0x7BCF; break;  // Gray
  }
  uint16_t bubbleBg = COL_BUBBLE;

  if (fullRedraw) {
    ui.tft.fillScreen(TFT_BLACK);

    // === Top section: source icon circle + source label ===
    int iconCx = 28;
    int iconCy = 28;
    int iconR = 20;

    ui.tft.fillCircle(iconCx, iconCy, iconR, accentColor);

    switch (srcType) {
      case SRC_IMESSAGE: drawSpeechBubbleIcon(iconCx, iconCy, TFT_WHITE); break;
      case SRC_SLACK:    drawSlackIcon(iconCx, iconCy, TFT_WHITE); break;
      case SRC_DISCORD:  drawDiscordIcon(iconCx, iconCy, TFT_WHITE); break;
      case SRC_EMAIL:    drawEnvelopeIcon(iconCx, iconCy, TFT_WHITE); break;
      case SRC_CALENDAR: drawCalendarIcon(iconCx, iconCy, TFT_WHITE); break;
      case SRC_CHROME:   drawGlobeIcon(iconCx, iconCy, TFT_WHITE); break;
      case SRC_FACETIME: drawPhoneIcon(iconCx, iconCy, TFT_WHITE); break;
      case SRC_WALLET:   drawWalletIcon(iconCx, iconCy, TFT_WHITE); break;
      case SRC_NOTES:    drawNoteIcon(iconCx, iconCy, TFT_WHITE); break;
      default:           drawBellIcon(iconCx, iconCy, TFT_WHITE); break;
    }

    // Source label
    const char* sourceLabel;
    switch (srcType) {
      case SRC_IMESSAGE: sourceLabel = "iMessage"; break;
      case SRC_SLACK:    sourceLabel = "Slack"; break;
      case SRC_DISCORD:  sourceLabel = "Discord"; break;
      case SRC_EMAIL:    sourceLabel = "Mail"; break;
      case SRC_CALENDAR: sourceLabel = "Calendar"; break;
      case SRC_CHROME:   sourceLabel = "Chrome"; break;
      case SRC_FACETIME: sourceLabel = "FaceTime"; break;
      case SRC_WALLET:   sourceLabel = "Wallet"; break;
      case SRC_NOTES:    sourceLabel = "Notes"; break;
      default:           sourceLabel = n.source; break;
    }

    int textX = iconCx + iconR + 12;
    ui.tft.setTTFFont(Arial_16_Bold);
    ui.tft.setTextColor(COL_SENDER, TFT_BLACK);
    ui.tft.setCursor(textX, 12);
    ui.tft.print(sourceLabel);

    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(COL_SUBTEXT, TFT_BLACK);
    char timeBuf[16];
    formatTimeAgo(n.arrivedAt, timeBuf, sizeof(timeBuf));
    ui.tft.setCursor(textX, 34);
    ui.tft.print(timeBuf);



    // === Draw bubble background ===
    ui.tft.fillRoundRect(BUBBLE_X, BUBBLE_Y, BUBBLE_W, BUBBLE_H, 12, bubbleBg);

    // Accent stripe
    ui.tft.fillRect(BUBBLE_X + 1, BUBBLE_Y + 12, 3, BUBBLE_H - 24, accentColor);
    ui.tft.fillRect(BUBBLE_X + 2, BUBBLE_Y + 8, 2, 4, accentColor);
    ui.tft.fillRect(BUBBLE_X + 2, BUBBLE_Y + BUBBLE_H - 12, 2, 4, accentColor);

    // Sender name
    ui.tft.setTTFFont(Arial_16_Bold);
    ui.tft.setTextColor(accentColor, bubbleBg);
    char senderBuf[64];
    strncpy(senderBuf, n.sender, sizeof(senderBuf) - 1);
    senderBuf[sizeof(senderBuf) - 1] = '\0';
    ui.truncateText(senderBuf, BUBBLE_W - TEXT_INSET * 2 - 8);
    ui.tft.setCursor(BUBBLE_X + TEXT_INSET + 4, BUBBLE_Y + 10);
    ui.tft.print(senderBuf);

    // === Dot indicators at bottom ===
    if (notifCount > 1) {
      int dotSpacing = 14;
      int dotsWidth = (notifCount - 1) * dotSpacing;
      int startX = (240 - dotsWidth) / 2;
      int dotY = 228;

      for (int i = 0; i < notifCount && i < MAX_NOTIFS; i++) {
        if (i == idx) {
          ui.tft.fillCircle(startX + i * dotSpacing, dotY, 4, accentColor);
        } else {
          ui.tft.fillCircle(startX + i * dotSpacing, dotY, 3, 0x31A6);
        }
      }
    }
  }

  // === Message text (scrollable area) ===
  // Flicker-free approach: do NOT clear the text area before drawing.
  // Instead, rely on TTF's background-color fill (setTextColor(fg, bg))
  // which writes each character cell in a single SPI transaction — bg pixels
  // and fg pixels together — so there's no intermediate blank flash.
  // We only use fillRect for the small gaps that text doesn't cover:
  //   - Top gap (where a line scrolled off)
  //   - Right margin after each line's text ends
  //   - Bottom gap below the last visible line
  int textAreaX = BUBBLE_X + 6;
  int textAreaW = BUBBLE_W - 8;
  int textAreaY = TEXT_TOP;
  int textAreaH = TEXT_BOTTOM - TEXT_TOP;
  int textX = BUBBLE_X + TEXT_INSET + 4;
  int textMaxW = BUBBLE_W - TEXT_INSET * 2 - 8;  // max text draw width

  ui.tft.setTTFFont(Arial_14);
  ui.tft.setTextColor(COL_TEXT, bubbleBg);

  int lastDrawnBottom = textAreaY;

  for (int i = 0; i < cachedNumLines; i++) {
    int yPos = textAreaY + (i * LINE_HEIGHT) - scrollPx;

    // Skip lines fully above or below the visible region
    if (yPos + LINE_HEIGHT <= textAreaY || yPos >= TEXT_BOTTOM) continue;

    // For lines partially off-screen (top or bottom), just track their
    // area for gap-filling but don't render text
    if (yPos < textAreaY || yPos + LINE_HEIGHT > TEXT_BOTTOM) {
      int drawY = (yPos < textAreaY) ? textAreaY : yPos;
      int drawEnd = (yPos + LINE_HEIGHT > TEXT_BOTTOM) ? TEXT_BOTTOM : (yPos + LINE_HEIGHT);
      // Clear this partial area
      if (drawY > lastDrawnBottom) {
        ui.tft.fillRect(textAreaX, lastDrawnBottom, textAreaW, drawY - lastDrawnBottom, bubbleBg);
      }
      ui.tft.fillRect(textAreaX, drawY, textAreaW, drawEnd - drawY, bubbleBg);
      lastDrawnBottom = drawEnd;
      continue;
    }

    // Clear any gap between last drawn content and this line's top
    if (yPos > lastDrawnBottom) {
      ui.tft.fillRect(textAreaX, lastDrawnBottom, textAreaW, yPos - lastDrawnBottom, bubbleBg);
    }

    // Draw text — TTF bg fill overwrites old content with no flash
    ui.tft.setCursor(textX, yPos);
    ui.tft.print(cachedLines[i]);

    // Clear the right margin after the text ends (small strip, low flicker)
    int lineTextW = ui.tft.TTFtextWidth(cachedLines[i]);
    int textEndX = textX + lineTextW;
    int rightMargin = (textAreaX + textAreaW) - textEndX;
    if (rightMargin > 0) {
      ui.tft.fillRect(textEndX, yPos, rightMargin, LINE_HEIGHT, bubbleBg);
    }

    lastDrawnBottom = yPos + LINE_HEIGHT;
  }

  // Clear any remaining space below the last visible line
  if (lastDrawnBottom < TEXT_BOTTOM) {
    ui.tft.fillRect(textAreaX, lastDrawnBottom, textAreaW, TEXT_BOTTOM - lastDrawnBottom, bubbleBg);
  }

  // Re-draw accent stripe over the text area (thin, fast)
  ui.tft.fillRect(BUBBLE_X + 1, textAreaY, 3, textAreaH, accentColor);
}

// ---------------------------------------------------------------------------
// POST /notify handler
// ---------------------------------------------------------------------------

void handleNotify() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no body\"}");
    return;
  }

  String body = server.arg("plain");
  Serial.printf("Notify: %s\n", body.c_str());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }

  // Shift notifications down (newest at index 0)
  if (notifCount < MAX_NOTIFS) {
    notifCount++;
  }
  for (int i = notifCount - 1; i > 0; i--) {
    notifs[i] = notifs[i - 1];
  }

  // Store new notification at index 0
  Notification& n = notifs[0];
  memset(&n, 0, sizeof(n));
  strlcpy(n.source, doc["source"] | "unknown", sizeof(n.source));
  strlcpy(n.sender, doc["sender"] | "Unknown", sizeof(n.sender));
  const char* srcText = doc["text"] | "";
  strlcpy(n.text, srcText, sizeof(n.text));
  // Add ellipsis if truncated
  if (strlen(srcText) >= sizeof(n.text)) {
    n.text[sizeof(n.text) - 4] = '\0';
    strlcat(n.text, "...", sizeof(n.text));
  }
  n.arrivedAt = millis();

  // Auto-show the newest notification
  currentIdx = 0;
  cachedIdx = -1;  // invalidate cache
  needsRedraw = true;


  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Button handling
// ---------------------------------------------------------------------------

void handleButton() {
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);  // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (notifCount > 1) {
        currentIdx = (currentIdx + 1) % notifCount;
        needsRedraw = true;
      }
    }
  }
  buttonWasPressed = pressed;
}

// ---------------------------------------------------------------------------
// Setup & Loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(460800);
  Serial.println("Starting notice app");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run(); // DO NOT EDIT

  // Register our notification endpoint on the shared web server
  server.on("/notify", HTTP_POST, handleNotify);

  ui.init();
  ui.clear();

  // Load server URL from preferences
  preferences.begin("app", true);
  serverUrl = preferences.getString("server_url");
  preferences.end();

  Serial.printf("Device IP: %s\n", WiFi.localIP().toString().c_str());

  if (serverUrl.length() == 0) {
    Serial.println("WARNING: server_url not set, device registration disabled. Set via ./tools/dev init");
  } else {
    Serial.printf("Server URL: %s\n", serverUrl.c_str());
    registerWithServer();
  }

  // Show idle screen
  drawIdleScreen();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT

    if (needsRedraw) {
      if (notifCount > 0) {
        drawNotification(currentIdx);
      } else {
        drawIdleScreen();
        scrollActive = false;
      }
      needsRedraw = false;
    }

    // Auto-scroll long messages
    if (scrollActive && cachedIdx == currentIdx && millis() >= scrollStartTime) {
      if (scrollPausedAtBottom) {
        // Waiting at bottom — check if pause is over
        if (millis() >= scrollBottomPauseEnd) {
          scrollPausedAtBottom = false;
          scrollOffset = 0;
          scrollStartTime = millis() + SCROLL_PAUSE_MS;
          // Clear just the text area to remove ghost pixels, then redraw
          int textAreaX = BUBBLE_X + 6;
          int textAreaW = BUBBLE_W - 8;
          ui.tft.fillRect(textAreaX, TEXT_TOP, textAreaW, TEXT_BOTTOM - TEXT_TOP, COL_BUBBLE);
          drawNotificationFrame(currentIdx, false, 0);
        }
      } else if (millis() - lastScrollTime >= SCROLL_SPEED_MS) {
        lastScrollTime = millis();
        scrollOffset++;
        if (scrollOffset > scrollMaxOffset) {
          // Reached the end — pause at bottom before resetting
          scrollOffset = scrollMaxOffset;
          scrollPausedAtBottom = true;
          scrollBottomPauseEnd = millis() + SCROLL_BOTTOM_PAUSE_MS;
        } else {
          drawNotificationFrame(currentIdx, false, scrollOffset);
        }
      }
    }

    handleButton();

    // Update relative timestamp and expire old notifications
    if (notifCount > 0 && millis() - lastTimeUpdate > TIME_UPDATE_INTERVAL) {
      // Discard notifications older than 30 minutes (from the end of the array)
      unsigned long now = millis();
      int prevCount = notifCount;
      while (notifCount > 0 && (now - notifs[notifCount - 1].arrivedAt) > 1800000UL) {
        notifCount--;
      }
      if (notifCount < prevCount) {
        Serial.printf("Expired %d notification(s), %d remaining\n", prevCount - notifCount, notifCount);
      }
      if (notifCount == 0) {
        Serial.println("All notifications expired, showing idle screen");
        currentIdx = 0;
        cachedIdx = -1;
        scrollActive = false;
        needsRedraw = true;
      } else {
        if (currentIdx >= notifCount) {
          currentIdx = notifCount - 1;
          needsRedraw = true;
        }
        updateTimestamp();
      }
      lastTimeUpdate = now;
    }

    // Re-register periodically (30s retry on failure, 5min keepalive on success)
    unsigned long regInterval = registered ? REGISTER_INTERVAL : REGISTER_RETRY_INTERVAL;
    if (millis() - lastRegister > regInterval) {
      registerWithServer();
    }
  }

  delay(10);
}
