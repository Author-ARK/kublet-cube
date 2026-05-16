#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;
bool buttonWasPressed = false;

// Maze config: 23x23 cells, rendered at 5px per grid unit
#define MW 23
#define MH 23
#define GW (2 * MW + 1)  // 47
#define GH (2 * MH + 1)  // 47
#define CPX 5
#define OX ((240 - GW * CPX) / 2)
#define OY ((240 - GH * CPX) / 2)

// Colors
#define CLR_WALL    0x18C3   // dark gray
#define CLR_PATH    TFT_WHITE
#define CLR_CURSOR  0xFD20   // orange
#define CLR_EXPLORE 0x2B5F   // dim blue
#define CLR_SOLVE   0x07E0   // green
#define CLR_BG      TFT_BLACK

// Grid: 0=wall, 1=passage
uint8_t grid[GH][GW];
bool visited[MH][MW];

// Directions: N, E, S, W
const int8_t dx[] = {0, 1, 0, -1};
const int8_t dy[] = {-1, 0, 1, 0};

// Generation algorithms
enum GenAlg { GEN_DFS, GEN_PRIM, GEN_BINARY, GEN_SIDEWINDER, GEN_COUNT };
const char* genNames[] = {"Backtracker", "Prim's", "Binary Tree", "Sidewinder"};
GenAlg currentGen = GEN_DFS;

// Solve algorithms
enum SolveAlg { SOLVE_BFS, SOLVE_DFS, SOLVE_ASTAR, SOLVE_WALL, SOLVE_DIJKSTRA, SOLVE_COUNT };
const char* solveNames[] = {"BFS", "DFS", "A*", "Wall Follower", "Dijkstra"};
SolveAlg currentSolve = SOLVE_BFS;

// App state
enum AppState { IDLE, SHOW_LABELS, GENERATING, GEN_PAUSE,
                SOLVING, TRACING, SOLVE_PAUSE, DONE };
AppState appState = IDLE;
unsigned long stateStartMs = 0;

// --- Generation state ---

// DFS gen state
struct Pos { int8_t x, y; };
Pos genStack[MW * MH];
int genTop = -1;

// Prim's frontier
Pos primFrontier[MW * MH];
int primCount = 0;

// Binary Tree / Sidewinder iteration
int stepCx, stepCy, runStart;

// --- Solve state ---

// Shared solve structures
int8_t parentDir[MH][MW]; // -1=unvisited, -2=start, 0-3=came from dir
bool solveFound;

// BFS state
Pos bfsQueue[MW * MH];
int bfsHead, bfsTail;

// DFS solve state
Pos solveStack[MW * MH];
int solveTop;

// A* state
struct AStarEntry { int8_t x, y; int16_t f; };
AStarEntry openList[MW * MH];
int openCount;
int16_t gScore[MH][MW];

// Dijkstra state (reuses gScore and openList/openCount from A*)
// Edge weights stored per wall passage for visual variety
uint8_t edgeWeight[GH][GW]; // random weights on passages

// Wall follower state
int wfX, wfY, wfDir; // current position and facing direction

// Solution path
Pos solvePath[MW * MH];
int solveLen, traceIdx;

#define STEPS_GEN 5
#define STEPS_SOLVE 8
#define STEPS_TRACE 3

// --- Drawing ---

void drawGrid(int gx, int gy, uint16_t color) {
  ui.tft.fillRect(OX + gx * CPX, OY + gy * CPX, CPX, CPX, color);
}

void drawCell(int cx, int cy, uint16_t color) {
  drawGrid(2 * cx + 1, 2 * cy + 1, color);
}

void carve(int cx1, int cy1, int cx2, int cy2, uint16_t cellColor) {
  int wx = cx1 + cx2 + 1;
  int wy = cy1 + cy2 + 1;
  grid[wy][wx] = 1;
  grid[2*cy1+1][2*cx1+1] = 1;
  grid[2*cy2+1][2*cx2+1] = 1;
  drawGrid(wx, wy, CLR_PATH);
  drawCell(cx1, cy1, CLR_PATH);
  drawCell(cx2, cy2, cellColor);
}

bool canMove(int cx, int cy, int d) {
  int nx = cx + dx[d], ny = cy + dy[d];
  if (nx < 0 || nx >= MW || ny < 0 || ny >= MH) return false;
  int wx = cx + nx + 1, wy = cy + ny + 1;
  return grid[wy][wx] == 1;
}

void showLabel(const char* text) {
  ui.tft.fillScreen(CLR_BG);
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.tft.setTextColor(TFT_WHITE, CLR_BG);
  int w = ui.tft.TTFtextWidth(text);
  ui.tft.setCursor((240 - w) / 2, 110);
  ui.tft.print(text);
}

// --- Init maze grid ---

void clearMaze() {
  memset(grid, 0, sizeof(grid));
  memset(visited, 0, sizeof(visited));
  for (int gy = 0; gy < GH; gy++)
    for (int gx = 0; gx < GW; gx++)
      drawGrid(gx, gy, CLR_WALL);
}

// ===================== GENERATION ALGORITHMS =====================

// --- DFS (Recursive Backtracker) ---

void genDfsInit() {
  clearMaze();
  genTop = 0;
  genStack[0] = {0, 0};
  visited[0][0] = true;
  grid[1][1] = 1;
  drawCell(0, 0, CLR_CURSOR);
}

bool genDfsStep() {
  if (genTop < 0) return true;
  Pos cur = genStack[genTop];

  int dirs[4], nd = 0;
  for (int d = 0; d < 4; d++) {
    int nx = cur.x + dx[d], ny = cur.y + dy[d];
    if (nx >= 0 && nx < MW && ny >= 0 && ny < MH && !visited[ny][nx])
      dirs[nd++] = d;
  }

  if (nd > 0) {
    int d = dirs[random(nd)];
    int nx = cur.x + dx[d], ny = cur.y + dy[d];
    visited[ny][nx] = true;
    drawCell(cur.x, cur.y, CLR_PATH);
    carve(cur.x, cur.y, nx, ny, CLR_CURSOR);
    genStack[++genTop] = {(int8_t)nx, (int8_t)ny};
  } else {
    drawCell(cur.x, cur.y, CLR_PATH);
    genTop--;
    if (genTop >= 0)
      drawCell(genStack[genTop].x, genStack[genTop].y, CLR_CURSOR);
  }
  return genTop < 0;
}

// --- Prim's ---

void primAddFrontier(int cx, int cy) {
  for (int d = 0; d < 4; d++) {
    int nx = cx + dx[d], ny = cy + dy[d];
    if (nx >= 0 && nx < MW && ny >= 0 && ny < MH && !visited[ny][nx]) {
      bool found = false;
      for (int i = 0; i < primCount; i++) {
        if (primFrontier[i].x == nx && primFrontier[i].y == ny) { found = true; break; }
      }
      if (!found)
        primFrontier[primCount++] = {(int8_t)nx, (int8_t)ny};
    }
  }
}

void genPrimInit() {
  clearMaze();
  primCount = 0;
  int sx = random(MW), sy = random(MH);
  visited[sy][sx] = true;
  grid[2*sy+1][2*sx+1] = 1;
  drawCell(sx, sy, CLR_PATH);
  primAddFrontier(sx, sy);
}

bool genPrimStep() {
  if (primCount == 0) return true;

  int idx = random(primCount);
  Pos cell = primFrontier[idx];
  primFrontier[idx] = primFrontier[--primCount];

  int dirs[4], nd = 0;
  for (int d = 0; d < 4; d++) {
    int nx = cell.x + dx[d], ny = cell.y + dy[d];
    if (nx >= 0 && nx < MW && ny >= 0 && ny < MH && visited[ny][nx])
      dirs[nd++] = d;
  }

  if (nd > 0) {
    int d = dirs[random(nd)];
    int nx = cell.x + dx[d], ny = cell.y + dy[d];
    visited[cell.y][cell.x] = true;
    carve(cell.x, cell.y, nx, ny, CLR_CURSOR);
    drawCell(cell.x, cell.y, CLR_PATH);
    primAddFrontier(cell.x, cell.y);
  }
  return primCount == 0;
}

// --- Binary Tree ---

void genBinaryInit() {
  clearMaze();
  stepCx = 0;
  stepCy = 0;
}

bool genBinaryStep() {
  if (stepCy >= MH) return true;

  int cx = stepCx, cy = stepCy;
  grid[2*cy+1][2*cx+1] = 1;

  bool canN = (cy > 0), canW = (cx > 0);
  if (canN && canW) {
    if (random(2) == 0) carve(cx, cy, cx, cy - 1, CLR_CURSOR);
    else carve(cx, cy, cx - 1, cy, CLR_CURSOR);
  } else if (canN) {
    carve(cx, cy, cx, cy - 1, CLR_CURSOR);
  } else if (canW) {
    carve(cx, cy, cx - 1, cy, CLR_CURSOR);
  } else {
    drawCell(cx, cy, CLR_CURSOR);
  }
  drawCell(cx, cy, CLR_PATH);

  stepCx++;
  if (stepCx >= MW) { stepCx = 0; stepCy++; }
  return stepCy >= MH;
}

// --- Sidewinder ---

void genSidewinderInit() {
  clearMaze();
  stepCx = 0;
  stepCy = 0;
  runStart = 0;
}

bool genSidewinderStep() {
  if (stepCy >= MH) return true;

  int cx = stepCx, cy = stepCy;
  grid[2*cy+1][2*cx+1] = 1;

  if (cy == 0) {
    if (cx > 0) carve(cx, cy, cx - 1, cy, CLR_CURSOR);
    drawCell(cx, cy, CLR_PATH);
  } else {
    bool atEnd = (cx == MW - 1);
    bool closeRun = atEnd || (random(2) == 0);

    if (closeRun) {
      int pick = runStart + random(cx - runStart + 1);
      carve(pick, cy, pick, cy - 1, CLR_CURSOR);
      drawCell(pick, cy, CLR_PATH);
      if (pick != cx) drawCell(cx, cy, CLR_PATH);
      runStart = cx + 1;
    } else {
      carve(cx, cy, cx + 1, cy, CLR_CURSOR);
      drawCell(cx, cy, CLR_PATH);
    }
  }

  stepCx++;
  if (stepCx >= MW) { stepCx = 0; stepCy++; runStart = 0; }
  return stepCy >= MH;
}

// --- Generation dispatch ---

void genInit() {
  switch (currentGen) {
    case GEN_DFS: genDfsInit(); break;
    case GEN_PRIM: genPrimInit(); break;
    case GEN_BINARY: genBinaryInit(); break;
    case GEN_SIDEWINDER: genSidewinderInit(); break;
    default: break;
  }
}

bool genStep() {
  switch (currentGen) {
    case GEN_DFS: return genDfsStep();
    case GEN_PRIM: return genPrimStep();
    case GEN_BINARY: return genBinaryStep();
    case GEN_SIDEWINDER: return genSidewinderStep();
    default: return true;
  }
}

// ===================== SOLVE ALGORITHMS =====================

// --- Common: build solution path from parentDir ---

void buildSolvePath() {
  solveLen = 0;
  int cx = MW - 1, cy = MH - 1;
  while (parentDir[cy][cx] != -2) {
    solvePath[solveLen++] = {(int8_t)cx, (int8_t)cy};
    int d = parentDir[cy][cx];
    cx -= dx[d];
    cy -= dy[d];
  }
  solvePath[solveLen++] = {0, 0};
  traceIdx = solveLen - 1;
}

// --- BFS Solver ---

void solveBfsInit() {
  memset(parentDir, -1, sizeof(parentDir));
  solveFound = false;
  bfsHead = 0;
  bfsTail = 0;
  bfsQueue[bfsTail++] = {0, 0};
  parentDir[0][0] = -2;
}

bool solveBfsStep() {
  if (solveFound || bfsHead >= bfsTail) return true;

  Pos cur = bfsQueue[bfsHead++];

  if (cur.x == MW - 1 && cur.y == MH - 1) {
    solveFound = true;
    buildSolvePath();
    return true;
  }

  if (cur.x != 0 || cur.y != 0)
    drawCell(cur.x, cur.y, CLR_EXPLORE);

  for (int d = 0; d < 4; d++) {
    int nx = cur.x + dx[d], ny = cur.y + dy[d];
    if (nx >= 0 && nx < MW && ny >= 0 && ny < MH &&
        parentDir[ny][nx] == -1 && canMove(cur.x, cur.y, d)) {
      parentDir[ny][nx] = d;
      bfsQueue[bfsTail++] = {(int8_t)nx, (int8_t)ny};
    }
  }
  return false;
}

// --- DFS Solver ---

void solveDfsInit() {
  memset(parentDir, -1, sizeof(parentDir));
  solveFound = false;
  solveTop = 0;
  solveStack[0] = {0, 0};
  parentDir[0][0] = -2;
}

bool solveDfsStep() {
  if (solveFound || solveTop < 0) return true;

  Pos cur = solveStack[solveTop];

  if (cur.x == MW - 1 && cur.y == MH - 1) {
    solveFound = true;
    buildSolvePath();
    return true;
  }

  if (cur.x != 0 || cur.y != 0)
    drawCell(cur.x, cur.y, CLR_EXPLORE);

  // Try to find an unvisited neighbor
  bool pushed = false;
  for (int d = 0; d < 4; d++) {
    int nx = cur.x + dx[d], ny = cur.y + dy[d];
    if (nx >= 0 && nx < MW && ny >= 0 && ny < MH &&
        parentDir[ny][nx] == -1 && canMove(cur.x, cur.y, d)) {
      parentDir[ny][nx] = d;
      solveStack[++solveTop] = {(int8_t)nx, (int8_t)ny};
      pushed = true;
      break; // DFS: go deep, one neighbor at a time
    }
  }

  if (!pushed) {
    solveTop--; // backtrack
  }
  return false;
}

// --- A* Solver ---

int16_t heuristic(int cx, int cy) {
  return abs(cx - (MW - 1)) + abs(cy - (MH - 1));
}

void solveAStarInit() {
  memset(parentDir, -1, sizeof(parentDir));
  solveFound = false;
  openCount = 0;
  memset(gScore, 0x7F, sizeof(gScore)); // init to large value
  gScore[0][0] = 0;
  parentDir[0][0] = -2;
  openList[openCount++] = {0, 0, heuristic(0, 0)};
}

bool solveAStarStep() {
  if (solveFound || openCount == 0) return true;

  // Find entry with lowest f score
  int bestIdx = 0;
  for (int i = 1; i < openCount; i++) {
    if (openList[i].f < openList[bestIdx].f)
      bestIdx = i;
  }

  AStarEntry cur = openList[bestIdx];
  openList[bestIdx] = openList[--openCount]; // remove

  if (cur.x == MW - 1 && cur.y == MH - 1) {
    solveFound = true;
    buildSolvePath();
    return true;
  }

  if (cur.x != 0 || cur.y != 0)
    drawCell(cur.x, cur.y, CLR_EXPLORE);

  int16_t newG = gScore[cur.y][cur.x] + 1;

  for (int d = 0; d < 4; d++) {
    int nx = cur.x + dx[d], ny = cur.y + dy[d];
    if (nx >= 0 && nx < MW && ny >= 0 && ny < MH &&
        canMove(cur.x, cur.y, d) && newG < gScore[ny][nx]) {
      gScore[ny][nx] = newG;
      parentDir[ny][nx] = d;
      int16_t f = newG + heuristic(nx, ny);
      openList[openCount++] = {(int8_t)nx, (int8_t)ny, f};
    }
  }
  return false;
}

// --- Wall Follower (Right-Hand Rule) ---

void solveWallInit() {
  memset(parentDir, -1, sizeof(parentDir));
  solveFound = false;
  wfX = 0;
  wfY = 0;
  wfDir = 1; // start facing East
  parentDir[0][0] = -2;
}

bool solveWallStep() {
  if (solveFound) return true;

  if (wfX == MW - 1 && wfY == MH - 1) {
    solveFound = true;
    buildSolvePath();
    return true;
  }

  if (wfX != 0 || wfY != 0)
    drawCell(wfX, wfY, CLR_EXPLORE);

  // Right-hand rule: try right, forward, left, back
  int tryDirs[4];
  tryDirs[0] = (wfDir + 1) % 4; // right
  tryDirs[1] = wfDir;            // forward
  tryDirs[2] = (wfDir + 3) % 4; // left
  tryDirs[3] = (wfDir + 2) % 4; // back

  for (int i = 0; i < 4; i++) {
    int d = tryDirs[i];
    if (canMove(wfX, wfY, d)) {
      int nx = wfX + dx[d], ny = wfY + dy[d];
      if (parentDir[ny][nx] == -1) {
        parentDir[ny][nx] = d;
      }
      wfX = nx;
      wfY = ny;
      wfDir = d;
      break;
    }
  }

  return false;
}

// --- Dijkstra's ---

void solveDijkstraInit() {
  memset(parentDir, -1, sizeof(parentDir));
  solveFound = false;
  openCount = 0;
  memset(gScore, 0x7F, sizeof(gScore));

  // Assign random weights to passages for visual variety
  for (int gy = 0; gy < GH; gy++)
    for (int gx = 0; gx < GW; gx++)
      edgeWeight[gy][gx] = (grid[gy][gx] == 1) ? (1 + random(10)) : 0;

  gScore[0][0] = 0;
  parentDir[0][0] = -2;
  openList[openCount++] = {0, 0, 0};
}

bool solveDijkstraStep() {
  if (solveFound || openCount == 0) return true;

  // Find entry with lowest distance
  int bestIdx = 0;
  for (int i = 1; i < openCount; i++) {
    if (openList[i].f < openList[bestIdx].f)
      bestIdx = i;
  }

  AStarEntry cur = openList[bestIdx];
  openList[bestIdx] = openList[--openCount];

  // Skip if we already found a better path
  if (cur.f > gScore[cur.y][cur.x]) return false;

  if (cur.x == MW - 1 && cur.y == MH - 1) {
    solveFound = true;
    buildSolvePath();
    return true;
  }

  if (cur.x != 0 || cur.y != 0)
    drawCell(cur.x, cur.y, CLR_EXPLORE);

  for (int d = 0; d < 4; d++) {
    int nx = cur.x + dx[d], ny = cur.y + dy[d];
    if (nx >= 0 && nx < MW && ny >= 0 && ny < MH && canMove(cur.x, cur.y, d)) {
      int wx = cur.x + nx + 1, wy = cur.y + ny + 1;
      int16_t newG = gScore[cur.y][cur.x] + edgeWeight[wy][wx];
      if (newG < gScore[ny][nx]) {
        gScore[ny][nx] = newG;
        parentDir[ny][nx] = d;
        openList[openCount++] = {(int8_t)nx, (int8_t)ny, newG};
      }
    }
  }
  return false;
}

// --- Solve dispatch ---

void solveInit() {
  switch (currentSolve) {
    case SOLVE_BFS: solveBfsInit(); break;
    case SOLVE_DFS: solveDfsInit(); break;
    case SOLVE_ASTAR: solveAStarInit(); break;
    case SOLVE_WALL: solveWallInit(); break;
    case SOLVE_DIJKSTRA: solveDijkstraInit(); break;
    default: break;
  }
}

bool solveStep() {
  switch (currentSolve) {
    case SOLVE_BFS: return solveBfsStep();
    case SOLVE_DFS: return solveDfsStep();
    case SOLVE_ASTAR: return solveAStarStep();
    case SOLVE_WALL: return solveWallStep();
    case SOLVE_DIJKSTRA: return solveDijkstraStep();
    default: return true;
  }
}

// --- Trace solution path ---

bool traceStep() {
  if (traceIdx < 0) return true;

  Pos p = solvePath[traceIdx--];
  drawCell(p.x, p.y, CLR_SOLVE);

  if (traceIdx >= 0) {
    Pos next = solvePath[traceIdx];
    int wx = p.x + next.x + 1;
    int wy = p.y + next.y + 1;
    drawGrid(wx, wy, CLR_SOLVE);
  }

  return traceIdx < 0;
}

// --- State machine ---

void showBothLabels() {
  ui.tft.fillScreen(CLR_BG);
  ui.tft.setTTFFont(Arial_14_Bold);

  // Gen label
  ui.tft.setTextColor(CLR_CURSOR, CLR_BG); // orange
  const char* gn = genNames[currentGen];
  int gw = ui.tft.TTFtextWidth(gn);
  ui.tft.setCursor((240 - gw) / 2, 90);
  ui.tft.print(gn);

  // Solve label
  ui.tft.setTextColor(CLR_SOLVE, CLR_BG); // green
  const char* sn = solveNames[currentSolve];
  int sw = ui.tft.TTFtextWidth(sn);
  ui.tft.setCursor((240 - sw) / 2, 120);
  ui.tft.print(sn);
}

void startGeneration() {
  randomSeed(esp_random());
  appState = SHOW_LABELS;
  stateStartMs = millis();
  showBothLabels();
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting maze app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ui.tft.fillScreen(CLR_BG);

  randomSeed(esp_random());

  startGeneration();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  // Button: skip to next cycle
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      currentGen = (GenAlg)((currentGen + 1) % GEN_COUNT);
      currentSolve = (SolveAlg)((currentSolve + 1) % SOLVE_COUNT);
      Serial.printf("Gen: %s, Solve: %s\n", genNames[currentGen], solveNames[currentSolve]);
      startGeneration();
    }
  }
  buttonWasPressed = pressed;

  switch (appState) {
    case SHOW_LABELS:
      if (millis() - stateStartMs > 1500) {
        ui.tft.fillScreen(CLR_BG);
        genInit();
        appState = GENERATING;
      }
      break;

    case GENERATING:
      for (int i = 0; i < STEPS_GEN; i++) {
        if (genStep()) {
          appState = GEN_PAUSE;
          stateStartMs = millis();
          break;
        }
      }
      delay(20);
      break;

    case GEN_PAUSE:
      if (millis() - stateStartMs > 1000) {
        solveInit();
        appState = SOLVING;
      }
      break;

    case SOLVING:
      for (int i = 0; i < STEPS_SOLVE; i++) {
        if (solveStep()) {
          appState = solveFound ? TRACING : DONE;
          break;
        }
      }
      delay(15);
      break;

    case TRACING:
      for (int i = 0; i < STEPS_TRACE; i++) {
        if (traceStep()) {
          appState = SOLVE_PAUSE;
          stateStartMs = millis();
          break;
        }
      }
      delay(30);
      break;

    case SOLVE_PAUSE:
      if (millis() - stateStartMs > 2000) {
        // 4 gen × 5 solve = 20 unique combos before repeating
        currentGen = (GenAlg)((currentGen + 1) % GEN_COUNT);
        currentSolve = (SolveAlg)((currentSolve + 1) % SOLVE_COUNT);
        Serial.printf("Next: Gen=%s, Solve=%s\n", genNames[currentGen], solveNames[currentSolve]);
        startGeneration();
      }
      break;

    case DONE:
    case IDLE:
      delay(100);
      break;
  }
}
