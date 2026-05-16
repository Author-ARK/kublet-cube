#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;
bool buttonWasPressed = false;

// Grid config: 48x48 cells at 5px each = 240x240
#define GRID_W 48
#define GRID_H 48
#define CELL   5

// Double-buffered grid: 0=dead, 1=alive
uint8_t grid[2][GRID_H][GRID_W];
int cur = 0; // current buffer index

// Generation counter and stagnation detection
unsigned long generation = 0;
int prevPopulation = 0;
int staleCount = 0;

// Colors
#define CLR_BG    TFT_BLACK
#define CLR_ALIVE 0x07E0  // bright green
#define CLR_DIM   0x0320  // darker green for recently died (fade effect)

// Track previous display state to only redraw changed cells
uint8_t displayState[GRID_H][GRID_W]; // 0=off, 1=alive, 2=dim

// Tick timing
#define TICK_MS 100

unsigned long lastTick = 0;

// --- Drawing ---

void drawCellAt(int x, int y, uint16_t color) {
  ui.tft.fillRect(x * CELL, y * CELL, CELL, CELL, color);
}

void drawGeneration() {
  // Show generation count in top-left, subtle
  char buf[16];
  snprintf(buf, sizeof(buf), "G%lu", generation);
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(0x4208, CLR_BG); // dim gray text
  ui.tft.setCursor(2, 2);
  ui.tft.print(buf);
}

// --- Grid logic ---

void randomizeGrid() {
  memset(grid, 0, sizeof(grid));
  memset(displayState, 0, sizeof(displayState));
  cur = 0;
  generation = 0;
  prevPopulation = 0;
  staleCount = 0;

  // ~30% fill density for interesting patterns
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      grid[cur][y][x] = (random(100) < 30) ? 1 : 0;
    }
  }

  // Full redraw
  ui.tft.fillScreen(CLR_BG);
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      if (grid[cur][y][x]) {
        drawCellAt(x, y, CLR_ALIVE);
        displayState[y][x] = 1;
      }
    }
  }
}

int countNeighbors(int x, int y) {
  int count = 0;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) continue;
      // Wrap around edges (toroidal grid)
      int nx = (x + dx + GRID_W) % GRID_W;
      int ny = (y + dy + GRID_H) % GRID_H;
      count += grid[cur][ny][nx];
    }
  }
  return count;
}

void stepGeneration() {
  int nxt = 1 - cur;
  int population = 0;

  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      int n = countNeighbors(x, y);
      bool alive = grid[cur][y][x];

      if (alive) {
        grid[nxt][y][x] = (n == 2 || n == 3) ? 1 : 0;
      } else {
        grid[nxt][y][x] = (n == 3) ? 1 : 0;
      }

      population += grid[nxt][y][x];

      // Update display only for changed cells
      uint8_t newState;
      if (grid[nxt][y][x]) {
        newState = 1;
      } else if (grid[cur][y][x] && !grid[nxt][y][x]) {
        newState = 2; // just died — show dim
      } else if (displayState[y][x] == 2) {
        newState = 0; // was dim, now fade to black
      } else {
        newState = displayState[y][x]; // no change needed
      }

      if (newState != displayState[y][x]) {
        uint16_t color;
        switch (newState) {
          case 1:  color = CLR_ALIVE; break;
          case 2:  color = CLR_DIM;   break;
          default: color = CLR_BG;    break;
        }
        drawCellAt(x, y, color);
        displayState[y][x] = newState;
      }
    }
  }

  cur = nxt;
  generation++;

  // Stagnation detection: if population unchanged for many gens, re-seed
  if (population == prevPopulation) {
    staleCount++;
  } else {
    staleCount = 0;
  }
  prevPopulation = population;

  // Auto-randomize if stale for 50 gens or everything died
  if (staleCount > 50 || population == 0) {
    Serial.printf("Stale after %lu generations (pop=%d), re-seeding\n", generation, population);
    randomizeGrid();
  }
}

// --- Arduino ---

void setup() {
  Serial.begin(460800);
  Serial.println("Starting life app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  randomSeed(esp_random());
  randomizeGrid();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  // Button: randomize board
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Button: randomize");
      randomizeGrid();
    }
  }
  buttonWasPressed = pressed;

  unsigned long now = millis();
  if (now - lastTick >= TICK_MS) {
    lastTick = now;
    stepGeneration();
  }
}
