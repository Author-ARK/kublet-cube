#pragma once

#include <SPI.h>
#include <TFT_eSPI.h>
#include <vector>

#include "TFT_eSPI_ext.h"
#include "font_Arial.h"
#include "font_ArialBold.h"

#define K_GREEN TFT_GREEN
#define K_RED TFT_RED

class KGFX {
  private:
    TFT_eSPI t = TFT_eSPI();

    uint16_t red_palette[15] =  {K_RED, 0x5000, 0x5000, 0x4800, 0x4800, 0x4000, 0x4000, 0x3800, 0x3800, 0x3000, 0x3000, 0x2800, 0x2000};
    uint16_t green_palette[15] =  {K_GREEN, 0x02C0, 0x0240, 0x0200, 0x01C0, 0x0180, 0x0140, 0x0100, 0x00E0, 0x00C0, 0x00A0, 0x0080, 0x0060, 0x0040, 0x0020};
    uint16_t palette[16];

    int chartLen = 30;
    int fa[30];

    int* fmtChartArray(std::vector<float> arr, int height=80);
    void createPalette(int color);
    void drawVGradient(int x, int y, int y1=5);
    void drawGraphLine(int x, int y, int x1, int y1, int color);

  public:
    void init();
    void clear();

    TFT_eSPI_ext tft = TFT_eSPI_ext(&t);

    TFT_eSprite chartSpr = TFT_eSprite(&tft);

    TFT_eSprite createSprite(int width, int height);
    TFT_eSprite createSpriteLarge(int width, int height);

    void createChartSprite();
    void createChartSpriteLarge(int x, int y);

    void drawText(TFT_eSprite &spr, const char *txt, const tftfont_t &f, int color, int x, int y);
    void drawTextCenter(TFT_eSprite &spr, const char *txt, const tftfont_t &f, int color, int y);
    void drawText(const char *txt, const tftfont_t &f, int color, int x, int y);
    void drawTextCenter(const char *txt, const tftfont_t &f, int color, int y);

    void deleteSprite(TFT_eSprite &spr);
    void deleteChartSprite();

    void drawChart(std::vector<float> arr, int color, int y, int spacing=7, int height=80);
    void drawChartWide(std::vector<float> arr, int color, int y);
    void drawChartLarge(std::vector<float> arr, int color, int y, int height=120);
};