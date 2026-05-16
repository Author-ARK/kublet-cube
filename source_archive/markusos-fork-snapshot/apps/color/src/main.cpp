#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>

// Kublet button is on GPIO 19 (active LOW)
#define BUTTON_PIN 19

OTAServer otaserver;
KGFX ui;

int refreshTimeInSeconds = 300;
unsigned long lastTime = 0;
bool buttonWasPressed = false;

uint16_t randomColor() {
  // Generate a random RGB565 color, avoiding very dark colors
  uint8_t r = random(4, 32);  // 5 bits
  uint8_t g = random(8, 64);  // 6 bits
  uint8_t b = random(4, 32);  // 5 bits
  return (r << 11) | (g << 5) | b;
}

void showColor() {
  uint16_t color = randomColor();
  ui.tft.fillScreen(color);
  Serial.printf("Color: 0x%04X\n", color);
}

void setup() {
  Serial.begin(460800);
  Serial.println("Starting color app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run(); // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  randomSeed(esp_random());

  showColor();
  lastTime = millis();
}

void loop() {
  if ((WiFi.status() == WL_CONNECTED)) {
    otaserver.handle(); // DO NOT EDIT

    // Timer-based refresh
    if ((millis() - lastTime) > (unsigned long)refreshTimeInSeconds * 1000) {
      lastTime = millis();
      showColor();
    }

    // Button press detection (active LOW, with simple debounce)
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);
    if (pressed && !buttonWasPressed) {
      delay(50);  // debounce
      if (digitalRead(BUTTON_PIN) == LOW) {
        Serial.println("Button pressed — changing color");
        showColor();
        lastTime = millis();  // reset timer
      }
    }
    buttonWasPressed = pressed;
  }

  delay(1);
}
