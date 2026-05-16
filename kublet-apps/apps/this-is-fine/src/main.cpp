#include <Arduino.h>
#include <otaserver.h>
#include <AnimatedGIF.h>
#include <TFT_eSPI.h>

#include "this_is_fine.h"
#include <GIFDraw.h>

#define GIF_IMAGE this_is_fine

OTAServer otaserver;
Preferences preferences;

unsigned long lastTime = 0;
unsigned long timerDelay = 600000;

TFT_eSPI tft = TFT_eSPI();
AnimatedGIF gif;

void setup()
{
  Serial.begin(460800);
  Serial.println("Starting app");

  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  GIFDrawSetTFT(&tft);
  gif.begin(BIG_ENDIAN_PIXELS);
}

void loop()
{
  if ((WiFi.status() == WL_CONNECTED))
  {
    otaserver.handle(); // DO NOT EDIT

    if (gif.open((uint8_t *)GIF_IMAGE, sizeof(GIF_IMAGE), GIFDraw))
    {
      tft.startWrite();
      while (gif.playFrame(false, NULL))
      {
        delay(50);
      }
      gif.close();
      tft.endWrite();
    }
  }
}
