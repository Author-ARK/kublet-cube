#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Color constants (RGB565)
// ---------------------------------------------------------------------------
#define TFT_BLACK       0x0000
#define TFT_WHITE       0xFFFF
#define TFT_RED         0xF800
#define TFT_GREEN       0x07E0
#define TFT_BLUE        0x001F
#define TFT_YELLOW      0xFFE0
#define TFT_CYAN        0x07FF
#define TFT_MAGENTA     0xF81F
#define TFT_ORANGE      0xFDA0
#define TFT_GREENYELLOW 0xB7E0
#define TFT_PINK        0xFE19
#define TFT_BROWN       0x9A60
#define TFT_GOLD        0xFEA0
#define TFT_SILVER      0xC618
#define TFT_SKYBLUE     0x867D
#define TFT_VIOLET      0x915C
#define TFT_NAVY        0x000F
#define TFT_MAROON      0x7800
#define TFT_PURPLE      0x780F
#define TFT_OLIVE       0x7BE0
#define TFT_LIGHTGREY   0xD69A
#define TFT_DARKGREY    0x7BEF
#define TFT_DARKGREEN   0x03E0
#define TFT_DARKCYAN    0x03EF

// Text datum
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 3
#define MC_DATUM 4
#define MR_DATUM 5
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8

// Forward declaration
class TFT_eSprite;

// Global display registration (used by main_wrapper to find the primary framebuffer)
extern void _emu_register_display(void* tft);

// ---------------------------------------------------------------------------
// TFT_eSPI mock — renders to an in-memory RGB565 framebuffer
// ---------------------------------------------------------------------------
// Global shared framebuffer — all non-sprite TFT_eSPI instances share it
// (on real hardware there's one physical display; both KGFX::t and KGFX::tft write to the same SPI bus)
extern uint16_t _emu_shared_fb[240 * 240];

class TFT_eSPI {
public:
  static constexpr int EMU_W = 240;
  static constexpr int EMU_H = 240;

  TFT_eSPI() { _fb = _emu_shared_fb; }
  virtual ~TFT_eSPI() {}

  // --- Init ---
  void begin() { _emu_register_display(this); }
  void init() { _emu_register_display(this); }
  void setRotation(uint8_t) {}

  // --- Dimensions ---
  virtual int16_t width()  { return _width; }
  virtual int16_t height() { return _height; }

  // --- Drawing primitives (implemented in TFT_eSPI_impl.cpp) ---
  virtual void drawPixel(int32_t x, int32_t y, uint16_t color);
  virtual uint16_t readPixel(int32_t x, int32_t y);

  virtual void fillScreen(uint16_t color);
  virtual void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
  virtual void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
  virtual void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color);
  virtual void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color);

  virtual void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color);
  virtual void drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color);
  virtual void drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t color);

  virtual void drawCircle(int32_t x, int32_t y, int32_t r, uint16_t color);
  virtual void fillCircle(int32_t x, int32_t y, int32_t r, uint16_t color);
  virtual void drawSmoothCircle(int32_t x, int32_t y, int32_t r, uint16_t fg, uint16_t bg);
  virtual void fillSmoothCircle(int32_t x, int32_t y, int32_t r, uint16_t fg, uint16_t bg);

  virtual void drawSmoothArc(int32_t cx, int32_t cy, int32_t oR, int32_t iR,
                              int32_t startAngle, int32_t endAngle,
                              uint16_t fg, uint16_t bg, bool roundEnds);

  virtual void fillTriangle(int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2, uint16_t color);

  virtual void drawTriangle(int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2, uint16_t color);

  // --- Text ---
  virtual void setTextColor(uint16_t fg) { textcolor = fg; textbgcolor = fg; }
  virtual void setTextColor(uint16_t fg, uint16_t bg) { textcolor = fg; textbgcolor = bg; }
  virtual void setTextSize(uint8_t s) { textsize = s; }
  virtual void setTextDatum(uint8_t d) { textdatum = d; }
  virtual void setTextFont(uint8_t f) { textfont = f; }
  virtual void setTextWrap(bool w) { textwrapX = w; }

  void setCursor(int16_t x, int16_t y) { cursor_x = x; cursor_y = y; }
  void setCursor(int16_t x, int16_t y, uint8_t font) { cursor_x = x; cursor_y = y; textfont = font; }

  virtual size_t write(uint8_t c) { return 1; } // basic write — overridden by TFT_eSPI_ext for TTF
  size_t print(const char* s) {
    size_t n = 0;
    while (*s) { n += write(*s++); }
    return n;
  }
  size_t print(char c) { return write(c); }
  size_t print(int v) { char buf[16]; snprintf(buf,16,"%d",v); return print(buf); }
  size_t print(float v, int d=2) { char buf[32]; snprintf(buf,32,"%.*f",d,v); return print(buf); }
  size_t println(const char* s = "") { size_t n = print(s); n += print('\n'); return n; }
  size_t println(int v) { size_t n = print(v); n += print('\n'); return n; }

  virtual void drawChar(int32_t x, int32_t y, uint16_t c, uint16_t color, uint16_t bg, uint8_t size);

  int16_t drawString(const char* s, int32_t x, int32_t y) {
    setCursor(x, y);
    print(s);
    return 0;
  }

  // --- Pixel push interface (for TFT_eSPI_ext font rendering) ---
  virtual void setWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
  virtual void pushBlock(uint16_t color, uint32_t len);
  virtual void pushColor(uint16_t color);
  virtual void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* data);
  virtual void pushPixels(uint16_t* data, uint32_t len);
  virtual void setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h);

  virtual void startWrite() {}
  virtual void endWrite() {}

  // --- Color utility ---
  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }

  static uint16_t alphaBlend(uint8_t alpha, uint16_t fg, uint16_t bg);

  // --- Framebuffer access (for SDL rendering) ---
  const uint16_t* getFramebuffer() const { return _fb; }
  uint16_t* getFramebuffer() { return _fb; }

  // --- Public members matching TFT_eSPI ---
  int16_t cursor_x = 0, cursor_y = 0;
  uint16_t textcolor = TFT_WHITE, textbgcolor = TFT_BLACK;
  uint8_t textfont = 1, textsize = 1, textdatum = 0;
  bool textwrapX = true;

protected:
  uint16_t* _fb;
  int16_t _width = EMU_W, _height = EMU_H;

  // Window cursor for pushBlock/pushColor
  int32_t _win_x0 = 0, _win_y0 = 0, _win_x1 = 0, _win_y1 = 0;
  int32_t _win_cx = 0, _win_cy = 0;

  inline void _setPixel(int32_t x, int32_t y, uint16_t color) {
    if (x >= 0 && x < _width && y >= 0 && y < _height)
      _fb[y * _width + x] = color;
  }
  inline uint16_t _getPixel(int32_t x, int32_t y) const {
    if (x >= 0 && x < _width && y >= 0 && y < _height)
      return _fb[y * _width + x];
    return 0;
  }
};

// ---------------------------------------------------------------------------
// TFT_eSprite — off-screen sprite with its own buffer
// ---------------------------------------------------------------------------
class TFT_eSprite : public TFT_eSPI {
public:
  TFT_eSprite(TFT_eSPI* parent) : _parent(parent) {
    _width = 0; _height = 0;
  }

  ~TFT_eSprite() { deleteSprite(); }

  void setColorDepth(uint8_t depth) { _colorDepth = depth; }

  void* createSprite(int16_t w, int16_t h);
  void deleteSprite();

  void fillSprite(uint16_t color);

  void pushSprite(int32_t x, int32_t y);
  void pushSprite(int32_t x, int32_t y, uint16_t transparent);

  // Override drawing to use sprite buffer
  void drawPixel(int32_t x, int32_t y, uint16_t color) override;
  uint16_t readPixel(int32_t x, int32_t y) override;
  void fillScreen(uint16_t color) override { fillSprite(color); }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) override;

  // Pixel push for sprite
  void setWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1) override;
  void pushBlock(uint16_t color, uint32_t len) override;
  void pushColor(uint16_t color) override;

  int16_t width() override { return _spr_w; }
  int16_t height() override { return _spr_h; }

  // Palette for 8-bit sprites
  void createPalette(const uint16_t* pal, int sz = 16);

private:
  TFT_eSPI* _parent = nullptr;
  uint8_t _colorDepth = 16;
  int16_t _spr_w = 0, _spr_h = 0;

  // 16-bit buffer
  uint16_t* _buf16 = nullptr;
  // 8-bit buffer
  uint8_t* _buf8 = nullptr;
  // Palette (for 8-bit mode)
  uint16_t _palette[256] = {};
  bool _hasPalette = false;
  bool _created = false;
};
