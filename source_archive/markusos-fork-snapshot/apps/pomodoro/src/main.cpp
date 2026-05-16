#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;

// Timer config
#define WORK_MINUTES 25
#define BREAK_MINUTES 5

// Colors
#define CLR_BG       TFT_BLACK
#define CLR_WORK     0x34DF   // bright blue
#define CLR_BREAK    0x07E0   // green
#define CLR_RING_BG  0x2104   // dark gray
#define CLR_TEXT     TFT_WHITE
#define CLR_LABEL    0x7BEF   // gray

// Display geometry
#define CX 120
#define CY 120
#define RING_R_OUTER 110
#define RING_R_INNER 92

// Timer state
enum TimerState { STOPPED, WORK, BREAK_TIME, TRANSITION };
TimerState state = STOPPED;
unsigned long phaseStartMs = 0;
int phaseDurationMs = 0;
int prevMinutesLeft = -1;
int prevProgressDeg = -1;
TimerState prevState = STOPPED;

// Transition state
TimerState nextPhase = WORK;       // which phase starts after transition
unsigned long transitionStartMs = 0;
int prevBlinkState = -1;           // tracks blink on/off to avoid redundant draws

// Button
bool buttonWasPressed = false;

// Draw a full ring in one color (used for initial draw and reset)
void drawFullRing(uint16_t color) {
  ui.tft.drawSmoothArc(CX, CY, RING_R_OUTER, RING_R_INNER, 0, 360, color, CLR_BG, true);
}

// Incrementally convert one degree from color to gray
// Only redraws a small slice around the new degree, no full ring redraw
// TFT_eSPI: 0=6 o'clock (bottom), clockwise. 12 o'clock = 180.
void advanceGray(int prevDeg, int newDeg) {
  for (int d = prevDeg; d < newDeg; d++) {
    int tftAngle = (180 + d) % 360;
    int sliceStart = tftAngle;
    int sliceEnd = (tftAngle + 1) % 360;
    // Draw a 1-degree gray arc without rounded ends to avoid flicker
    if (sliceEnd > sliceStart) {
      ui.tft.drawSmoothArc(CX, CY, RING_R_OUTER, RING_R_INNER, sliceStart, sliceEnd, CLR_RING_BG, CLR_BG, false);
    } else {
      // Wraps around 360/0
      ui.tft.drawSmoothArc(CX, CY, RING_R_OUTER, RING_R_INNER, sliceStart, 360, CLR_RING_BG, CLR_BG, false);
      if (sliceEnd > 0)
        ui.tft.drawSmoothArc(CX, CY, RING_R_OUTER, RING_R_INNER, 0, sliceEnd, CLR_RING_BG, CLR_BG, false);
    }
  }
}

void drawCentered(const char* text, const tftfont_t& font, uint16_t color, int y, int ySpan = 0) {
  ui.tft.setTTFFont(font);
  if (ySpan > 0) ui.tft.fillRect(CX - 70, y - 2, 140, ySpan, CLR_BG);
  // Use same fg/bg to prevent TTF renderer from drawing its own background
  // fills (which extend beyond our fillRect and cause artifacts)
  ui.tft.setTextColor(color, color);
  int w = ui.tft.TTFtextWidth(text);
  ui.tft.setCursor((240 - w) / 2, y);
  ui.tft.print(text);
}

void drawMinutes(int minutesLeft) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", minutesLeft);
  drawCentered(buf, Arial_48_Bold, CLR_TEXT, 95, 52);
}

void drawLabel(TimerState st) {
  uint16_t color;
  const char* label;
  if (st == STOPPED) {
    label = "READY";
    color = CLR_LABEL;
  } else if (st == WORK) {
    label = "WORK";
    color = CLR_WORK;
  } else {
    label = "BREAK";
    color = CLR_BREAK;
  }
  drawCentered(label, Arial_14_Bold, color, 148, 22);
}

void startPhase(TimerState newState) {
  state = newState;
  phaseDurationMs = (newState == WORK) ? WORK_MINUTES * 60000 : BREAK_MINUTES * 60000;
  phaseStartMs = millis();
  prevMinutesLeft = -1;
  prevProgressDeg = -1;
  prevState = STOPPED; // force full redraw

  // Full redraw for new phase
  int totalMin = (newState == WORK) ? WORK_MINUTES : BREAK_MINUTES;
  drawFullRing((newState == WORK) ? CLR_WORK : CLR_BREAK);
  drawMinutes(totalMin);
  drawLabel(newState);
  prevMinutesLeft = totalMin;
  prevState = newState;
  prevProgressDeg = 0;
}

void stopTimer() {
  state = STOPPED;
  prevMinutesLeft = -1;
  prevProgressDeg = -1;
  prevState = STOPPED;
  drawFullRing(CLR_RING_BG);
  drawMinutes(WORK_MINUTES);
  drawLabel(STOPPED);
  prevMinutesLeft = WORK_MINUTES;
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting pomodoro app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ui.tft.fillScreen(CLR_BG);

  // Draw initial stopped state
  drawFullRing(CLR_RING_BG);
  drawMinutes(WORK_MINUTES);
  drawLabel(STOPPED);
  prevMinutesLeft = WORK_MINUTES;
  prevState = STOPPED;
  prevProgressDeg = 0;
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  // Button handling
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50); // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (state == STOPPED) {
        Serial.println("Starting work phase");
        startPhase(WORK);
      } else {
        // Stop from any active state (WORK, BREAK_TIME, or TRANSITION)
        Serial.println("Timer stopped");
        stopTimer();
      }
    }
  }
  buttonWasPressed = pressed;

  // Transition blink logic
  if (state == TRANSITION) {
    unsigned long elapsed = millis() - transitionStartMs;

    if (elapsed >= 3000) {
      // Transition done, start next phase
      Serial.printf("Transition done, starting %s\n", nextPhase == WORK ? "work" : "break");
      startPhase(nextPhase);
      return;
    }

    // Blink: 0-500ms on, 500-1000ms off, 1000-1500ms on, etc.
    int blinkState = ((int)(elapsed / 500)) % 2; // 0=on, 1=off
    if (blinkState != prevBlinkState) {
      const char* label = (nextPhase == WORK) ? "WORK" : "BREAK";
      uint16_t color = (nextPhase == WORK) ? CLR_WORK : CLR_BREAK;
      if (blinkState == 0) {
        drawCentered(label, Arial_14_Bold, color, 148, 22);
      } else {
        // Clear label area
        ui.tft.fillRect(CX - 70, 146, 140, 22, CLR_BG);
      }
      prevBlinkState = blinkState;
    }
  }

  // Timer logic
  if (state == WORK || state == BREAK_TIME) {
    unsigned long elapsed = millis() - phaseStartMs;

    // Phase complete? Start transition to next phase
    if (elapsed >= (unsigned long)phaseDurationMs) {
      nextPhase = (state == WORK) ? BREAK_TIME : WORK;
      state = TRANSITION;
      transitionStartMs = millis();
      prevBlinkState = -1;

      // Clear minutes, show 0, and set ring to full gray
      drawMinutes(0);
      drawFullRing(CLR_RING_BG);
      // Clear old label so blink starts clean
      ui.tft.fillRect(CX - 70, 146, 140, 22, CLR_BG);
      Serial.printf("Phase done, transitioning to %s\n", nextPhase == WORK ? "work" : "break");
      return;
    }

    // Calculate progress
    float progress = (float)elapsed / phaseDurationMs;
    if (progress > 1.0f) progress = 1.0f;
    int progressDeg = (int)(progress * 360);

    // Calculate minutes left (ceiling)
    int msLeft = phaseDurationMs - (int)elapsed;
    if (msLeft < 0) msLeft = 0;
    int minutesLeft = (msLeft + 59999) / 60000;

    // Update ring incrementally — only draw newly elapsed degrees
    if (progressDeg != prevProgressDeg && progressDeg > prevProgressDeg) {
      advanceGray(prevProgressDeg, progressDeg);
      prevProgressDeg = progressDeg;
    }

    // Update minutes text only when it changes
    if (minutesLeft != prevMinutesLeft) {
      drawMinutes(minutesLeft);
      drawLabel(state);  // redraw label (TTF background fill overlaps it)
      prevMinutesLeft = minutesLeft;
    }
  }

  delay(200);
}
