#include <TFT_eSPI.h>

#include "kgfx.h"

/***************************************************************************************
** Function name:           init
** Description:             Initializes GFX library
***************************************************************************************/
void KGFX::init() {
  tft.begin();
  palette[0] = TFT_BLACK;
}

/***************************************************************************************
** Function name:           clear
** Description:             Paints screen black
***************************************************************************************/
void KGFX::clear() {
  t.fillScreen(TFT_BLACK);
  tft.fillScreen(TFT_BLACK);
}

/***************************************************************************************
** Function name:           createSprite
** Description:             Creates sprite with given width and height
***************************************************************************************/
TFT_eSprite KGFX::createSprite(int width, int height) {
  TFT_eSprite spr = TFT_eSprite(&tft);
  spr.setColorDepth(16);
  spr.createSprite(width, height);
  return spr;
}

/***************************************************************************************
** Function name:           createSpriteLarge
** Description:             Creates sprite with given width and height
***************************************************************************************/
TFT_eSprite KGFX::createSpriteLarge(int width, int height) {
  TFT_eSprite spr = TFT_eSprite(&tft);
  // On an ESP32 the workspace RAM is more limited than the datasheet implies so a
  // 16-bit colour Sprite is limited to about 200x200 pixels (~80Kbytes), an 8-bit
  // sprite to 320x240 pixels (~76kbytes)
  // https://github.com/Bodmer/TFT_eSPI?tab=readme-ov-file#sprites
  spr.setColorDepth(8);
  spr.createSprite(width, height);
  return spr;
}

/***************************************************************************************
** Function name:           drawText
** Description:             Draws text to given sprite
***************************************************************************************/
void KGFX::drawText(TFT_eSprite &spr, const char *txt, const tftfont_t &f, int color, int x, int y) {
  tft.TTFdestination(&spr);
  spr.fillSprite(TFT_BLACK);

  tft.setTTFFont(f);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(0,0);
  tft.print(txt);

  spr.pushSprite(x, y);
}

/***************************************************************************************
 * Function name:           drawTextCenter
 * Description:             Draws text to given sprite centered
 ***************************************************************************************/

void KGFX::drawTextCenter(TFT_eSprite &spr, const char *txt, const tftfont_t &f, int color, int y) {
  tft.TTFdestination(&spr);
  spr.fillSprite(TFT_BLACK);

  tft.setTTFFont(f);
  tft.setTextColor(color, TFT_BLACK);

  int w = tft.TTFtextWidth(txt); 
  int x1 = (spr.width() - w)/2; // center text in sprite
  int x2 = (tft.width() - spr.width())/2; // center sprite on screen

  tft.setCursor(x1, 0);
  tft.print(txt);

  spr.pushSprite(x2, y);
}

/***************************************************************************************
** Function name:           drawText
** Description:             Draws text to screen
***************************************************************************************/
void KGFX::drawText(const char *txt, const tftfont_t &f, int color, int x, int y) {
  tft.setTTFFont(f);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x,y);
  tft.print(txt);
}

/***************************************************************************************
 * Function name:           drawTextCenter
 * Description:             Draws text to screen centered
 ***************************************************************************************/

void KGFX::drawTextCenter(const char *txt, const tftfont_t &f, int color, int y) {
  tft.setTTFFont(f);
  tft.setTextColor(color, TFT_BLACK);
  int x = tft.width() / 2 - tft.TTFtextWidth(txt) / 2;
  tft.setCursor(x, y);
  tft.print(txt);
}

/***************************************************************************************
** Function name:           createChartSprite
** Description:             Creates chart sprite
***************************************************************************************/
void KGFX::createChartSprite() {
  chartSpr.setColorDepth(16);
  chartSpr.createSprite(240,80);
}

/***************************************************************************************
** Function name:           createChartSpriteLarge
** Description:             Creates chart sprite with 8 bit colors
***************************************************************************************/
void KGFX::createChartSpriteLarge(int x, int y) {
  chartSpr.setColorDepth(8);
  chartSpr.createSprite(x,y);
}

/***************************************************************************************
** Function name:           deleteSprite
** Description:             Deletes given sprite
***************************************************************************************/
void KGFX::deleteSprite(TFT_eSprite &spr) {
  spr.deleteSprite();
}

/***************************************************************************************
** Function name:           deleteChartSprite
** Description:             Deletes chart sprite
***************************************************************************************/
void KGFX::deleteChartSprite() {
  chartSpr.deleteSprite();
}

/***************************************************************************************
** Function name:           drawChart
** Description:             Draws default chart size to sprite
***************************************************************************************/
void KGFX::drawChart(std::vector<float> arr, int color, int y, int spacing, int height) {
  tft.TTFdestination(&chartSpr);
  chartSpr.fillSprite(TFT_BLACK);

  createPalette(color);
  chartSpr.createPalette(palette);

  int *fa = fmtChartArray(arr, height);
  for (int i=0;i<(chartLen-2);i++) {
    drawGraphLine(i*spacing, fa[i+1], (i+1)*spacing, fa[i+2], color);
  }

  int multi = 5;
  if (height>80) {
    multi = height/13;
  }
  for (int i=0;i<240;i++) {
    for (int j=0;j<height;j++) {
      if (TFT_BLACK == chartSpr.readPixel(i, j) && (j>0 && TFT_BLACK != chartSpr.readPixel(i, j-1))) {
        drawVGradient(i, j, multi);
        break;
      }
    }
  }

  chartSpr.pushSprite(0, y);
}

/***************************************************************************************
** Function name:           drawChartWide
** Description:             Draws default chart size that fills entire width to sprite
***************************************************************************************/
void KGFX::drawChartWide(std::vector<float> arr, int color, int y) {
  drawChart(arr, color, y, 8);
}

/***************************************************************************************
** Function name:           drawChartLarge
** Description:             Draws wide chart that default to 120 pixel height chart
***************************************************************************************/
void KGFX::drawChartLarge(std::vector<float> arr, int color, int y, int height) {
  drawChart(arr, color, y, 8, height);
}

void KGFX::createPalette(int color) {
  if (color == K_GREEN) {
    for (int i=0;i<15;i++) {
      palette[i+1] = green_palette[i];
    }
    return;
  }
  if (color == K_RED) {
    for (int i=0;i<15;i++) {
      palette[i+1] = red_palette[i];
    }
    return;
  }
}

void KGFX::drawGraphLine(int x, int y, int x1, int y1, int pcolor) {
  for (int i=0;i<3;i++) {
    chartSpr.drawLine(x, y, x1, y1, pcolor);

    y++; y1++;
  }
}

void KGFX::drawVGradient(int x, int y, int y1) {
  int off = abs(y1 - y);

  for (int i=3;i<=15;i++) {
    for (int j = 0; j < y1; j++) {
      if (off-- && off > 0) {
        continue;
      }

      if (y1 > 5) {
          chartSpr.drawPixel(x, y, palette[8]);
      } else {
          chartSpr.drawPixel(x, y, palette[i]);
      }
      y++;
    }
  }
}

int* KGFX::fmtChartArray(std::vector<float> arr, int height) {
  int multi = 1;
  if (arr.size() < 30) {
    Serial.println("Malformed array len: cannot fmt");
    return nullptr;
  }

  float v = arr[0];
  if (v >= 1000) {
  } else {
    if (v >= 10 && v < 1000) {
      multi = 100;
    } else {
      multi = 10000;
    }
  }

  int hi = arr[0] * multi;
  int lo = arr[0] * multi;

  for(int i=1;i<chartLen;i++) {
    int v = arr[i] * multi;
    if (v > hi) {
      hi = v;
    }
    if (v < lo) {
      lo = v;
    }
  }

  int h = 50;
  if (height > 80) {
    h = (height/5)*4;
  }

  int diff = hi - lo;
  for(int i=0;i<chartLen;i++) {
    int v = arr[i] * multi;
    fa[i] = h-round(((v-lo) *h)/diff);
  }

  return fa;
}
