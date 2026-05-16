#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Stub JRESULT type
typedef int JRESULT;
#define JDR_OK 0

// Callback type matching the real TJpg_Decoder
typedef bool (*SketchCallback)(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *data);

class TJpg_Decoder {
public:
  TJpg_Decoder() {}
  ~TJpg_Decoder() {}

  void setJpgScale(uint8_t scale) { _scale = scale; }
  void setSwapBytes(bool swap) { _swap = swap; }
  void setCallback(SketchCallback cb) { _callback = cb; }

  // Decode JPEG from memory array and get dimensions
  JRESULT getJpgSize(uint16_t *w, uint16_t *h, const uint8_t *array, uint32_t array_size);

  // Decode JPEG from memory array and render via callback
  JRESULT drawJpg(int32_t x, int32_t y, const uint8_t *array, uint32_t array_size);

private:
  uint8_t _scale = 1;
  bool _swap = false;
  SketchCallback _callback = nullptr;
};

extern TJpg_Decoder TJpgDec;
