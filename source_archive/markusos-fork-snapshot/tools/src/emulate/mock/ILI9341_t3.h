#pragma once
// Font headers (font_Arial.h etc.) include this to get ILI9341_t3_font_t.
// In the real TFT_eSPI_ext.h, this struct is called tftfont_t.
// The font .c files actually use tftfont_t directly, so we just need this
// typedef for the font .h declarations.
#include "TFT_eSPI_ext.h"
typedef tftfont_t ILI9341_t3_font_t;
