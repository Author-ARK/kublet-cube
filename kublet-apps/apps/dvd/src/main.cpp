#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;
bool buttonWasPressed = false;

// Screen
#define SCR_W 240
#define SCR_H 240

// Logo dimensions — square block like the classic DVD screensaver
#define LOGO_W 60
#define LOGO_H 60

// Sprite is exactly logo-sized — pushed atomically each frame
TFT_eSprite spr = TFT_eSprite(&ui.t);

// Position and velocity (fixed point: x256 for sub-pixel smoothness)
int32_t posX, posY;
int32_t velX, velY;

// Previous integer position for dirty-rect tracking
int prevX, prevY;

// Color palette
const uint16_t colors[] = {
  0xF800, // red
  0x07E0, // green
  0x001F, // blue
  0xFFE0, // yellow
  0xF81F, // magenta
  0x07FF, // cyan
  0xFD20, // orange
  0x781F, // purple
  0x7FE0, // lime
  0xFBE0, // gold
};
#define NUM_COLORS 10
int colorIdx = 0;
uint16_t logoColor;

// Corner hit counter
int cornerHits = 0;

// Corner flash effect
unsigned long cornerFlashTime = 0;
bool cornerFlash = false;

// Temp sprite for rendering text before shearing
TFT_eSprite tmpSpr = TFT_eSprite(&ui.t);

// --- DVD Logo Drawing (into sprite using TTF fonts) ---

void drawLogoToSprite(uint16_t color) {
  // Filled rectangle with slight corner rounding
  spr.fillRect(2, 0, LOGO_W - 4, LOGO_H, color);
  spr.fillRect(0, 2, LOGO_W, LOGO_H - 4, color);
  spr.fillRect(1, 1, LOGO_W - 2, LOGO_H - 2, color);

  // Render "DVD" into temp sprite, then shear-copy into main sprite
  #define DVD_TMP_W 48
  #define DVD_TMP_H 18
  #define SHEAR 3  // total horizontal shear in pixels across the height

  tmpSpr.fillSprite(color);

  ui.tft.TTFdestination(&tmpSpr);
  ui.tft.setTextColor(TFT_BLACK, color);
  ui.tft.setTTFFont(Arial_14_Bold);
  int dvdW = ui.tft.TTFtextWidth("DVD");
  int dvdCx = (DVD_TMP_W - dvdW) / 2 + SHEAR / 2;
  // Triple-strike: render 3 times at -1, 0, +1 for centered boldness
  ui.tft.setCursor(dvdCx - 1, 0);
  ui.tft.print("DVD");
  ui.tft.setCursor(dvdCx, 0);
  ui.tft.print("DVD");
  ui.tft.setCursor(dvdCx + 1, 0);
  ui.tft.print("DVD");

  // Copy row-by-row with horizontal shear (top rows shift right, bottom rows shift left)
  int dvdX = (LOGO_W - DVD_TMP_W) / 2;
  int dvdY = 13;
  for (int row = 0; row < DVD_TMP_H; row++) {
    int shift = SHEAR - (SHEAR * 2 * row) / DVD_TMP_H;
    for (int col = 0; col < DVD_TMP_W; col++) {
      uint16_t px = tmpSpr.readPixel(col, row);
      int destCol = dvdX + col + shift;
      if (destCol >= 0 && destCol < LOGO_W) {
        spr.drawPixel(destCol, dvdY + row, px);
      }
    }
  }

  // "VIDEO" in white (no shear)
  ui.tft.TTFdestination(&spr);
  ui.tft.setTextColor(TFT_WHITE, color);
  ui.tft.setTTFFont(Arial_10_Bold);
  int videoW = ui.tft.TTFtextWidth("VIDEO");
  ui.tft.setCursor((LOGO_W - videoW) / 2, 33);
  ui.tft.print("VIDEO");

  // Restore TTF rendering to screen
  ui.tft.TTFdestination(&ui.t);
}

// --- Init ---

void resetPosition() {
  posX = random(0, (SCR_W - LOGO_W)) * 256;
  posY = random(0, (SCR_H - LOGO_H)) * 256;

  // Velocity: ~1.5 pixels per frame
  velX = random(0, 2) ? 384 : -384;
  velY = random(0, 2) ? 384 : -384;

  colorIdx = random(NUM_COLORS);
  logoColor = colors[colorIdx];

  prevX = posX / 256;
  prevY = posY / 256;
}

// Erase old logo by blacking out only the thin edge strips the logo moved away from
void eraseEdges(int oldX, int oldY, int newX, int newY) {
  int deltaX = newX - oldX;
  int deltaY = newY - oldY;

  if (deltaX > 0)
    ui.tft.fillRect(oldX, oldY, deltaX, LOGO_H, TFT_BLACK);
  else if (deltaX < 0)
    ui.tft.fillRect(newX + LOGO_W, oldY, -deltaX, LOGO_H, TFT_BLACK);

  if (deltaY > 0)
    ui.tft.fillRect(oldX, oldY, LOGO_W, deltaY, TFT_BLACK);
  else if (deltaY < 0)
    ui.tft.fillRect(oldX, newY + LOGO_H, LOGO_W, -deltaY, TFT_BLACK);
}

// Render logo — single atomic sprite push, no flicker
void renderFrame(int logoX, int logoY) {
  spr.fillSprite(TFT_BLACK);

  uint16_t drawColor = logoColor;
  if (cornerFlash && (millis() - cornerFlashTime < 150)) {
    drawColor = TFT_WHITE;
  }

  drawLogoToSprite(drawColor);
  spr.pushSprite(logoX, logoY);
}

void drawCornerCount() {
  if (cornerHits > 0) {
    ui.tft.fillRect(0, 222, 50, 18, TFT_BLACK);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", cornerHits);
    ui.tft.setTTFFont(Arial_14_Bold);
    ui.tft.setTextColor(0x4208, TFT_BLACK);
    ui.tft.setCursor(4, 222);
    ui.tft.print(buf);
  }
}

// --- Arduino ---

void setup() {
  Serial.begin(460800);
  Serial.println("Starting dvd app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ui.tft.fillScreen(TFT_BLACK);

  // Pre-allocate sprites — never reallocated
  spr.setColorDepth(16);
  spr.createSprite(LOGO_W, LOGO_H);
  tmpSpr.setColorDepth(16);
  tmpSpr.createSprite(DVD_TMP_W, DVD_TMP_H);

  randomSeed(esp_random());
  resetPosition();

  renderFrame(prevX, prevY);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  // Button: cycle color
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      colorIdx = (colorIdx + 1) % NUM_COLORS;
      logoColor = colors[colorIdx];
      Serial.printf("Button: color %d\n", colorIdx);
    }
  }
  buttonWasPressed = pressed;

  // Update position
  posX += velX;
  posY += velY;

  // Bounce detection
  bool hitX = false, hitY = false;

  if (posX < 0) {
    posX = 0;
    velX = -velX;
    hitX = true;
  } else if (posX > (SCR_W - LOGO_W) * 256) {
    posX = (SCR_W - LOGO_W) * 256;
    velX = -velX;
    hitX = true;
  }

  if (posY < 0) {
    posY = 0;
    velY = -velY;
    hitY = true;
  } else if (posY > (SCR_H - LOGO_H) * 256) {
    posY = (SCR_H - LOGO_H) * 256;
    velY = -velY;
    hitY = true;
  }

  // Change color on any wall hit
  if (hitX || hitY) {
    colorIdx = (colorIdx + 1) % NUM_COLORS;
    logoColor = colors[colorIdx];
  }

  // Corner hit!
  if (hitX && hitY) {
    cornerHits++;
    cornerFlash = true;
    cornerFlashTime = millis();
    Serial.printf("CORNER HIT! Total: %d\n", cornerHits);
    drawCornerCount();
  }

  int curX = posX / 256;
  int curY = posY / 256;

  // Only redraw when position changes or flash ending
  if (curX != prevX || curY != prevY) {
    eraseEdges(prevX, prevY, curX, curY);
    renderFrame(curX, curY);
    prevX = curX;
    prevY = curY;
  } else if (cornerFlash && (millis() - cornerFlashTime >= 150)) {
    cornerFlash = false;
    renderFrame(curX, curY);
  }

  delay(16); // ~60fps
}
