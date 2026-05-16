// Based off https://github.com/bitbank2/AnimatedGIF/blob/master/examples/TFT_eSPI_memory/GIFDraw.ino

#include "GIFDraw.h"
#include <cstdint>

#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 240
#define BUFFER_SIZE 240

static TFT_eSPI *_tft = nullptr;
static uint16_t usTemp[BUFFER_SIZE];

void GIFDrawSetTFT(TFT_eSPI *display) {
  _tft = display;
}

// Draw a line of image directly on the LCD
void GIFDraw(GIFDRAW *pDraw)
{
  uint8_t *s;
  uint16_t *d, *usPalette;
  int x, y, iWidth;

  if (!_tft) return;

  // Display bounds check and cropping
  iWidth = pDraw->iWidth;
  if (iWidth + pDraw->iX > DISPLAY_WIDTH)
    iWidth = DISPLAY_WIDTH - pDraw->iX;
  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y; // current line
  if (y >= DISPLAY_HEIGHT || pDraw->iX >= DISPLAY_WIDTH || iWidth < 1)
    return;

  // Old image disposal
  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) // restore to background color
  {
    for (x = 0; x < iWidth; x++)
    {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }

  // Translate 8-bit pixels through the RGB565 palette
  s = pDraw->pPixels;
  d = usTemp;
  for (x = 0; x < iWidth; x++)
    *d++ = usPalette[*s++];

  _tft->setAddrWindow(pDraw->iX, y, iWidth, 1);
  _tft->pushPixels(usTemp, iWidth);
}
