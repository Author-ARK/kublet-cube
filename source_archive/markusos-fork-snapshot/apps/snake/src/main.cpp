#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;
bool buttonWasPressed = false;

// Grid config: 22x22 playfield, 10px per cell, 20px score bar at top
#define GRID_W 22
#define GRID_H 22
#define CELL   10
#define SCORE_H 20
#define OX     ((240 - GRID_W * CELL) / 2)
#define OY     SCORE_H

// Directions: right, down, left, up
const int8_t dx[] = {1, 0, -1, 0};
const int8_t dy[] = {0, 1, 0, -1};

// Colors
#define CLR_BG      TFT_BLACK
#define CLR_HEAD    0x07E0   // bright green
#define CLR_BODY    0x03A0   // darker green
#define CLR_FOOD    TFT_RED
#define CLR_BORDER  0x2104   // subtle dark gray

// Game state
enum State { PLAYING, GAME_OVER };
State gameState = PLAYING;

// Snake as circular buffer
#define MAX_LEN (GRID_W * GRID_H)
struct Pos { int8_t x, y; };
Pos snake[MAX_LEN];
int snakeHead = 0;
int snakeLen = 0;

int dir = 0;       // current direction index
Pos food;
int score = 0;
int highScore = 0;
unsigned long lastTick = 0;
unsigned long gameOverTime = 0;

// Occupancy grid for fast collision checks
bool occupied[GRID_H][GRID_W];

// AI scratch buffers (reused each tick to avoid alloc)
int8_t bfsParent[GRID_H][GRID_W];   // -1=unvisited, -2=start, 0-3=came from dir
Pos bfsQueue[MAX_LEN];
bool floodVisited[GRID_H][GRID_W];

// --- Drawing ---

void drawCell(int gx, int gy, uint16_t color) {
  ui.tft.fillRect(OX + gx * CELL, OY + gy * CELL, CELL, CELL, color);
}

void drawCellInner(int gx, int gy, uint16_t color) {
  // Draw with 1px padding for a segmented look
  ui.tft.fillRect(OX + gx * CELL + 1, OY + gy * CELL + 1, CELL - 1, CELL - 1, color);
}

void drawScore() {
  // Clear score area
  ui.tft.fillRect(0, 0, 240, SCORE_H, CLR_BG);
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, CLR_BG);

  char buf[32];
  snprintf(buf, sizeof(buf), "Score: %d", score);
  ui.tft.setCursor(4, 2);
  ui.tft.print(buf);

  if (highScore > 0) {
    snprintf(buf, sizeof(buf), "Hi: %d", highScore);
    int w = ui.tft.TTFtextWidth(buf);
    ui.tft.setCursor(240 - w - 4, 2);
    ui.tft.print(buf);
  }
}

void drawBorder() {
  // Draw border around playfield
  int x0 = OX - 1;
  int y0 = OY - 1;
  int w = GRID_W * CELL + 2;
  int h = GRID_H * CELL + 2;
  ui.tft.drawRect(x0, y0, w, h, CLR_BORDER);
}

// --- Snake helpers ---

Pos snakeAt(int idx) {
  // idx 0 = tail, idx snakeLen-1 = head
  return snake[(snakeHead - snakeLen + 1 + idx + MAX_LEN) % MAX_LEN];
}

Pos head() {
  return snake[snakeHead];
}

bool isOccupied(int x, int y) {
  if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) return true;
  return occupied[y][x];
}

void rebuildOccupied() {
  memset(occupied, 0, sizeof(occupied));
  for (int i = 0; i < snakeLen; i++) {
    Pos p = snakeAt(i);
    occupied[p.y][p.x] = true;
  }
}

// --- Food ---

void spawnFood() {
  // Find random empty cell
  int empty = GRID_W * GRID_H - snakeLen;
  if (empty <= 0) return; // full board, you win

  int pick = random(empty);
  int count = 0;
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      if (!occupied[y][x]) {
        if (count == pick) {
          food = {(int8_t)x, (int8_t)y};
          drawCellInner(x, y, CLR_FOOD);
          return;
        }
        count++;
      }
    }
  }
}

// --- AI ---

// Flood fill from (sx,sy) counting reachable empty cells.
// Uses the current `occupied` grid but can temporarily patch cells.
int floodFillCount(int sx, int sy) {
  if (sx < 0 || sx >= GRID_W || sy < 0 || sy >= GRID_H) return 0;
  if (occupied[sy][sx]) return 0;

  memset(floodVisited, 0, sizeof(floodVisited));
  int qHead = 0, qTail = 0;
  bfsQueue[qTail++] = {(int8_t)sx, (int8_t)sy};
  floodVisited[sy][sx] = true;
  int count = 0;

  while (qHead < qTail) {
    Pos cur = bfsQueue[qHead++];
    count++;
    for (int d = 0; d < 4; d++) {
      int nx = cur.x + dx[d], ny = cur.y + dy[d];
      if (nx >= 0 && nx < GRID_W && ny >= 0 && ny < GRID_H &&
          !occupied[ny][nx] && !floodVisited[ny][nx]) {
        floodVisited[ny][nx] = true;
        bfsQueue[qTail++] = {(int8_t)nx, (int8_t)ny};
      }
    }
  }
  return count;
}

// A* from head to target, returns first-step direction or -1 if unreachable.
// Uses Manhattan distance heuristic for optimal pathfinding.
int16_t gScore[GRID_H][GRID_W];

int astarPathTo(int tx, int ty) {
  Pos h = head();
  if (h.x == tx && h.y == ty) return dir; // already there

  memset(bfsParent, -1, sizeof(bfsParent));
  memset(gScore, 0x7F, sizeof(gScore)); // init to large value

  // Open list as simple array (grid is small enough)
  struct AStarEntry { int8_t x, y; int16_t f; };
  AStarEntry openList[MAX_LEN];
  int openCount = 0;

  gScore[h.y][h.x] = 0;
  bfsParent[h.y][h.x] = -2; // start marker
  int16_t heur = abs(h.x - tx) + abs(h.y - ty);
  openList[openCount++] = {h.x, h.y, heur};

  while (openCount > 0) {
    // Find entry with lowest f score
    int bestIdx = 0;
    for (int i = 1; i < openCount; i++) {
      if (openList[i].f < openList[bestIdx].f) bestIdx = i;
    }
    AStarEntry cur = openList[bestIdx];
    openList[bestIdx] = openList[--openCount];

    // Skip stale entries
    int16_t curG = gScore[cur.y][cur.x];
    if (cur.f > curG + abs(cur.x - tx) + abs(cur.y - ty)) continue;

    if (cur.x == tx && cur.y == ty) {
      // Trace back to find first step from head
      int cx = tx, cy = ty;
      while (true) {
        int pd = bfsParent[cy][cx];
        int px = cx - dx[pd], py = cy - dy[pd];
        if (px == h.x && py == h.y) return pd;
        cx = px;
        cy = py;
      }
    }

    int16_t newG = curG + 1;

    for (int d = 0; d < 4; d++) {
      int nx = cur.x + dx[d], ny = cur.y + dy[d];
      if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) continue;
      if (occupied[ny][nx] && !(nx == tx && ny == ty)) continue;
      if (newG >= gScore[ny][nx]) continue;

      gScore[ny][nx] = newG;
      bfsParent[ny][nx] = d;
      int16_t f = newG + abs(nx - tx) + abs(ny - ty);
      openList[openCount++] = {(int8_t)nx, (int8_t)ny, f};
    }
  }
  return -1; // unreachable
}

// Check if moving in direction d is safe: after the move, the snake
// must still have access to enough space (flood fill >= snake length).
bool isSafeMove(int d) {
  Pos h = head();
  int nx = h.x + dx[d], ny = h.y + dy[d];
  if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) return false;
  if (occupied[ny][nx]) return false;

  // Simulate the move: remove tail (unless eating food).
  // Don't mark new head as occupied — flood fill starts there and
  // needs it to be empty as the entry point.
  bool eating = (nx == food.x && ny == food.y);

  Pos tail = snakeAt(0);
  if (!eating) occupied[tail.y][tail.x] = false;

  int reachable = floodFillCount(nx, ny);
  int needed = eating ? snakeLen : snakeLen - 1;

  // Undo simulation
  if (!eating) occupied[tail.y][tail.x] = true;

  return reachable >= needed;
}

int aiChooseDirection() {
  Pos h = head();

  // Strategy 1: A* shortest path to food
  int foodDir = astarPathTo(food.x, food.y);

  // If we found a path to food and the move is safe, take it
  if (foodDir >= 0 && !(snakeLen > 1 && foodDir == (dir + 2) % 4) && isSafeMove(foodDir)) {
    return foodDir;
  }

  // Strategy 2: Chase our own tail to buy time
  // The tail is always about to move, so it's a safe target
  Pos tail = snakeAt(0);
  int tailDir = astarPathTo(tail.x, tail.y);
  if (tailDir >= 0 && !(snakeLen > 1 && tailDir == (dir + 2) % 4) && isSafeMove(tailDir)) {
    return tailDir;
  }

  // Strategy 3: Fallback — pick the safe direction with the most reachable space
  int best = -1;
  int bestSpace = -1;

  for (int d = 0; d < 4; d++) {
    if (snakeLen > 1 && d == (dir + 2) % 4) continue;

    int nx = h.x + dx[d], ny = h.y + dy[d];
    if (isOccupied(nx, ny)) continue;

    // Simulate tail moving (don't mark head — flood fill starts there)
    Pos tail = snakeAt(0);
    occupied[tail.y][tail.x] = false;

    int space = floodFillCount(nx, ny);

    occupied[tail.y][tail.x] = true;

    // Among safe moves, pick the one with the most space.
    // Accept even "unsafe" moves as last resort.
    if (space > bestSpace) {
      bestSpace = space;
      best = d;
    }
  }

  return best; // -1 only if truly no move at all
}

// --- Game logic ---

void resetGame() {
  ui.tft.fillScreen(CLR_BG);

  memset(occupied, 0, sizeof(occupied));
  snakeLen = 3;
  snakeHead = 2;
  dir = 0; // start moving right

  // Place snake in center
  int startX = GRID_W / 2 - 1;
  int startY = GRID_H / 2;
  for (int i = 0; i < 3; i++) {
    snake[i] = {(int8_t)(startX - 2 + i), (int8_t)startY};
    occupied[startY][startX - 2 + i] = true;
  }

  score = 0;
  gameState = PLAYING;

  drawBorder();
  drawScore();

  // Draw initial snake
  for (int i = 0; i < snakeLen; i++) {
    Pos p = snakeAt(i);
    uint16_t color = (i == snakeLen - 1) ? CLR_HEAD : CLR_BODY;
    drawCellInner(p.x, p.y, color);
  }

  randomSeed(esp_random());
  spawnFood();
  lastTick = millis();
}

void gameStep() {
  int newDir = aiChooseDirection();

  if (newDir < 0) {
    // No safe move — game over
    gameState = GAME_OVER;
    if (score > highScore) highScore = score;
    gameOverTime = millis();

    // Flash head red
    Pos h = head();
    drawCellInner(h.x, h.y, TFT_RED);

    // Show game over text
    ui.tft.setTTFFont(Arial_14_Bold);
    ui.tft.setTextColor(TFT_WHITE, CLR_BG);
    const char* msg = "GAME OVER";
    int w = ui.tft.TTFtextWidth(msg);
    ui.tft.setCursor((240 - w) / 2, 110);
    ui.tft.print(msg);

    drawScore();
    return;
  }

  dir = newDir;

  Pos h = head();
  int nx = h.x + dx[dir];
  int ny = h.y + dy[dir];

  // Recolor old head as body
  drawCellInner(h.x, h.y, CLR_BODY);

  bool ate = (nx == food.x && ny == food.y);

  if (!ate) {
    // Remove tail
    Pos tail = snakeAt(0);
    drawCell(tail.x, tail.y, CLR_BG);
    occupied[tail.y][tail.x] = false;
  } else {
    snakeLen++;
    score++;
    drawScore();
  }

  // Add new head
  snakeHead = (snakeHead + 1) % MAX_LEN;
  snake[snakeHead] = {(int8_t)nx, (int8_t)ny};
  occupied[ny][nx] = true;

  drawCellInner(nx, ny, CLR_HEAD);

  if (ate) {
    spawnFood();
  }
}

int tickInterval() {
  // Speed up as score increases: 150ms down to 80ms
  int ms = 150 - score * 2;
  if (ms < 80) ms = 80;
  return ms;
}

// --- Arduino ---

void setup() {
  Serial.begin(460800);
  Serial.println("Starting snake app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  randomSeed(esp_random());
  resetGame();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  // Button: reset game
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Button: reset");
      resetGame();
    }
  }
  buttonWasPressed = pressed;

  unsigned long now = millis();

  switch (gameState) {
    case PLAYING:
      if (now - lastTick >= (unsigned long)tickInterval()) {
        lastTick = now;
        gameStep();
      }
      break;

    case GAME_OVER:
      // Auto-restart after 3 seconds
      if (now - gameOverTime > 3000) {
        resetGame();
      }
      break;
  }
}
