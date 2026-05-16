#include "TFT_eSPI.h"
#include <cmath>
#include <algorithm>

// Undefine Arduino min/max macros to avoid conflicts with std::min/std::max
#undef min
#undef max

// Shared framebuffer for all non-sprite TFT_eSPI instances
uint16_t _emu_shared_fb[240 * 240] = {};

// ===========================================================================
// TFT_eSPI drawing implementations
// ===========================================================================

void TFT_eSPI::drawPixel(int32_t x, int32_t y, uint16_t color) {
  _setPixel(x, y, color);
}

uint16_t TFT_eSPI::readPixel(int32_t x, int32_t y) {
  if (x >= 0 && x < _width && y >= 0 && y < _height)
    return _fb[y * _width + x];
  return 0;
}

void TFT_eSPI::fillScreen(uint16_t color) {
  fillRect(0, 0, _width, _height, color);
}

void TFT_eSPI::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  int32_t x1 = std::max((int32_t)0, x);
  int32_t y1 = std::max((int32_t)0, y);
  int32_t x2 = std::min((int32_t)_width,  x + w);
  int32_t y2 = std::min((int32_t)_height, y + h);
  for (int32_t j = y1; j < y2; j++)
    for (int32_t i = x1; i < x2; i++)
      drawPixel(i, j, color);
}

void TFT_eSPI::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  drawFastHLine(x, y, w, color);
  drawFastHLine(x, y + h - 1, w, color);
  drawFastVLine(x, y, h, color);
  drawFastVLine(x + w - 1, y, h, color);
}

void TFT_eSPI::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  drawFastHLine(x + r, y, w - 2 * r, color);
  drawFastHLine(x + r, y + h - 1, w - 2 * r, color);
  drawFastVLine(x, y + r, h - 2 * r, color);
  drawFastVLine(x + w - 1, y + r, h - 2 * r, color);
  // corners (simple — just draw arcs as quarter circles)
  int32_t cx1 = x + r, cy1 = y + r;
  int32_t cx2 = x + w - r - 1, cy2 = y + h - r - 1;
  // Midpoint circle for corners
  int32_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, px = 0, py = r;
  while (px < py) {
    if (f >= 0) { py--; ddF_y += 2; f += ddF_y; }
    px++; ddF_x += 2; f += ddF_x;
    drawPixel(cx2 + px, cy1 - py, color); drawPixel(cx2 + py, cy1 - px, color);
    drawPixel(cx1 - px, cy1 - py, color); drawPixel(cx1 - py, cy1 - px, color);
    drawPixel(cx2 + px, cy2 + py, color); drawPixel(cx2 + py, cy2 + px, color);
    drawPixel(cx1 - px, cy2 + py, color); drawPixel(cx1 - py, cy2 + px, color);
  }
}

void TFT_eSPI::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (r <= 0) { fillRect(x, y, w, h, color); return; }
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;

  // Fill center rectangle
  fillRect(x + r, y, w - 2 * r, h, color);
  // Fill left and right strips (between corners)
  fillRect(x, y + r, r, h - 2 * r, color);
  fillRect(x + w - r, y + r, r, h - 2 * r, color);

  // Fill four corners using midpoint circle (scanline fill)
  int32_t cx1 = x + r;           // left corner center x
  int32_t cx2 = x + w - 1 - r;   // right corner center x
  int32_t cy1 = y + r;           // top corner center y
  int32_t cy2 = y + h - 1 - r;   // bottom corner center y

  int32_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r;
  int32_t px = 0, py = r;

  // Fill the px==0 scanlines (top and bottom of circle)
  drawFastHLine(cx1 - r, cy1 - 0, 1 + r, color);  // top-left at py offset
  drawFastHLine(cx2, cy1 - 0, 1 + r, color);       // top-right
  drawFastHLine(cx1 - r, cy2 + 0, 1 + r, color);   // bottom-left
  drawFastHLine(cx2, cy2 + 0, 1 + r, color);        // bottom-right

  while (px < py) {
    if (f >= 0) { py--; ddF_y += 2; f += ddF_y; }
    px++; ddF_x += 2; f += ddF_x;

    // Each iteration fills 4 scanlines per corner (8 total for top+bottom)
    // Top-left corner
    drawFastHLine(cx1 - px, cy1 - py, px + 1, color);
    drawFastHLine(cx1 - py, cy1 - px, py + 1, color);
    // Top-right corner
    drawFastHLine(cx2, cy1 - py, px + 1, color);
    drawFastHLine(cx2, cy1 - px, py + 1, color);
    // Bottom-left corner
    drawFastHLine(cx1 - px, cy2 + py, px + 1, color);
    drawFastHLine(cx1 - py, cy2 + px, py + 1, color);
    // Bottom-right corner
    drawFastHLine(cx2, cy2 + py, px + 1, color);
    drawFastHLine(cx2, cy2 + px, py + 1, color);
  }
}

// Bresenham line
void TFT_eSPI::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color) {
  if (x0 == x1) { drawFastVLine(x0, std::min(y0,y1), abs(y1-y0)+1, color); return; }
  if (y0 == y1) { drawFastHLine(std::min(x0,x1), y0, abs(x1-x0)+1, color); return; }

  int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int32_t err = dx + dy;
  for (;;) {
    drawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int32_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void TFT_eSPI::drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color) {
  if (y < 0 || y >= _height || w <= 0) return;
  int32_t x1 = std::max((int32_t)0, x);
  int32_t x2 = std::min((int32_t)_width, x + w);
  for (int32_t i = x1; i < x2; i++) drawPixel(i, y, color);
}

void TFT_eSPI::drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t color) {
  if (x < 0 || x >= _width || h <= 0) return;
  int32_t y1 = std::max((int32_t)0, y);
  int32_t y2 = std::min((int32_t)_height, y + h);
  for (int32_t j = y1; j < y2; j++) drawPixel(x, j, color);
}

// Midpoint circle
void TFT_eSPI::drawCircle(int32_t cx, int32_t cy, int32_t r, uint16_t color) {
  int32_t x = 0, y = r, d = 1 - r;
  while (x <= y) {
    drawPixel(cx+x, cy+y, color); drawPixel(cx-x, cy+y, color);
    drawPixel(cx+x, cy-y, color); drawPixel(cx-x, cy-y, color);
    drawPixel(cx+y, cy+x, color); drawPixel(cx-y, cy+x, color);
    drawPixel(cx+y, cy-x, color); drawPixel(cx-y, cy-x, color);
    if (d < 0) { d += 2*x + 3; }
    else { d += 2*(x-y) + 5; y--; }
    x++;
  }
}

void TFT_eSPI::fillCircle(int32_t cx, int32_t cy, int32_t r, uint16_t color) {
  drawFastHLine(cx - r, cy, 2 * r + 1, color);
  int32_t x = 0, y = r, d = 1 - r;
  while (x <= y) {
    drawFastHLine(cx - y, cy + x, 2 * y + 1, color);
    drawFastHLine(cx - y, cy - x, 2 * y + 1, color);
    drawFastHLine(cx - x, cy + y, 2 * x + 1, color);
    drawFastHLine(cx - x, cy - y, 2 * x + 1, color);
    if (d < 0) { d += 2*x + 3; }
    else { d += 2*(x-y)+5; y--; }
    x++;
  }
}

void TFT_eSPI::drawSmoothCircle(int32_t x, int32_t y, int32_t r, uint16_t fg, uint16_t bg) {
  drawCircle(x, y, r, fg);
}

void TFT_eSPI::fillSmoothCircle(int32_t x, int32_t y, int32_t r, uint16_t fg, uint16_t bg) {
  fillCircle(x, y, r, fg);
}

// drawSmoothArc — filled annular arc segment
void TFT_eSPI::drawSmoothArc(int32_t cx, int32_t cy, int32_t oR, int32_t iR,
                               int32_t startAngle, int32_t endAngle,
                               uint16_t fg, uint16_t bg, bool roundEnds) {
  if (oR <= 0 || iR < 0) return;

  // Full circle special case (before normalization eats endAngle=360)
  bool fullCircle = (endAngle - startAngle >= 360) ||
                    (startAngle == 0 && endAngle == 360);

  // Normalize angles to 0..359
  startAngle = ((startAngle % 360) + 360) % 360;
  endAngle   = ((endAngle   % 360) + 360) % 360;

  // Scan bounding box
  int32_t r = oR + 1;
  for (int32_t dy = -r; dy <= r; dy++) {
    for (int32_t dx = -r; dx <= r; dx++) {
      float dist = sqrtf((float)(dx*dx + dy*dy));
      if (dist < (float)iR - 0.5f || dist > (float)oR + 0.5f) continue;

      if (fullCircle) {
        drawPixel(cx + dx, cy + dy, fg);
        continue;
      }

      // Angle in degrees (0 = bottom/6 o'clock, counterclockwise)
      float ang = atan2f((float)-dx, (float)dy) * 180.0f / M_PI;
      if (ang < 0) ang += 360.0f;
      int32_t a = (int32_t)(ang + 0.5f) % 360;

      bool inArc;
      if (startAngle <= endAngle)
        inArc = (a >= startAngle && a <= endAngle);
      else
        inArc = (a >= startAngle || a <= endAngle);

      if (inArc) {
        drawPixel(cx + dx, cy + dy, fg);
      }
    }
  }
}

// Scanline triangle fill
void TFT_eSPI::fillTriangle(int32_t x0, int32_t y0,
                              int32_t x1, int32_t y1,
                              int32_t x2, int32_t y2, uint16_t color) {
  // Sort by y
  if (y0 > y1) { std::swap(x0,x1); std::swap(y0,y1); }
  if (y1 > y2) { std::swap(x1,x2); std::swap(y1,y2); }
  if (y0 > y1) { std::swap(x0,x1); std::swap(y0,y1); }

  if (y0 == y2) { // degenerate
    int32_t mn = std::min({x0,x1,x2}), mx = std::max({x0,x1,x2});
    drawFastHLine(mn, y0, mx - mn + 1, color);
    return;
  }

  auto interp = [](int32_t y, int32_t y0, int32_t x0, int32_t y1, int32_t x1) -> int32_t {
    if (y1 == y0) return x0;
    return x0 + (int32_t)((int64_t)(x1-x0)*(y-y0)/(y1-y0));
  };

  for (int32_t y = y0; y <= y2; y++) {
    int32_t xa, xb;
    if (y < y1) {
      xa = interp(y, y0, x0, y2, x2);
      xb = interp(y, y0, x0, y1, x1);
    } else {
      xa = interp(y, y0, x0, y2, x2);
      xb = interp(y, y1, x1, y2, x2);
    }
    if (xa > xb) std::swap(xa, xb);
    drawFastHLine(xa, y, xb - xa + 1, color);
  }
}

void TFT_eSPI::drawTriangle(int32_t x0, int32_t y0,
                              int32_t x1, int32_t y1,
                              int32_t x2, int32_t y2, uint16_t color) {
  drawLine(x0,y0,x1,y1,color);
  drawLine(x1,y1,x2,y2,color);
  drawLine(x2,y2,x0,y0,color);
}

// --- Pixel push interface ---
void TFT_eSPI::setWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  _win_x0 = x0; _win_y0 = y0;
  _win_x1 = x1; _win_y1 = y1;
  _win_cx = x0; _win_cy = y0;
}

void TFT_eSPI::pushBlock(uint16_t color, uint32_t len) {
  while (len--) {
    drawPixel(_win_cx, _win_cy, color);
    _win_cx++;
    if (_win_cx > _win_x1) { _win_cx = _win_x0; _win_cy++; }
    if (_win_cy > _win_y1) break;
  }
}

void TFT_eSPI::pushColor(uint16_t color) {
  drawPixel(_win_cx, _win_cy, color);
  _win_cx++;
  if (_win_cx > _win_x1) { _win_cx = _win_x0; _win_cy++; }
}

void TFT_eSPI::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* data) {
  for (int32_t j = 0; j < h; j++)
    for (int32_t i = 0; i < w; i++)
      drawPixel(x + i, y + j, data[j * w + i]);
}

void TFT_eSPI::setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  setWindow(x, y, x + w - 1, y + h - 1);
}

void TFT_eSPI::pushPixels(uint16_t* data, uint32_t len) {
  // pushPixels receives raw SPI-bus-format data (big-endian RGB565).
  // GIFDraw builds pixels from a big-endian palette (BIG_ENDIAN_PIXELS).
  // Our framebuffer is native-endian, so we byte-swap each pixel.
  while (len--) {
    uint16_t c = *data++;
    c = (c >> 8) | (c << 8);
    drawPixel(_win_cx, _win_cy, c);
    _win_cx++;
    if (_win_cx > _win_x1) { _win_cx = _win_x0; _win_cy++; }
    if (_win_cy > _win_y1) break;
  }
}

// --- Basic 5x7 font for drawChar ---
static const uint8_t font5x7[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, // space (32)
  {0x00,0x00,0x5F,0x00,0x00}, // !
  {0x00,0x07,0x00,0x07,0x00}, // "
  {0x14,0x7F,0x14,0x7F,0x14}, // #
  {0x24,0x2A,0x7F,0x2A,0x12}, // $
  {0x23,0x13,0x08,0x64,0x62}, // %
  {0x36,0x49,0x55,0x22,0x50}, // &
  {0x00,0x05,0x03,0x00,0x00}, // '
  {0x00,0x1C,0x22,0x41,0x00}, // (
  {0x00,0x41,0x22,0x1C,0x00}, // )
  {0x08,0x2A,0x1C,0x2A,0x08}, // *
  {0x08,0x08,0x3E,0x08,0x08}, // +
  {0x00,0x50,0x30,0x00,0x00}, // ,
  {0x08,0x08,0x08,0x08,0x08}, // -
  {0x00,0x60,0x60,0x00,0x00}, // .
  {0x20,0x10,0x08,0x04,0x02}, // /
  {0x3E,0x51,0x49,0x45,0x3E}, // 0
  {0x00,0x42,0x7F,0x40,0x00}, // 1
  {0x42,0x61,0x51,0x49,0x46}, // 2
  {0x21,0x41,0x45,0x4B,0x31}, // 3
  {0x18,0x14,0x12,0x7F,0x10}, // 4
  {0x27,0x45,0x45,0x45,0x39}, // 5
  {0x3C,0x4A,0x49,0x49,0x30}, // 6
  {0x01,0x71,0x09,0x05,0x03}, // 7
  {0x36,0x49,0x49,0x49,0x36}, // 8
  {0x06,0x49,0x49,0x29,0x1E}, // 9
  {0x00,0x36,0x36,0x00,0x00}, // :
  {0x00,0x56,0x36,0x00,0x00}, // ;
  {0x00,0x08,0x14,0x22,0x41}, // <
  {0x14,0x14,0x14,0x14,0x14}, // =
  {0x41,0x22,0x14,0x08,0x00}, // >
  {0x02,0x01,0x51,0x09,0x06}, // ?
  {0x32,0x49,0x79,0x41,0x3E}, // @
  {0x7E,0x11,0x11,0x11,0x7E}, // A
  {0x7F,0x49,0x49,0x49,0x36}, // B
  {0x3E,0x41,0x41,0x41,0x22}, // C
  {0x7F,0x41,0x41,0x22,0x1C}, // D
  {0x7F,0x49,0x49,0x49,0x41}, // E
  {0x7F,0x09,0x09,0x01,0x01}, // F
  {0x3E,0x41,0x41,0x51,0x32}, // G
  {0x7F,0x08,0x08,0x08,0x7F}, // H
  {0x00,0x41,0x7F,0x41,0x00}, // I
  {0x20,0x40,0x41,0x3F,0x01}, // J
  {0x7F,0x08,0x14,0x22,0x41}, // K
  {0x7F,0x40,0x40,0x40,0x40}, // L
  {0x7F,0x02,0x04,0x02,0x7F}, // M
  {0x7F,0x04,0x08,0x10,0x7F}, // N
  {0x3E,0x41,0x41,0x41,0x3E}, // O
  {0x7F,0x09,0x09,0x09,0x06}, // P
  {0x3E,0x41,0x51,0x21,0x5E}, // Q
  {0x7F,0x09,0x19,0x29,0x46}, // R
  {0x46,0x49,0x49,0x49,0x31}, // S
  {0x01,0x01,0x7F,0x01,0x01}, // T
  {0x3F,0x40,0x40,0x40,0x3F}, // U
  {0x1F,0x20,0x40,0x20,0x1F}, // V
  {0x7F,0x20,0x18,0x20,0x7F}, // W
  {0x63,0x14,0x08,0x14,0x63}, // X
  {0x03,0x04,0x78,0x04,0x03}, // Y
  {0x61,0x51,0x49,0x45,0x43}, // Z
  {0x00,0x00,0x7F,0x41,0x41}, // [
  {0x02,0x04,0x08,0x10,0x20}, // backslash
  {0x41,0x41,0x7F,0x00,0x00}, // ]
  {0x04,0x02,0x01,0x02,0x04}, // ^
  {0x40,0x40,0x40,0x40,0x40}, // _
  {0x00,0x01,0x02,0x04,0x00}, // `
  {0x20,0x54,0x54,0x54,0x78}, // a
  {0x7F,0x48,0x44,0x44,0x38}, // b
  {0x38,0x44,0x44,0x44,0x20}, // c
  {0x38,0x44,0x44,0x48,0x7F}, // d
  {0x38,0x54,0x54,0x54,0x18}, // e
  {0x08,0x7E,0x09,0x01,0x02}, // f
  {0x08,0x14,0x54,0x54,0x3C}, // g
  {0x7F,0x08,0x04,0x04,0x78}, // h
  {0x00,0x44,0x7D,0x40,0x00}, // i
  {0x20,0x40,0x44,0x3D,0x00}, // j
  {0x00,0x7F,0x10,0x28,0x44}, // k
  {0x00,0x41,0x7F,0x40,0x00}, // l
  {0x7C,0x04,0x18,0x04,0x78}, // m
  {0x7C,0x08,0x04,0x04,0x78}, // n
  {0x38,0x44,0x44,0x44,0x38}, // o
  {0x7C,0x14,0x14,0x14,0x08}, // p
  {0x08,0x14,0x14,0x18,0x7C}, // q
  {0x7C,0x08,0x04,0x04,0x08}, // r
  {0x48,0x54,0x54,0x54,0x20}, // s
  {0x04,0x3F,0x44,0x40,0x20}, // t
  {0x3C,0x40,0x40,0x20,0x7C}, // u
  {0x1C,0x20,0x40,0x20,0x1C}, // v
  {0x3C,0x40,0x30,0x40,0x3C}, // w
  {0x44,0x28,0x10,0x28,0x44}, // x
  {0x0C,0x50,0x50,0x50,0x3C}, // y
  {0x44,0x64,0x54,0x4C,0x44}, // z
  {0x00,0x08,0x36,0x41,0x00}, // {
  {0x00,0x00,0x7F,0x00,0x00}, // |
  {0x00,0x41,0x36,0x08,0x00}, // }
  {0x08,0x08,0x2A,0x1C,0x08}, // ~
};

void TFT_eSPI::drawChar(int32_t x, int32_t y, uint16_t c, uint16_t color, uint16_t bg, uint8_t size) {
  if (c < 32 || c > 126) c = 32;
  const uint8_t* glyph = font5x7[c - 32];
  for (int8_t col = 0; col < 5; col++) {
    uint8_t line = glyph[col];
    for (int8_t row = 0; row < 7; row++) {
      uint16_t clr = (line & (1 << row)) ? color : bg;
      if (size == 1) {
        drawPixel(x + col, y + row, clr);
      } else {
        fillRect(x + col * size, y + row * size, size, size, clr);
      }
    }
  }
  // Gap column
  if (size == 1) {
    for (int8_t row = 0; row < 7; row++) drawPixel(x + 5, y + row, bg);
  } else {
    fillRect(x + 5 * size, y, size, 7 * size, bg);
  }
}

uint16_t TFT_eSPI::alphaBlend(uint8_t alpha, uint16_t fg, uint16_t bg) {
  uint8_t fgR = (fg >> 11) & 0x1F, fgG = (fg >> 5) & 0x3F, fgB = fg & 0x1F;
  uint8_t bgR = (bg >> 11) & 0x1F, bgG = (bg >> 5) & 0x3F, bgB = bg & 0x1F;
  uint8_t r = (fgR * alpha + bgR * (255 - alpha)) / 255;
  uint8_t g = (fgG * alpha + bgG * (255 - alpha)) / 255;
  uint8_t b = (fgB * alpha + bgB * (255 - alpha)) / 255;
  return (r << 11) | (g << 5) | b;
}

// ===========================================================================
// TFT_eSprite implementations
// ===========================================================================

void* TFT_eSprite::createSprite(int16_t w, int16_t h) {
  deleteSprite();
  _spr_w = w; _spr_h = h;
  _width = w; _height = h;
  if (_colorDepth == 8) {
    _buf8 = new uint8_t[w * h]();
    _created = true;
    return _buf8;
  } else {
    _buf16 = new uint16_t[w * h]();
    _created = true;
    return _buf16;
  }
}

void TFT_eSprite::deleteSprite() {
  delete[] _buf16; _buf16 = nullptr;
  delete[] _buf8;  _buf8 = nullptr;
  _created = false; _spr_w = 0; _spr_h = 0;
  _width = 0; _height = 0;
}

void TFT_eSprite::fillSprite(uint16_t color) {
  if (_colorDepth == 8 && _buf8) {
    memset(_buf8, (uint8_t)color, _spr_w * _spr_h);
  } else if (_buf16) {
    for (int i = 0; i < _spr_w * _spr_h; i++) _buf16[i] = color;
  }
}

void TFT_eSprite::drawPixel(int32_t x, int32_t y, uint16_t color) {
  if (x < 0 || x >= _spr_w || y < 0 || y >= _spr_h) return;
  if (_colorDepth == 8 && _buf8) {
    _buf8[y * _spr_w + x] = (uint8_t)color;
  } else if (_buf16) {
    _buf16[y * _spr_w + x] = color;
  }
}

uint16_t TFT_eSprite::readPixel(int32_t x, int32_t y) {
  if (x < 0 || x >= _spr_w || y < 0 || y >= _spr_h) return 0;
  if (_colorDepth == 8 && _buf8) {
    uint8_t idx = _buf8[y * _spr_w + x];
    return _hasPalette ? _palette[idx] : idx;
  } else if (_buf16) {
    return _buf16[y * _spr_w + x];
  }
  return 0;
}

void TFT_eSprite::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  int32_t x1 = std::max((int32_t)0, x);
  int32_t y1 = std::max((int32_t)0, y);
  int32_t x2 = std::min((int32_t)_spr_w, x + w);
  int32_t y2 = std::min((int32_t)_spr_h, y + h);
  if (_colorDepth == 8 && _buf8) {
    for (int32_t j = y1; j < y2; j++)
      for (int32_t i = x1; i < x2; i++)
        _buf8[j * _spr_w + i] = (uint8_t)color;
  } else if (_buf16) {
    for (int32_t j = y1; j < y2; j++)
      for (int32_t i = x1; i < x2; i++)
        _buf16[j * _spr_w + i] = color;
  }
}

void TFT_eSprite::pushSprite(int32_t x, int32_t y) {
  if (!_parent) return;
  uint16_t* dst = _parent->getFramebuffer();
  int dw = _parent->width();
  int dh = _parent->height();
  for (int32_t j = 0; j < _spr_h; j++) {
    int32_t dy = y + j;
    if (dy < 0 || dy >= dh) continue;
    for (int32_t i = 0; i < _spr_w; i++) {
      int32_t dx = x + i;
      if (dx < 0 || dx >= dw) continue;
      uint16_t c;
      if (_colorDepth == 8 && _buf8) {
        uint8_t idx = _buf8[j * _spr_w + i];
        c = _hasPalette ? _palette[idx] : idx;
      } else if (_buf16) {
        c = _buf16[j * _spr_w + i];
      } else continue;
      dst[dy * dw + dx] = c;
    }
  }
}

void TFT_eSprite::pushSprite(int32_t x, int32_t y, uint16_t transparent) {
  if (!_parent) return;
  uint16_t* dst = _parent->getFramebuffer();
  int dw = _parent->width();
  int dh = _parent->height();
  for (int32_t j = 0; j < _spr_h; j++) {
    int32_t dy = y + j;
    if (dy < 0 || dy >= dh) continue;
    for (int32_t i = 0; i < _spr_w; i++) {
      int32_t dx = x + i;
      if (dx < 0 || dx >= dw) continue;
      uint16_t c;
      if (_colorDepth == 8 && _buf8) {
        uint8_t idx = _buf8[j * _spr_w + i];
        c = _hasPalette ? _palette[idx] : idx;
      } else if (_buf16) {
        c = _buf16[j * _spr_w + i];
      } else continue;
      if (c != transparent) dst[dy * dw + dx] = c;
    }
  }
}

void TFT_eSprite::setWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  _win_x0 = x0; _win_y0 = y0;
  _win_x1 = x1; _win_y1 = y1;
  _win_cx = x0; _win_cy = y0;
}

void TFT_eSprite::pushBlock(uint16_t color, uint32_t len) {
  while (len--) {
    drawPixel(_win_cx, _win_cy, color);
    _win_cx++;
    if (_win_cx > _win_x1) { _win_cx = _win_x0; _win_cy++; }
    if (_win_cy > _win_y1) break;
  }
}

void TFT_eSprite::pushColor(uint16_t color) {
  drawPixel(_win_cx, _win_cy, color);
  _win_cx++;
  if (_win_cx > _win_x1) { _win_cx = _win_x0; _win_cy++; }
}

void TFT_eSprite::createPalette(const uint16_t* pal, int sz) {
  int n = std::min(sz, 256);
  for (int i = 0; i < n; i++) _palette[i] = pal[i];
  _hasPalette = true;
}
