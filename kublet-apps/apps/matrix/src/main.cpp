#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

#define BUTTON_PIN 19

// GLCD font: 6px wide, 8px tall
#define CHAR_W 6
#define CHAR_H 8
#define COLS (240 / CHAR_W) // 40
#define ROWS (240 / CHAR_H) // 30

OTAServer otaserver;
KGFX ui;
bool buttonWasPressed = false;

// Color theme: head, bright trail, mid trail, dim trail
struct ColorTheme {
  uint16_t head;
  uint16_t bright;
  uint16_t mid;
  uint16_t dim;
};

static const ColorTheme themes[] = {
    {TFT_WHITE, 0x07E0, 0x03E0, 0x01A0}, // Green (classic)
    {TFT_WHITE, 0x07FF, 0x03EF, 0x01A8}, // Cyan
    {TFT_WHITE, 0x001F, 0x0012, 0x0009}, // Blue
    {TFT_WHITE, 0xF800, 0x9800, 0x4800}, // Red
    {TFT_WHITE, 0xF81F, 0x9012, 0x4809}, // Purple
};
#define NUM_THEMES (sizeof(themes) / sizeof(themes[0]))

uint8_t currentTheme = 0;

// Per-column rain state
struct Column {
  int16_t headY;     // current head row (can be negative = above screen)
  uint8_t speed;     // frames between advances
  uint8_t trailLen;  // trail length in rows
  uint8_t counter;   // frame counter
  char chars[ROWS];  // character at each row
};

Column columns[COLS];

char randomChar() {
  return (char)random(33, 127);
}

void initColumn(int col) {
  columns[col].headY = random(-ROWS, 0);
  columns[col].speed = random(1, 4);
  columns[col].trailLen = random(6, ROWS - 2);
  columns[col].counter = 0;
  for (int r = 0; r < ROWS; r++) {
    columns[col].chars[r] = randomChar();
  }
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting matrix app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  randomSeed(esp_random());

  ui.tft.fillScreen(TFT_BLACK);
  ui.tft.setTextFont(1);

  for (int i = 0; i < COLS; i++) {
    initColumn(i);
  }
}

inline void drawCharAt(int col, int row, char c, uint16_t color) {
  ui.tft.drawChar(col * CHAR_W, row * CHAR_H, c, color, TFT_BLACK, 1);
}

inline void eraseCell(int col, int row) {
  ui.tft.fillRect(col * CHAR_W, row * CHAR_H, CHAR_W, CHAR_H, TFT_BLACK);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT

    // Button press — cycle color theme
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);
    if (pressed && !buttonWasPressed) {
      delay(50);
      if (digitalRead(BUTTON_PIN) == LOW) {
        currentTheme = (currentTheme + 1) % NUM_THEMES;
        Serial.printf("Theme: %d\n", currentTheme);
      }
    }
    buttonWasPressed = pressed;

    const ColorTheme &theme = themes[currentTheme];

    for (int col = 0; col < COLS; col++) {
      Column &c = columns[col];
      c.counter++;
      if (c.counter < c.speed)
        continue;
      c.counter = 0;

      // Erase the tail end
      int tailRow = c.headY - c.trailLen;
      if (tailRow >= 0 && tailRow < ROWS) {
        eraseCell(col, tailRow);
      }

      // Dim the previous head to bright green
      int prevHead = c.headY;
      if (prevHead >= 0 && prevHead < ROWS) {
        drawCharAt(col, prevHead, c.chars[prevHead], theme.bright);
      }

      // Dim the mid-trail character
      int midRow = c.headY - 2;
      if (midRow >= 0 && midRow < ROWS) {
        drawCharAt(col, midRow, c.chars[midRow], theme.mid);
      }

      // Dim the far-trail character
      int dimRow = c.headY - (c.trailLen / 2);
      if (dimRow >= 0 && dimRow < ROWS) {
        drawCharAt(col, dimRow, c.chars[dimRow], theme.dim);
      }

      // Flicker: occasionally change a random trail character
      if (random(100) < 20) {
        int flickerRow = c.headY - random(1, c.trailLen);
        if (flickerRow >= 0 && flickerRow < ROWS) {
          c.chars[flickerRow] = randomChar();
          // Determine color based on distance from head
          int dist = c.headY - flickerRow;
          uint16_t color = theme.dim;
          if (dist <= 1)
            color = theme.bright;
          else if (dist <= 3)
            color = theme.mid;
          drawCharAt(col, flickerRow, c.chars[flickerRow], color);
        }
      }

      // Advance head
      c.headY++;

      // Draw new head character (bright white)
      if (c.headY >= 0 && c.headY < ROWS) {
        c.chars[c.headY] = randomChar();
        drawCharAt(col, c.headY, c.chars[c.headY], theme.head);
      }

      // Reset column when trail is fully off screen
      if (tailRow >= ROWS) {
        initColumn(col);
      }
    }

    delay(30);
  }

  delay(1);
}
