#pragma once
// SPI stub — not needed for emulation, but kgfx.h includes it
class SPIClass {
public:
  void begin() {}
  void end() {}
};
extern SPIClass SPI;
