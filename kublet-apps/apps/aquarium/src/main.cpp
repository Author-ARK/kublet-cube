#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;

// Strip-based framebuffer: 240x80 at 16-bit = 38,400 bytes (fits in ESP32 RAM)
TFT_eSprite fb = TFT_eSprite(&ui.tft);

#define SW 240
#define SH 240
#define STRIP_H 80
#define NUM_STRIPS 3

int stripY = 0; // current strip's world-y offset

// Colors
uint16_t CLR_BUBBLE;
uint16_t CLR_BUBBLE_HI;

#define SAND_Y 215

// --- SEAWEED ---
#define NUM_WEEDS 8
struct Seaweed {
  int16_t x, baseY;
  int8_t segments;
  uint16_t color;
  float phase, speed;
};
Seaweed weeds[NUM_WEEDS];

// --- KELP (tall, lush plants) ---
#define NUM_KELP 4
struct Kelp {
  int16_t x, baseY;
  int8_t segments;   // 10-16 segments = tall
  uint16_t colorStem, colorLeaf;
  float phase, speed;
};
Kelp kelps[NUM_KELP];

// --- CORAL (organic structures) ---
#define NUM_CORAL 2
#define CORAL_MAX_ARMS 5
struct Coral {
  int16_t x, baseY;
  int8_t numArms;
  uint16_t color1, color2;
  float phase, speed;
  // Per-arm randomized shape params
  float armAngle[CORAL_MAX_ARMS];   // growth direction
  float armCurve[CORAL_MAX_ARMS];   // how much it curves
  int8_t armLen[CORAL_MAX_ARMS];    // length in segments
  int8_t armThick[CORAL_MAX_ARMS];  // thickness
};
Coral corals[NUM_CORAL];

// Total plant slots for spacing: weeds + kelp + coral
#define TOTAL_PLANTS (NUM_WEEDS + NUM_KELP + NUM_CORAL)
int16_t plantPositions[TOTAL_PLANTS];

// --- BUBBLES ---
#define MAX_BUBBLES 15
struct Bubble {
  float x, y, vx;
  int8_t r;
  bool active;
};
Bubble bubbles[MAX_BUBBLES];

// --- CREATURES ---
enum CreatureType { FISH_SMALL, FISH_MED, FISH_LARGE, JELLYFISH, CRAB, SEAHORSE };
#define MAX_CREATURES 9

struct Creature {
  CreatureType type;
  float x, y, vx, vy;
  int8_t dir;
  uint16_t color1, color2;
  float animPhase, animSpeed;
  bool active;
  int8_t state;
};
Creature creatures[MAX_CREATURES];

int themeIndex = 0;
bool buttonWasPressed = false;
unsigned long lastFrame = 0, lastBubble = 0;

struct Theme {
  uint8_t wr, wg, wb, sr, sg, sb;
  bool night;
};
const Theme themes[] = {
  {10, 30, 80,   194, 178, 128,  false},
  {5, 10, 35,    140, 125, 90,   true},
  {20, 60, 60,   180, 170, 120,  false},
};
#define NUM_THEMES 3

// Forward declarations
void drawBackground();
void drawWeeds();
void drawBubbles();
void drawCreatures();
void spawnCreature(int idx, CreatureType type);
void spawnBubble(float x, float y);
uint16_t waterColor(int y);

// --- Helper: draw NxN pixel in strip-local coords ---
inline void drawPixelNx(int bx, int by, int px, int py, int n, uint16_t col) {
  int sy = (by + py * n) - stripY;
  if (sy < -(n-1) || sy >= STRIP_H) return;
  fb.fillRect(bx + px * n, sy, n, n, col);
}
inline void drawPixel2x(int bx, int by, int px, int py, uint16_t col) { drawPixelNx(bx, by, px, py, 2, col); }
inline void drawPixel3x(int bx, int by, int px, int py, uint16_t col) { drawPixelNx(bx, by, px, py, 3, col); }

// --- SMALL FISH --- body flexes with a sine wave, tail swings wide
void drawFishSmall(int x, int y, int dir, uint16_t c1, uint16_t c2, float anim) {
  int bx = x - 10, by = y - 7;
  // Body flex: each column gets a vertical offset
  auto flex = [&](int col) -> int {
    int headCol = (dir == 1) ? 8 : 1;
    int dist = abs(col - headCol);
    return (int)(sin(anim + dist * 0.5f) * (dist * 0.25f));
  };
  int finFlap = (int)(sin(anim * 2.0f) * 1);

  if (dir == 1) {
    // Top edge
    drawPixel2x(bx, by, 3, 0 + flex(3), c1); drawPixel2x(bx, by, 4, 0 + flex(4), c1);
    drawPixel2x(bx, by, 5, 0 + flex(5), c1);
    // Upper body
    drawPixel2x(bx, by, 2, 1 + flex(2), c1); drawPixel2x(bx, by, 3, 1 + flex(3), c1);
    drawPixel2x(bx, by, 4, 1 + flex(4), c1); drawPixel2x(bx, by, 5, 1 + flex(5), c1);
    drawPixel2x(bx, by, 6, 1 + flex(6), c1); drawPixel2x(bx, by, 7, 1 + flex(7), c2);
    // Mid body — stripe + eye
    drawPixel2x(bx, by, 2, 2 + flex(2), c2); drawPixel2x(bx, by, 3, 2 + flex(3), c2);
    drawPixel2x(bx, by, 4, 2 + flex(4), c1); drawPixel2x(bx, by, 5, 2 + flex(5), c1);
    drawPixel2x(bx, by, 6, 2 + flex(6), c1); drawPixel2x(bx, by, 7, 2 + flex(7), c1);
    drawPixel2x(bx, by, 8, 2 + flex(8), TFT_BLACK); // eye
    // Lower body
    drawPixel2x(bx, by, 2, 3 + flex(2), c1); drawPixel2x(bx, by, 3, 3 + flex(3), c1);
    drawPixel2x(bx, by, 4, 3 + flex(4), c1); drawPixel2x(bx, by, 5, 3 + flex(5), c1);
    drawPixel2x(bx, by, 6, 3 + flex(6), c1); drawPixel2x(bx, by, 7, 3 + flex(7), c2);
    // Bottom edge
    drawPixel2x(bx, by, 3, 4 + flex(3), c1); drawPixel2x(bx, by, 4, 4 + flex(4), c1);
    drawPixel2x(bx, by, 5, 4 + flex(5), c1);
    // Tail — wide swing, forked
    int tw = (int)(sin(anim) * 2);
    drawPixel2x(bx, by, 1, 1 + tw, c2); drawPixel2x(bx, by, 0, 0 + tw, c2);
    drawPixel2x(bx, by, 1, 2 + tw, c1);
    drawPixel2x(bx, by, 1, 3 + tw, c2); drawPixel2x(bx, by, 0, 4 + tw, c2);
    // Pectoral fin
    drawPixel2x(bx, by, 6, 4 + finFlap, c2);
  } else {
    drawPixel2x(bx, by, 5, 0 + flex(5), c1); drawPixel2x(bx, by, 4, 0 + flex(4), c1);
    drawPixel2x(bx, by, 3, 0 + flex(3), c1);
    drawPixel2x(bx, by, 6, 1 + flex(6), c1); drawPixel2x(bx, by, 5, 1 + flex(5), c1);
    drawPixel2x(bx, by, 4, 1 + flex(4), c1); drawPixel2x(bx, by, 3, 1 + flex(3), c1);
    drawPixel2x(bx, by, 2, 1 + flex(2), c1); drawPixel2x(bx, by, 1, 1 + flex(1), c2);
    drawPixel2x(bx, by, 6, 2 + flex(6), c2); drawPixel2x(bx, by, 5, 2 + flex(5), c2);
    drawPixel2x(bx, by, 4, 2 + flex(4), c1); drawPixel2x(bx, by, 3, 2 + flex(3), c1);
    drawPixel2x(bx, by, 2, 2 + flex(2), c1); drawPixel2x(bx, by, 1, 2 + flex(1), c1);
    drawPixel2x(bx, by, 0, 2 + flex(0), TFT_BLACK);
    drawPixel2x(bx, by, 6, 3 + flex(6), c1); drawPixel2x(bx, by, 5, 3 + flex(5), c1);
    drawPixel2x(bx, by, 4, 3 + flex(4), c1); drawPixel2x(bx, by, 3, 3 + flex(3), c1);
    drawPixel2x(bx, by, 2, 3 + flex(2), c1); drawPixel2x(bx, by, 1, 3 + flex(1), c2);
    drawPixel2x(bx, by, 5, 4 + flex(5), c1); drawPixel2x(bx, by, 4, 4 + flex(4), c1);
    drawPixel2x(bx, by, 3, 4 + flex(3), c1);
    int tw = (int)(sin(anim) * 2);
    drawPixel2x(bx, by, 7, 1 + tw, c2); drawPixel2x(bx, by, 8, 0 + tw, c2);
    drawPixel2x(bx, by, 7, 2 + tw, c1);
    drawPixel2x(bx, by, 7, 3 + tw, c2); drawPixel2x(bx, by, 8, 4 + tw, c2);
    drawPixel2x(bx, by, 2, 4 + finFlap, c2);
  }
}

// --- MEDIUM FISH --- body wave + tail swing + fin flap
void drawFishMed(int x, int y, int dir, uint16_t c1, uint16_t c2, float anim) {
  int bx = x - 14, by = y - 10;
  int tw = (int)(sin(anim) * 2);
  // Fin flap: oscillates up/down
  int finFlap = (int)(sin(anim * 1.5f) * 1);

  if (dir == 1) {
    for (int py = 1; py <= 5; py++) {
      int startX = (py == 1 || py == 5) ? 3 : (py == 2 || py == 4) ? 2 : 1;
      int endX = (py == 1 || py == 5) ? 7 : 8;
      for (int px = startX; px <= endX; px++) {
        // Body wave: columns closer to tail flex more
        int dist = 8 - px;
        int wave = (int)(sin(anim + dist * 0.5f) * (dist * 0.15f));
        drawPixel2x(bx, by, px, py + wave, ((px + py) % 3 == 0) ? c2 : c1);
      }
    }
    drawPixel2x(bx, by, 8, 3, TFT_BLACK);
    // Tail — wider swing
    drawPixel2x(bx, by, 0, 2 + tw, c2); drawPixel2x(bx, by, 0, 3 + tw, c2);
    drawPixel2x(bx, by, 0, 4 + tw, c2);
    drawPixel2x(bx, by, -1, 2 + tw, c2); drawPixel2x(bx, by, -1, 4 + tw, c2);
    // Dorsal fin flaps
    drawPixel2x(bx, by, 4, 0 + finFlap, c2); drawPixel2x(bx, by, 5, 0 + finFlap, c2);
    // Pectoral fin
    drawPixel2x(bx, by, 6, 5 - finFlap, c2);
  } else {
    for (int py = 1; py <= 5; py++) {
      int startX = (py == 1 || py == 5) ? 3 : 2;
      int endX = (py == 1 || py == 5) ? 7 : (py == 2 || py == 4) ? 8 : 9;
      for (int px = startX; px <= endX; px++) {
        int dist = px - 2;
        int wave = (int)(sin(anim + dist * 0.5f) * (dist * 0.15f));
        drawPixel2x(bx, by, px, py + wave, ((px + py) % 3 == 0) ? c2 : c1);
      }
    }
    drawPixel2x(bx, by, 2, 3, TFT_BLACK);
    drawPixel2x(bx, by, 10, 2 + tw, c2); drawPixel2x(bx, by, 10, 3 + tw, c2);
    drawPixel2x(bx, by, 10, 4 + tw, c2);
    drawPixel2x(bx, by, 11, 2 + tw, c2); drawPixel2x(bx, by, 11, 4 + tw, c2);
    drawPixel2x(bx, by, 5, 0 + finFlap, c2); drawPixel2x(bx, by, 6, 0 + finFlap, c2);
    drawPixel2x(bx, by, 4, 5 - finFlap, c2);
  }
}

// --- LARGE FISH --- slow undulation + tail sweep + dorsal fin wave
void drawFishLarge(int x, int y, int dir, uint16_t c1, uint16_t c2, float anim) {
  int bx = x - 18, by = y - 12;
  int tw = (int)(sin(anim) * 2);
  int finFlap = (int)(sin(anim * 1.2f) * 1);

  if (dir == 1) {
    for (int py = 1; py <= 6; py++) {
      int sx = (py <= 1 || py >= 6) ? 4 : (py <= 2 || py >= 5) ? 3 : 2;
      int ex = (py <= 1 || py >= 6) ? 9 : 10;
      for (int px = sx; px <= ex; px++) {
        int dist = 10 - px;
        int wave = (int)(sin(anim * 0.8f + dist * 0.4f) * (dist * 0.12f));
        drawPixel3x(bx, by, px, py + wave, ((px + py) % 4 == 0) ? c2 : c1);
      }
    }
    drawPixel3x(bx, by, 10, 3, TFT_BLACK); drawPixel3x(bx, by, 10, 4, TFT_BLACK);
    // Big tail sweep
    drawPixel3x(bx, by, 1, 2 + tw, c2); drawPixel3x(bx, by, 0, 2 + tw, c2);
    drawPixel3x(bx, by, 1, 3 + tw, c1); drawPixel3x(bx, by, 0, 3 + tw, c1);
    drawPixel3x(bx, by, 1, 5 + tw, c2); drawPixel3x(bx, by, 0, 5 + tw, c2);
    // Dorsal fin wave
    drawPixel3x(bx, by, 5, 0 + finFlap, c2);
    drawPixel3x(bx, by, 6, 0 + finFlap, c2);
    drawPixel3x(bx, by, 7, 0 + finFlap, c2);
    // Pectoral fin
    drawPixel3x(bx, by, 8, 6 - finFlap, c2);
  } else {
    for (int py = 1; py <= 6; py++) {
      int sx = (py <= 1 || py >= 6) ? 3 : 2;
      int ex = (py <= 1 || py >= 6) ? 8 : (py <= 2 || py >= 5) ? 9 : 10;
      for (int px = sx; px <= ex; px++) {
        int dist = px - 2;
        int wave = (int)(sin(anim * 0.8f + dist * 0.4f) * (dist * 0.12f));
        drawPixel3x(bx, by, px, py + wave, ((px + py) % 4 == 0) ? c2 : c1);
      }
    }
    drawPixel3x(bx, by, 2, 3, TFT_BLACK); drawPixel3x(bx, by, 2, 4, TFT_BLACK);
    drawPixel3x(bx, by, 11, 2 + tw, c2); drawPixel3x(bx, by, 12, 2 + tw, c2);
    drawPixel3x(bx, by, 11, 3 + tw, c1); drawPixel3x(bx, by, 12, 3 + tw, c1);
    drawPixel3x(bx, by, 11, 5 + tw, c2); drawPixel3x(bx, by, 12, 5 + tw, c2);
    drawPixel3x(bx, by, 5, 0 + finFlap, c2);
    drawPixel3x(bx, by, 6, 0 + finFlap, c2);
    drawPixel3x(bx, by, 7, 0 + finFlap, c2);
    drawPixel3x(bx, by, 4, 6 - finFlap, c2);
  }
}

// --- JELLYFISH --- large bell pulses, flowing tentacles, inner detail
void drawJellyfish(int x, int y, uint16_t c1, uint16_t c2, float anim) {
  int bx = x - 14, by = y - 18;

  // Bell pulsation
  float pulse = sin(anim * 1.5f);
  int bellExpand = (int)(pulse * 1.5f);
  int bellSquish = (int)(pulse * -0.6f);

  // Larger bell dome — 7 rows, wider
  for (int py = 0; py < 7; py++) {
    int baseHW;
    if (py == 0)      baseHW = 2;
    else if (py == 1) baseHW = 3;
    else if (py == 2) baseHW = 4;
    else if (py <= 4) baseHW = 5;
    else if (py == 5) baseHW = 6;
    else              baseHW = 5;

    int hw = baseHW + ((py >= 3) ? bellExpand : 0);
    if (hw < 1) hw = 1;
    int yOff = (py < 3) ? 0 : bellSquish;
    int cx = 6; // center column

    for (int px = cx - hw; px <= cx + hw; px++) {
      // Color gradient: lighter at top, body color below
      uint16_t c = (py < 2) ? c2 : c1;
      // Shimmering highlights travel across
      if ((px + (int)(anim * 2.5f)) % 4 == 0) c = c2;
      // Inner organs visible through translucent bell
      if (py >= 2 && py <= 4 && abs(px - cx) <= 1) {
        int organPulse = (int)(sin(anim * 2.0f + py) * 0.5f);
        if (organPulse == 0) c = c2;
      }
      drawPixel2x(bx, by, px, py + yOff, c);
    }
  }

  // Ruffled skirt/rim — wider, wavy edge
  for (int px = 0; px <= 12; px++) {
    int rimWave = (int)(sin(anim * 2.5f + px * 0.7f) * 1.0f);
    drawPixel2x(bx, by, px, 7 + rimWave + bellSquish, c2);
  }

  // Oral arms (thicker inner tentacles) — 2 of them
  for (int a = 0; a < 2; a++) {
    int ax = 4 + a * 4;
    float armPhase = anim * 1.0f + a * 2.5f;
    for (int ay = 8; ay < 14; ay++) {
      float depth = (ay - 8) * 0.5f;
      int wave = (int)(sin(armPhase + depth) * (1.2f + depth * 0.2f));
      drawPixel2x(bx, by, ax + wave, ay, c1);
      drawPixel2x(bx, by, ax + wave + 1, ay, c1);
    }
  }

  // Thin trailing tentacles — 5 of them, longer and flowing
  for (int t = 0; t < 5; t++) {
    int tx = 1 + t * 2;
    float tentPhase = anim * 1.3f + t * 1.6f;
    for (int ty = 8; ty < 16; ty++) {
      float depth = (ty - 8) * 0.35f;
      int wave = (int)(sin(tentPhase + depth) * (1.0f + depth * 0.4f));
      uint16_t c = (ty < 12) ? c2 : c1;
      drawPixel2x(bx, by, tx + wave, ty, c);
    }
  }
}

// --- CRAB --- round body, raised claws, animated walk
void drawCrab(int x, int y, int dir, uint16_t c1, uint16_t c2, float anim) {
  int bx = x - 15, by = y - 15;

  // Body rocks side to side as it scuttles
  int bodyRock = (int)(sin(anim * 3.0f) * 0.8f);
  int bodyBob  = (int)(sin(anim * 5.0f) * 0.6f);

  // Claws pump up and down alternately while walking
  int leftClawUp  = (int)(sin(anim * 2.0f) * 1.2f);
  int rightClawUp = (int)(sin(anim * 2.0f + 3.14f) * 1.2f);
  // Claws snap open/shut
  int leftSnap  = (int)(sin(anim * 1.5f) * 1);
  int rightSnap = (int)(sin(anim * 1.5f + 2.0f) * 1);

  // Legs: alternating gait
  auto legOff = [&](int i) -> int {
    return (int)(sin(anim * 5.0f + i * 2.1f) * 1.5f);
  };

  int cx = 5 + bodyRock;
  int bb = bodyBob;

  // --- Left claw: raised, pumps up/down, snaps ---
  drawPixel3x(bx, by, cx - 3, 3 + bb, c1);  // arm base
  drawPixel3x(bx, by, cx - 4, 2 + bb + leftClawUp, c1);  // arm mid
  drawPixel3x(bx, by, cx - 4, 1 + bb + leftClawUp, c2);  // claw base
  drawPixel3x(bx, by, cx - 5, 0 - leftSnap + bb + leftClawUp, c2); // top pincer
  drawPixel3x(bx, by, cx - 5, 1 + bb + leftClawUp, c2);            // claw palm
  drawPixel3x(bx, by, cx - 5, 2 + leftSnap + bb + leftClawUp, c2); // bottom pincer

  // --- Right claw: opposite phase ---
  drawPixel3x(bx, by, cx + 3, 3 + bb, c1);
  drawPixel3x(bx, by, cx + 4, 2 + bb + rightClawUp, c1);
  drawPixel3x(bx, by, cx + 4, 1 + bb + rightClawUp, c2);
  drawPixel3x(bx, by, cx + 5, 0 - rightSnap + bb + rightClawUp, c2);
  drawPixel3x(bx, by, cx + 5, 1 + bb + rightClawUp, c2);
  drawPixel3x(bx, by, cx + 5, 2 + rightSnap + bb + rightClawUp, c2);

  // --- Eyes on stalks — bob with body, wobble slightly ---
  int eyeWobble = (int)(sin(anim * 3.5f) * 0.5f);
  drawPixel3x(bx, by, cx - 1, 2 + bb, c2); // left stalk
  drawPixel3x(bx, by, cx + 1, 2 + bb, c2); // right stalk
  drawPixel3x(bx, by, cx - 1, 1 + bb + eyeWobble, TFT_WHITE);
  drawPixel3x(bx, by, cx + 1, 1 + bb - eyeWobble, TFT_WHITE);
  // Pupils — look in walking direction
  int pupilOff = (dir == 1) ? 2 : 0;
  fb.fillRect(bx + (cx - 1) * 3 + pupilOff, (by + (1 + bb + eyeWobble) * 3 + 1) - stripY, 1, 1, TFT_BLACK);
  fb.fillRect(bx + (cx + 1) * 3 + pupilOff, (by + (1 + bb - eyeWobble) * 3 + 1) - stripY, 1, 1, TFT_BLACK);

  // --- Round shell body — rocks with bodyRock ---
  int shellHW[] = {3, 4, 4, 4, 3, 2};
  for (int r = 0; r < 6; r++) {
    int hw = shellHW[r];
    int py = r + 3;
    for (int px = cx - hw; px <= cx + hw; px++) {
      uint16_t c = c1;
      if (r == 0 || r == 5) c = c2;
      else if (abs(px - cx) == hw) c = c2;
      else if (r == 2 && abs(px - cx) <= 1) c = c2;
      drawPixel3x(bx, by, px, py + bb, c);
    }
  }

  // --- 3 pairs of legs — scuttling gait ---
  for (int i = 0; i < 3; i++) {
    int lo = legOff(i);
    int loR = legOff(i + 3); // opposite phase for right side
    // Left leg
    int llx = cx - 2 - i;
    drawPixel3x(bx, by, llx, 8 + bb, c1);
    drawPixel3x(bx, by, llx - 1, 9 + lo, c1);
    drawPixel3x(bx, by, llx - 1, 10 + lo, c2);
    // Right leg
    int rlx = cx + 2 + i;
    drawPixel3x(bx, by, rlx, 8 + bb, c1);
    drawPixel3x(bx, by, rlx + 1, 9 + loR, c1);
    drawPixel3x(bx, by, rlx + 1, 10 + loR, c2);
  }
}

// --- SEAHORSE --- clean S-curve silhouette, golden colors
void drawSeahorse(int x, int y, int dir, uint16_t c1, uint16_t c2, float anim) {
  int bx = x - 12, by = y - 30;
  int headBob = (int)(sin(anim * 1.5f) * 0.7f);
  int hb = headBob;
  int d = (dir == 1) ? 1 : -1;

  // --- Crown: small pointed crest ---
  int cf = (int)(sin(anim * 3.0f) * 0.5f);
  drawPixel3x(bx, by, 3, 0 + hb + cf, c2);
  drawPixel3x(bx, by, 2, 1 + hb + cf, c2);
  drawPixel3x(bx, by, 4, 1 + hb + cf, c2);

  // --- Head: rounded 3x3 block ---
  for (int px = 2; px <= 4; px++)
    drawPixel3x(bx, by, px, 2 + hb, c1);
  for (int px = 2; px <= 4; px++)
    drawPixel3x(bx, by, px, 3 + hb, c1);
  drawPixel3x(bx, by, 3, 4 + hb, c1); // chin

  // Eye — white with dark center
  int eyeX = (dir == 1) ? 4 : 2;
  drawPixel3x(bx, by, eyeX, 2 + hb, TFT_WHITE);
  fb.fillRect(bx + eyeX * 3 + 1, (by + (2 + hb) * 3 + 1) - stripY, 1, 1, TFT_BLACK);

  // --- Snout: extends forward from face ---
  int snX = (dir == 1) ? 5 : 1;
  drawPixel3x(bx, by, snX, 3 + hb, c2);
  drawPixel3x(bx, by, snX + d, 3 + hb, c2);

  // --- Body: clean S-curve, 12 segments ---
  // S-curve offset table (pre-designed curve, not just sine)
  // Segments 0-2: neck (leans forward), 3-6: chest/belly (back), 7-10: lower (forward), 11: pre-tail
  float sCurve[] = {0.3f, 0.5f, 0.3f, 0.0f, -0.3f, -0.5f, -0.3f, 0.0f, 0.3f, 0.5f, 0.6f, 0.5f};
  int bodyW[]    = {2,    2,    3,    3,     3,      3,      2,     2,    2,    1,    1,    1};

  for (int seg = 0; seg < 12; seg++) {
    int py = 5 + seg;
    // Gentle wave animation added to the S-curve
    float bodyWave = sin(anim * 1.2f + seg * 0.3f) * 0.5f;
    int cx = 3 + (int)(sCurve[seg] * 2.0f * d + bodyWave);
    int w = bodyW[seg];

    for (int bw = 0; bw < w; bw++) {
      uint16_t c = c1;
      // Belly ridges: alternating stripe pattern
      if (seg % 2 == 0 && bw == 0) c = c2;
      drawPixel3x(bx, by, cx + bw, py, c);
    }

    // Dorsal fin — small bumps along back, flutters
    if (seg >= 1 && seg <= 8) {
      int finSide = (dir == 1) ? -1 : w;
      int finFlutter = (int)(sin(anim * 4.0f + seg * 0.9f) * 0.7f);
      drawPixel3x(bx, by, cx + finSide, py + finFlutter, c2);
    }

    // Belly pouch: slight bump on front side
    if (seg >= 3 && seg <= 6) {
      int bellySide = (dir == 1) ? w : -1;
      drawPixel3x(bx, by, cx + bellySide, py, c2);
    }
  }

  // --- Curled tail: tight spiral ---
  float tailWave = sin(anim * 1.2f + 12 * 0.3f) * 0.5f;
  int tb = 3 + (int)(0.5f * 2.0f * d + tailWave);
  // Curl: down, in, up to form a loop
  drawPixel3x(bx, by, tb, 17, c1);
  drawPixel3x(bx, by, tb + d, 18, c2);
  drawPixel3x(bx, by, tb + d, 19, c2);
  drawPixel3x(bx, by, tb, 19, c1);
  drawPixel3x(bx, by, tb - d, 18, c1); // inner curl
}

// --- WATER COLOR ---
uint16_t waterColor(int y) {
  const Theme& t = themes[themeIndex];
  float f = (float)y / SAND_Y;
  if (f > 1) f = 1;
  uint8_t r = t.wr + (int)((t.wr * 0.5f) * f);
  uint8_t g = t.wg + (int)((t.wg * 1.0f) * f);
  uint8_t b = t.wb + (int)((t.wb * 0.8f) * f);
  if (t.night) { r /= 2; g /= 2; b /= 2; }
  return fb.color565(r, g, b);
}

// --- BACKGROUND (renders only current strip) ---
void drawBackground() {
  const Theme& t = themes[themeIndex];
  int yEnd = stripY + STRIP_H;

  // Water gradient bands
  for (int y = max(0, stripY); y < min(SAND_Y, yEnd); y += 4) {
    int bandEnd = min(y + 4, min(SAND_Y, yEnd));
    uint16_t c = waterColor(y);
    fb.fillRect(0, y - stripY, SW, bandEnd - y, c);
  }

  // Sandy bottom
  for (int y = max(SAND_Y, stripY); y < min(SH, yEnd); y += 2) {
    int bandEnd = min(y + 2, min(SH, yEnd));
    uint16_t c = (y % 4 < 2) ?
      fb.color565(t.sr, t.sg, t.sb) :
      fb.color565(max(0, t.sr - 20), max(0, t.sg - 20), max(0, t.sb - 15));
    fb.fillRect(0, y - stripY, SW, bandEnd - y, c);
  }

  // Pebbles (only if in strip range)
  auto pebble = [&](int px, int py, int pr, uint8_t cr, uint8_t cg, uint8_t cb) {
    if (py + pr >= stripY && py - pr < yEnd)
      fb.fillCircle(px, py - stripY, pr, fb.color565(cr, cg, cb));
  };
  pebble(40, 225, 4, 120, 110, 90);
  pebble(150, 230, 3, 130, 120, 100);
  pebble(200, 222, 5, 110, 100, 85);
}

// --- PLANT SPACING ---
// Distribute plant positions evenly across the screen width with some jitter
void assignPlantPositions() {
  int spacing = (SW - 20) / TOTAL_PLANTS; // ~13px apart for 16 plants
  for (int i = 0; i < TOTAL_PLANTS; i++) {
    plantPositions[i] = 10 + i * spacing + random(max(1, spacing - 6));
  }
  // Shuffle to randomize which plant type gets which slot
  for (int i = TOTAL_PLANTS - 1; i > 0; i--) {
    int j = random(i + 1);
    int16_t tmp = plantPositions[i];
    plantPositions[i] = plantPositions[j];
    plantPositions[j] = tmp;
  }
}

// --- SEAWEED ---
void initWeeds() {
  for (int i = 0; i < NUM_WEEDS; i++) {
    weeds[i].x = plantPositions[i];
    weeds[i].baseY = SAND_Y;
    weeds[i].segments = 5 + random(6);
    int shade = random(60);
    weeds[i].color = ui.tft.color565(10 + shade, 100 + random(80), 20 + shade);
    weeds[i].phase = random(1000) / 100.0f;
    weeds[i].speed = 1.0f + random(100) / 100.0f;
  }
}

void updateWeeds(float dt) {
  for (int i = 0; i < NUM_WEEDS; i++) {
    weeds[i].phase += dt * weeds[i].speed;
    if (weeds[i].phase > 6.2832f) weeds[i].phase -= 6.2832f;
  }
}

void drawWeeds() {
  int yEnd = stripY + STRIP_H;
  for (int i = 0; i < NUM_WEEDS; i++) {
    Seaweed& w = weeds[i];
    for (int s = 0; s < w.segments; s++) {
      float sway = sin(w.phase + s * 0.6f) * (s * 0.8f);
      int sx = w.x + (int)sway;
      int sy = w.baseY - s * 6;
      int width = max(2, 4 - s / 3);
      int ry = sy - 3;
      if (ry + 6 >= stripY && ry < yEnd)
        fb.fillRect(sx - width / 2, ry - stripY, width, 6, w.color);
    }
  }
}

// --- KELP ---
void initKelp() {
  for (int i = 0; i < NUM_KELP; i++) {
    kelps[i].x = plantPositions[NUM_WEEDS + i];
    kelps[i].baseY = SAND_Y;
    kelps[i].segments = 10 + random(7); // 10-16 segments = very tall
    int shade = random(40);
    kelps[i].colorStem = ui.tft.color565(20 + shade, 80 + random(40), 15 + shade);
    kelps[i].colorLeaf = ui.tft.color565(30 + shade, 140 + random(60), 25 + shade);
    kelps[i].phase = random(1000) / 100.0f;
    kelps[i].speed = 0.6f + random(60) / 100.0f;
  }
}

void updateKelp(float dt) {
  for (int i = 0; i < NUM_KELP; i++) {
    kelps[i].phase += dt * kelps[i].speed;
    if (kelps[i].phase > 6.2832f) kelps[i].phase -= 6.2832f;
  }
}

void drawKelp() {
  int yEnd = stripY + STRIP_H;
  for (int i = 0; i < NUM_KELP; i++) {
    Kelp& k = kelps[i];
    for (int s = 0; s < k.segments; s++) {
      // Stem sways more at the top
      float sway = sin(k.phase + s * 0.4f) * (s * 0.6f);
      int sx = k.x + (int)sway;
      int sy = k.baseY - s * 8;

      // Thick stem (3px wide)
      int ry = sy - 4;
      if (ry + 8 >= stripY && ry < yEnd)
        fb.fillRect(sx - 1, ry - stripY, 3, 8, k.colorStem);

      // Leafy fronds on alternating sides, starting from segment 2
      if (s >= 2) {
        float leafSway = sin(k.phase * 1.3f + s * 0.7f) * 2.0f;
        int leafDir = (s % 2 == 0) ? 1 : -1;
        int leafLen = 4 + (k.segments - s) / 3; // longer leaves lower down
        int lx = sx + leafDir * 2 + (int)(leafSway * leafDir * 0.5f);
        int ly = sy - 2;

        if (ly + 4 >= stripY && ly < yEnd) {
          // Draw leaf as a tapered shape
          for (int l = 0; l < leafLen; l++) {
            int lsx = lx + leafDir * l * 2 + (int)(sin(k.phase + l * 0.5f + s) * 1.0f);
            int lsy = ly - l + (int)(sin(k.phase * 0.8f + l * 0.3f) * 1.5f);
            int lh = max(1, 3 - l / 2);
            if (lsy + lh >= stripY && lsy < yEnd)
              fb.fillRect(lsx, lsy - stripY, 2, lh, k.colorLeaf);
          }
        }
      }
    }
  }
}

// --- CORAL ---
void initCoral() {
  for (int i = 0; i < NUM_CORAL; i++) {
    Coral& c = corals[i];
    c.x = plantPositions[NUM_WEEDS + NUM_KELP + i];
    c.baseY = SAND_Y;
    c.numArms = 3 + random(3); // 3-5 arms
    c.phase = random(1000) / 100.0f;
    c.speed = 0.4f + random(40) / 100.0f;
    // Spread arms across an arc, with randomness
    // Spread arms in an upward arc from -0.8 to +0.8 rad (~-45° to +45° from vertical)
    float arcStart = -0.8f;
    float arcSpan = 1.6f;
    for (int a = 0; a < c.numArms; a++) {
      c.armAngle[a] = arcStart + arcSpan * a / (c.numArms - 1 + 0.01f) + (random(100) - 50) / 300.0f;
      // Curve gently inward (toward center), never enough to point downward
      c.armCurve[a] = (random(100) - 50) / 400.0f;
      c.armLen[a] = 4 + random(5);   // 4-8 segments
      c.armThick[a] = 2 + random(2); // 2-3px
    }
    int r = random(3);
    if (r == 0) { // pink/red
      c.color1 = ui.tft.color565(200 + random(55), 60 + random(40), 80 + random(40));
      c.color2 = ui.tft.color565(255, 140 + random(60), 160 + random(40));
    } else if (r == 1) { // orange
      c.color1 = ui.tft.color565(200 + random(55), 100 + random(40), 30 + random(30));
      c.color2 = ui.tft.color565(255, 180 + random(40), 80 + random(40));
    } else { // purple
      c.color1 = ui.tft.color565(140 + random(40), 50 + random(40), 180 + random(55));
      c.color2 = ui.tft.color565(200 + random(40), 120 + random(40), 255);
    }
  }
}

void updateCoral(float dt) {
  for (int i = 0; i < NUM_CORAL; i++) {
    corals[i].phase += dt * corals[i].speed;
    if (corals[i].phase > 6.2832f) corals[i].phase -= 6.2832f;
  }
}

void drawCoral() {
  int yEnd = stripY + STRIP_H;
  for (int i = 0; i < NUM_CORAL; i++) {
    Coral& c = corals[i];
    float bx = c.x, by = c.baseY;

    // Small mound base
    int baseR = 4 + c.numArms;
    int baseY = (int)by - 2;
    if (baseY + 4 >= stripY && baseY < yEnd)
      fb.fillRect((int)bx - baseR / 2, baseY - stripY, baseR, 4, c.color1);

    // Each arm grows outward as a curved tendril
    for (int a = 0; a < c.numArms; a++) {
      float angle = c.armAngle[a];
      float curve = c.armCurve[a];
      int len = c.armLen[a];
      int thick = c.armThick[a];

      // Trace the arm segment by segment
      float px = bx, py = by - 4;
      float dir = angle;
      for (int s = 0; s < len; s++) {
        // Animate gentle sway that increases toward tip
        float sway = sin(c.phase * 1.2f + s * 0.5f + a * 1.7f) * (0.3f + s * 0.15f);
        dir = angle + curve * s + sway * 0.3f;
        // Clamp so arms always point upward (between -90° and +90°)
        if (dir < -1.4f) dir = -1.4f;
        if (dir > 1.4f) dir = 1.4f;

        float dx = sin(dir) * 5.0f;
        float dy = -cos(dir) * 5.0f; // grow upward

        float nx = px + dx;
        float ny = py + dy;

        int sx = (int)nx, sy = (int)ny;
        int w = max(1, thick - s / 3); // taper toward tip

        // Alternate colors for organic look
        uint16_t col = (s % 2 == 0) ? c.color1 : c.color2;
        // Draw segment as a small rect connecting prev to current
        if (sy + 6 >= stripY && sy - 2 < yEnd)
          fb.fillRect(sx - w / 2, sy - stripY, w, 5, col);

        // Tip: small bulb
        if (s == len - 1) {
          int tipWave = (int)(sin(c.phase * 2.0f + a * 2.3f) * 1.5f);
          if (sy - 3 + tipWave + 3 >= stripY && sy - 3 + tipWave < yEnd)
            fb.fillRect(sx - 1, sy - 3 + tipWave - stripY, 3, 3, c.color2);
        }

        px = nx;
        py = ny;
      }
    }
  }
}

// --- BUBBLES ---
void spawnBubble(float x, float y) {
  for (int i = 0; i < MAX_BUBBLES; i++) {
    if (!bubbles[i].active) {
      bubbles[i].x = x + random(-5, 5);
      bubbles[i].y = y;
      bubbles[i].vx = (random(100) - 50) / 200.0f;
      bubbles[i].r = 1 + random(3);
      bubbles[i].active = true;
      return;
    }
  }
}

void updateBubbles(float dt) {
  for (int i = 0; i < MAX_BUBBLES; i++) {
    if (!bubbles[i].active) continue;
    bubbles[i].y -= 30.0f * dt;
    bubbles[i].x += bubbles[i].vx + sin(bubbles[i].y * 0.05f) * 0.3f;
    if (bubbles[i].y < -5) bubbles[i].active = false;
  }
}

void drawBubbles() {
  int yEnd = stripY + STRIP_H;
  for (int i = 0; i < MAX_BUBBLES; i++) {
    if (!bubbles[i].active) continue;
    int bx = (int)bubbles[i].x;
    int by = (int)bubbles[i].y;
    if (by - bubbles[i].r >= yEnd || by + bubbles[i].r < stripY) continue;
    fb.drawCircle(bx, by - stripY, bubbles[i].r, CLR_BUBBLE);
    if (bubbles[i].r >= 2)
      fb.drawPixel(bx - 1, by - 1 - stripY, CLR_BUBBLE_HI);
  }
}

// --- CREATURE MANAGEMENT ---
uint16_t randomFishColor() {
  switch (random(8)) {
    case 0: return ui.tft.color565(255, 100, 30);
    case 1: return ui.tft.color565(255, 200, 0);
    case 2: return ui.tft.color565(80, 180, 255);
    case 3: return ui.tft.color565(255, 80, 80);
    case 4: return ui.tft.color565(100, 255, 100);
    case 5: return ui.tft.color565(255, 130, 200);
    case 6: return ui.tft.color565(200, 100, 255);
    default: return ui.tft.color565(255, 255, 100);
  }
}

uint16_t accentColor(uint16_t base) {
  int r = ((base >> 11) & 0x1F) << 3;
  int g = ((base >> 5) & 0x3F) << 2;
  int b = (base & 0x1F) << 3;
  return ui.tft.color565(min(255, r + 60), min(255, g + 60), min(255, b + 60));
}

void spawnCreature(int idx, CreatureType type) {
  Creature& c = creatures[idx];
  c.type = type;
  c.active = true;
  c.dir = (random(2) == 0) ? 1 : -1;
  c.animPhase = random(1000) / 100.0f;
  c.state = 0;
  switch (type) {
    case FISH_SMALL:
      c.x = 20 + random(200);
      c.y = 30 + random(150);
      c.vx = (0.6f + random(100) / 100.0f) * c.dir;
      c.vy = 0;
      c.animSpeed = 6.0f + random(40) / 10.0f;
      c.color1 = randomFishColor();
      c.color2 = accentColor(c.color1);
      break;
    case FISH_MED:
      c.x = 20 + random(200);
      c.y = 40 + random(120);
      c.vx = (0.4f + random(60) / 100.0f) * c.dir;
      c.vy = 0;
      c.animSpeed = 4.0f + random(30) / 10.0f;
      c.color1 = randomFishColor();
      c.color2 = accentColor(c.color1);
      break;
    case FISH_LARGE:
      c.x = 25 + random(190);
      c.y = 50 + random(100);
      c.vx = (0.2f + random(30) / 100.0f) * c.dir;
      c.vy = 0;
      c.animSpeed = 3.0f;
      c.color1 = randomFishColor();
      c.color2 = accentColor(c.color1);
      break;
    case JELLYFISH:
      c.x = 30 + random(180);
      c.y = 5;
      c.vx = (random(100) - 50) / 200.0f;
      c.vy = 0.3f + random(30) / 100.0f;
      c.dir = 1;
      c.animSpeed = 2.0f;
      c.color1 = ui.tft.color565(180, 100, 255);
      c.color2 = ui.tft.color565(255, 180, 255);
      break;
    case CRAB:
      c.x = 20 + random(200);
      c.y = SAND_Y - 2;
      c.vx = (0.2f + random(30) / 100.0f) * c.dir;
      c.vy = 0;
      c.animSpeed = 5.0f;
      c.color1 = ui.tft.color565(200, 60, 30);
      c.color2 = ui.tft.color565(255, 120, 60);
      break;
    case SEAHORSE:
      c.x = 20 + random(200);
      c.y = 40 + random(120);
      c.vx = (0.15f + random(15) / 100.0f) * c.dir;
      c.vy = (random(2) == 0) ? (0.15f + random(10) / 100.0f) : -(0.15f + random(10) / 100.0f);
      c.animSpeed = 2.5f;
      c.color1 = ui.tft.color565(255, 180, 50);
      c.color2 = ui.tft.color565(255, 220, 100);
      break;
  }
}

void updateCreatures(float dt) {
  for (int i = 0; i < MAX_CREATURES; i++) {
    if (!creatures[i].active) continue;
    Creature& c = creatures[i];
    c.animPhase += dt * c.animSpeed;
    if (c.animPhase > 6.2832f) c.animPhase -= 6.2832f;
    float speed = dt * 30.0f;
    c.x += c.vx * speed;
    c.y += c.vy * speed;
    if (c.type == FISH_SMALL || c.type == FISH_MED || c.type == FISH_LARGE) {
      c.y += sin(c.animPhase * 0.5f) * 0.15f;
      if (random(500) == 0) spawnBubble(c.x + c.dir * 5, c.y - 3);
      // Random direction flip (~once every 8-15 seconds at 30fps)
      if (random(300) == 0) {
        c.dir = -c.dir;
        c.vx = -c.vx;
      }
      // Randomly adjust diagonal angle (~every 3-5 seconds)
      if (random(120) == 0) {
        c.vy = (random(100) - 50) / 200.0f; // slight up/down drift
      }
      // Clamp vertical position to stay in water
      if (c.y < 20) { c.y = 20; c.vy = max(0.05f, fabs(c.vy)); }
      if (c.y > SAND_Y - 15) { c.y = SAND_Y - 15; c.vy = -max(0.05f, fabs(c.vy)); }
    }
    if (c.type == JELLYFISH) {
      if (c.state == 0) { c.vy = 0.15f; if (c.y > SAND_Y - 40) { c.y = SAND_Y - 40; c.state = 1; } }
      else { c.vy = -0.4f; if (c.y < 5) { c.y = 5; c.state = 0; } }
      c.x += sin(c.animPhase * 0.3f) * 0.2f;
      if (random(200) == 0) spawnBubble(c.x, c.y - 10);
    }
    if (c.type == SEAHORSE) {
      if (c.y < 20) { c.y = 20; c.vy = max(0.05f, fabs(c.vy)); }
      if (c.y > SAND_Y - 30) { c.y = SAND_Y - 30; c.vy = -max(0.05f, fabs(c.vy)); }
    }
    if (c.type == CRAB) {
      c.y = SAND_Y - 2;
    }
    // Boundary bouncing: flip direction at screen edges instead of deactivating
    if (c.type == FISH_SMALL || c.type == FISH_MED || c.type == FISH_LARGE || c.type == CRAB) {
      if (c.x < 5 && c.vx < 0) { c.vx = -c.vx; c.dir = 1; }
      else if (c.x > SW - 5 && c.vx > 0) { c.vx = -c.vx; c.dir = -1; }
    }
    if (c.type == JELLYFISH) {
      if (c.x < 10) c.vx = 0.15f;
      else if (c.x > SW - 10) c.vx = -0.15f;
    }
    if (c.type == SEAHORSE) {
      if (c.x < 10 && c.vx < 0) { c.vx = -c.vx; c.dir = 1; }
      else if (c.x > SW - 10 && c.vx > 0) { c.vx = -c.vx; c.dir = -1; }
    }
  }
}

void drawCreatures() {
  for (int i = 0; i < MAX_CREATURES; i++) {
    if (!creatures[i].active) continue;
    Creature& c = creatures[i];
    int ix = (int)c.x, iy = (int)c.y;
    switch (c.type) {
      case FISH_SMALL: drawFishSmall(ix, iy, c.dir, c.color1, c.color2, c.animPhase); break;
      case FISH_MED:   drawFishMed(ix, iy, c.dir, c.color1, c.color2, c.animPhase); break;
      case FISH_LARGE: drawFishLarge(ix, iy, c.dir, c.color1, c.color2, c.animPhase); break;
      case JELLYFISH:  drawJellyfish(ix, iy, c.color1, c.color2, c.animPhase); break;
      case CRAB:       drawCrab(ix, iy, c.dir, c.color1, c.color2, c.animPhase); break;
      case SEAHORSE:   drawSeahorse(ix, iy, c.dir, c.color1, c.color2, c.animPhase); break;
    }
  }
}

// --- SPAWN ALL ---
void spawnAllCreatures() {
  for (int i = 0; i < MAX_CREATURES; i++) creatures[i].active = false;
  // 3 small fish, 2 medium, 1 large, 1 jellyfish, 1 crab, 1 seahorse = 9
  spawnCreature(0, FISH_SMALL);
  spawnCreature(1, FISH_SMALL);
  spawnCreature(2, FISH_SMALL);
  spawnCreature(3, FISH_MED);
  spawnCreature(4, FISH_MED);
  spawnCreature(5, FISH_LARGE);
  spawnCreature(6, JELLYFISH);
  spawnCreature(7, CRAB);
  spawnCreature(8, SEAHORSE);
}

// --- SETUP & LOOP ---

void setup() {
  Serial.begin(460800);
  Serial.println("Starting aquarium app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // 240x80 strip at 16-bit = 38,400 bytes — fits comfortably
  fb.setColorDepth(16);
  fb.createSprite(SW, STRIP_H);

  CLR_BUBBLE = fb.color565(140, 200, 255);
  CLR_BUBBLE_HI = fb.color565(200, 235, 255);

  randomSeed(esp_random());
  for (int i = 0; i < MAX_BUBBLES; i++) bubbles[i].active = false;
  for (int i = 0; i < MAX_CREATURES; i++) creatures[i].active = false;

  assignPlantPositions();
  initWeeds();
  initKelp();
  initCoral();
  spawnAllCreatures();

  lastFrame = millis();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      themeIndex = (themeIndex + 1) % NUM_THEMES;
      assignPlantPositions();
      initWeeds();
      initKelp();
      initCoral();
      spawnAllCreatures();
      Serial.printf("Theme: %d\n", themeIndex);
    }
  }
  buttonWasPressed = pressed;

  unsigned long now = millis();
  float dt = (now - lastFrame) / 1000.0f;
  if (dt < 0.033f) return;
  lastFrame = now;

  if (now - lastBubble > 800) {
    int wi = random(NUM_WEEDS);
    spawnBubble(weeds[wi].x, weeds[wi].baseY - weeds[wi].segments * 6);
    lastBubble = now;
  }

  // Update simulation once
  updateWeeds(dt);
  updateKelp(dt);
  updateCoral(dt);
  updateBubbles(dt);
  updateCreatures(dt);

  // Render 3 strips, push each immediately — flicker-free
  for (int s = 0; s < NUM_STRIPS; s++) {
    stripY = s * STRIP_H;
    drawBackground();
    drawWeeds();
    drawKelp();
    drawCoral();
    drawCreatures();
    drawBubbles();
    fb.pushSprite(0, stripY);
  }
}
