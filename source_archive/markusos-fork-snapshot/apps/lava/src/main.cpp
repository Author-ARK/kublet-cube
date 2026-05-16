#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;

// Render at low resolution and scale up for blobby look
#define GRID_W 48
#define GRID_H 48
#define SCALE  5   // 48*5 = 240

// Number of metaballs
#define NUM_BALLS 6

// Color palette index
int paletteIndex = 0;
bool buttonWasPressed = false;

struct Metaball {
  float x, y;     // position in grid coords
  float vx, vy;   // velocity
  float r;         // radius (influence strength)
};

Metaball balls[NUM_BALLS];

// Color palettes: each has 4 color stops that blend smoothly
struct Palette {
  uint8_t r0, g0, b0;  // background / low influence
  uint8_t r1, g1, b1;  // mid-low
  uint8_t r2, g2, b2;  // mid-high
  uint8_t r3, g3, b3;  // hot / high influence
};

const Palette palettes[] = {
  // Classic lava: dark red -> orange -> yellow -> white
  {20, 0, 5,   180, 20, 0,   255, 120, 0,   255, 240, 80},
  // Blue lava: deep blue -> cyan -> light blue -> white
  {5, 0, 30,   0, 40, 180,   0, 140, 255,   180, 230, 255},
  // Green slime: dark green -> lime -> yellow -> white
  {0, 15, 5,   0, 120, 20,   80, 220, 0,    200, 255, 100},
  // Purple plasma: dark purple -> magenta -> pink -> white
  {15, 0, 25,  120, 0, 160,  255, 50, 150,  255, 180, 255},
};
#define NUM_PALETTES 4

uint16_t colorFromPalette(float val, const Palette& p) {
  // val: 0..1 mapped across 4 color stops
  if (val < 0) val = 0;
  if (val > 1) val = 1;

  float t;
  uint8_t r, g, b;

  if (val < 0.33f) {
    t = val / 0.33f;
    r = p.r0 + (p.r1 - p.r0) * t;
    g = p.g0 + (p.g1 - p.g0) * t;
    b = p.b0 + (p.b1 - p.b0) * t;
  } else if (val < 0.66f) {
    t = (val - 0.33f) / 0.33f;
    r = p.r1 + (p.r2 - p.r1) * t;
    g = p.g1 + (p.g2 - p.g1) * t;
    b = p.b1 + (p.b2 - p.b1) * t;
  } else {
    t = (val - 0.66f) / 0.34f;
    r = p.r2 + (p.r3 - p.r2) * t;
    g = p.g2 + (p.g3 - p.g2) * t;
    b = p.b2 + (p.b3 - p.b2) * t;
  }

  return ui.tft.color565(r, g, b);
}

void initBalls() {
  for (int i = 0; i < NUM_BALLS; i++) {
    balls[i].x = random(5, GRID_W - 5);
    balls[i].y = random(5, GRID_H - 5);
    balls[i].vx = (random(100) - 50) / 200.0f;
    balls[i].vy = (random(100) - 50) / 200.0f;
    balls[i].r = random(40, 80) / 10.0f; // 4.0 - 8.0
  }
}

void updateBalls() {
  for (int i = 0; i < NUM_BALLS; i++) {
    balls[i].x += balls[i].vx;
    balls[i].y += balls[i].vy;

    // Bounce off edges with some padding
    if (balls[i].x < 2 || balls[i].x > GRID_W - 2) {
      balls[i].vx = -balls[i].vx;
      balls[i].x = constrain(balls[i].x, 2, GRID_W - 2);
    }
    if (balls[i].y < 2 || balls[i].y > GRID_H - 2) {
      balls[i].vy = -balls[i].vy;
      balls[i].y = constrain(balls[i].y, 2, GRID_H - 2);
    }

    // Slight gravity pull downward (lava lamp effect)
    balls[i].vy += 0.002f;

    // Dampen and add slight randomness for organic movement
    balls[i].vx *= 0.999f;
    balls[i].vy *= 0.999f;
    balls[i].vx += (random(100) - 50) / 5000.0f;
    balls[i].vy += (random(100) - 50) / 5000.0f;

    // Clamp velocity
    balls[i].vx = constrain(balls[i].vx, -0.4f, 0.4f);
    balls[i].vy = constrain(balls[i].vy, -0.4f, 0.4f);
  }
}

void renderFrame() {
  const Palette& pal = palettes[paletteIndex];

  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      // Sum metaball influence at this point
      float sum = 0;
      for (int i = 0; i < NUM_BALLS; i++) {
        float dx = gx - balls[i].x;
        float dy = gy - balls[i].y;
        float distSq = dx * dx + dy * dy;
        if (distSq < 0.1f) distSq = 0.1f;
        sum += (balls[i].r * balls[i].r) / distSq;
      }

      // Map influence to 0..1 range
      // Threshold around 1.0 gives nice blob boundaries
      float val = (sum - 0.3f) / 2.0f;

      uint16_t color = colorFromPalette(val, pal);

      // Draw scaled pixel
      int sx = gx * SCALE;
      int sy = gy * SCALE;
      ui.tft.fillRect(sx, sy, SCALE, SCALE, color);
    }
  }
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting lava lamp app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ui.tft.fillScreen(TFT_BLACK);

  randomSeed(esp_random());
  initBalls();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  // Button: cycle palette
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      paletteIndex = (paletteIndex + 1) % NUM_PALETTES;
      Serial.printf("Palette: %d\n", paletteIndex);
    }
  }
  buttonWasPressed = pressed;

  updateBalls();
  renderFrame();
}
