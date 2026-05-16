#include "TJpg_Decoder.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "../stb_image.h"

TJpg_Decoder TJpgDec;

// Convert RGB888 to RGB565
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Byte-swap a 16-bit value
static inline uint16_t swap16(uint16_t v) {
  return (v >> 8) | (v << 8);
}

JRESULT TJpg_Decoder::getJpgSize(uint16_t *w, uint16_t *h, const uint8_t *array, uint32_t array_size) {
  int iw, ih, comp;
  if (stbi_info_from_memory(array, (int)array_size, &iw, &ih, &comp)) {
    *w = (uint16_t)iw;
    *h = (uint16_t)ih;
    return JDR_OK;
  }
  *w = 0; *h = 0;
  return 1; // error
}

JRESULT TJpg_Decoder::drawJpg(int32_t x, int32_t y, const uint8_t *array, uint32_t array_size) {
  if (!_callback) return 1;

  int w, h, comp;
  uint8_t *pixels = stbi_load_from_memory(array, (int)array_size, &w, &h, &comp, 3);
  if (!pixels) return 1;

  int scale = _scale > 0 ? _scale : 1;
  int sw = w / scale;
  int sh = h / scale;

  // Process in 16-pixel-wide MCU blocks (matching TJpg_Decoder behavior)
  const int MCU_W = 16;
  const int MCU_H = 16;

  uint16_t block[MCU_W * MCU_H];

  for (int by = 0; by < sh; by += MCU_H) {
    for (int bx = 0; bx < sw; bx += MCU_W) {
      int bw = (bx + MCU_W <= sw) ? MCU_W : (sw - bx);
      int bh = (by + MCU_H <= sh) ? MCU_H : (sh - by);

      for (int j = 0; j < bh; j++) {
        for (int i = 0; i < bw; i++) {
          int srcX = (bx + i) * scale;
          int srcY = (by + j) * scale;
          int idx = (srcY * w + srcX) * 3;
          uint16_t c = rgb565(pixels[idx], pixels[idx + 1], pixels[idx + 2]);
          // Don't byte-swap: our framebuffer is native-endian, and we generate
          // native RGB565 from stb_image's RGB888 output. The real TJpg_Decoder's
          // setSwapBytes controls SPI bus byte order, which doesn't apply here.
          block[j * bw + i] = c;
        }
      }

      if (!_callback((int16_t)(x + bx), (int16_t)(y + by), (uint16_t)bw, (uint16_t)bh, block)) {
        stbi_image_free(pixels);
        return JDR_OK;
      }
    }
  }

  stbi_image_free(pixels);
  return JDR_OK;
}
