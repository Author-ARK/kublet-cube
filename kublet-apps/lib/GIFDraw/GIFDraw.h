#ifndef GIFDRAW_H
#define GIFDRAW_H

#include <AnimatedGIF.h>
#include <TFT_eSPI.h>

// Call once before using GIFDraw to set the render target
void GIFDrawSetTFT(TFT_eSPI *display);

// AnimatedGIF frame callback — renders scanlines to TFT
void GIFDraw(GIFDRAW *pDraw);

#endif // GIFDRAW_H
