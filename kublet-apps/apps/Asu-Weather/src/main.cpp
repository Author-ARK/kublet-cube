#include <Arduino.h>
#include <otaserver.h>
#include <kgfx.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#define BUTTON_PIN 19

// --- LOCATIONS ---
struct Location {
  const char* name;
  const char* lat;
  const char* lon;
};

// Single-location build of the upstream weather app — Asuncion, Paraguay.
// Pressing the button still re-fetches but no longer cycles cities. Keep
// the array structure so the upstream loop / fetch logic stays unchanged.
// Short label fits the new dense top row alongside sun + moon times.
const Location locations[] = {
  {"ASU",      "-25.28",  "-57.63"},
};
const int NUM_LOCATIONS = sizeof(locations) / sizeof(locations[0]);
int currentLocation = 0;

OTAServer otaserver;
KGFX ui;

bool buttonWasPressed = false;
unsigned long lastFetch = 0;
const unsigned long FETCH_INTERVAL = 15 * 60 * 1000; // 15 minutes

// --- WEATHER ICON TYPES ---
enum IconType {
  ICON_CLEAR,          // 0: clear sky
  ICON_MOSTLY_CLEAR,   // 1: mainly clear
  ICON_PARTLY_CLOUDY,  // 2: partly cloudy
  ICON_CLOUDY,         // 3: overcast
  ICON_FOG,            // 45, 48: fog
  ICON_DRIZZLE,        // 51, 53, 55: drizzle
  ICON_RAIN,           // 61-65, 80-82: rain
  ICON_FREEZING,       // 56-57, 66-67: freezing rain/sleet
  ICON_SNOW,           // 71-77, 85-86: snow
  ICON_THUNDER,        // 95-99: thunderstorm
};

// --- WEATHER DATA ---
struct WeatherData {
  float currentTemp;
  int currentCode;
  float dailyMax[3], dailyMin[3];
  int dailyCode[3];
  int dailyMonth[3], dailyDay[3];
  char dayNames[3][4];
  // Today's sunrise/sunset times in HH:MM form, parsed from the ISO
  // strings Open-Meteo returns under daily.sunrise[0] / daily.sunset[0].
  char sunrise[6];
  char sunset[6];
  bool valid;
};
WeatherData weather = {0, 0, {0}, {0}, {0}, {0}, {0}, {""}, "", "", false};
// Tracks whether the most recent fetch succeeded — drives the red/green
// dot in the bottom IP bar so you can see at a glance that updates are
// flowing on the 15-min cadence.
bool lastFetchOk = false;

// Moon data comes from api.met.no — Open-Meteo's free tier doesn't
// publish moonrise/moonset. met.no needs no key but requires a polite
// User-Agent header per their TOS. Times are returned in UTC; we shift
// to Asuncion local (-3) before storing as "HH:MM".
struct MoonData {
  char rise[6];
  char set[6];
  bool valid;
};
MoonData moon = {"", "", false};
int lastMoonDay = -1;  // gmtime tm_mday of the last successful moon fetch

// --- COLORS ---
uint16_t CLR_SUN, CLR_SUN_RAY, CLR_CLOUD, CLR_CLOUD_DK, CLR_RAIN, CLR_SNOW, CLR_SNOW_LT;
uint16_t CLR_LIGHTNING, CLR_FOG, CLR_ICE, CLR_BG, CLR_DIVIDER;

void initColors() {
  CLR_SUN      = ui.tft.color565(255, 220, 0);
  CLR_SUN_RAY  = ui.tft.color565(255, 160, 0);
  CLR_CLOUD    = ui.tft.color565(200, 200, 210);
  CLR_CLOUD_DK = ui.tft.color565(140, 140, 155);
  CLR_RAIN     = ui.tft.color565(80, 140, 255);
  CLR_SNOW     = ui.tft.color565(240, 240, 255);
  CLR_SNOW_LT  = ui.tft.color565(180, 210, 255);
  CLR_LIGHTNING = ui.tft.color565(255, 255, 100);
  CLR_FOG      = ui.tft.color565(170, 170, 185);
  CLR_ICE      = ui.tft.color565(140, 220, 255);
  CLR_BG       = TFT_BLACK;
  CLR_DIVIDER  = ui.tft.color565(40, 40, 50);
}

// --- WMO CODE MAPPING ---
IconType wmoToIcon(int code) {
  switch (code) {
    case 0:  return ICON_CLEAR;
    case 1:  return ICON_MOSTLY_CLEAR;
    case 2:  return ICON_PARTLY_CLOUDY;
    case 3:  return ICON_CLOUDY;
    case 45: case 48: return ICON_FOG;
    case 51: case 53: case 55: return ICON_DRIZZLE;
    case 56: case 57: case 66: case 67: return ICON_FREEZING;
    case 61: case 63: case 65: case 80: case 81: case 82: return ICON_RAIN;
    case 71: case 73: case 75: case 77: case 85: case 86: return ICON_SNOW;
    case 95: case 96: case 99: return ICON_THUNDER;
    default: return ICON_CLOUDY;
  }
}

const char* wmoToLabel(int code) {
  switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mostly Clear";
    case 2:  return "Partly Cloudy";
    case 3:  return "Cloudy";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing Drizzle";
    case 61: case 80: return "Light Rain";
    case 63: case 81: return "Rain";
    case 65: case 82: return "Heavy Rain";
    case 66: case 67: return "Freezing Rain";
    case 71: case 85: return "Light Snow";
    case 73: case 86: return "Snow";
    case 75: return "Heavy Snow";
    case 77: return "Snow Grains";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm";
    default: return "Unknown";
  }
}

// --- ANIMATION STATE ---
float animPhase = 0;
unsigned long lastAnimFrame = 0;
const unsigned long ANIM_INTERVAL = 120; // ~8 fps for smooth animation

// Sprite for flicker-free icon animation (120x110 @ 16-bit = 26,400 bytes)
#define ICON_SPR_W 90
#define ICON_SPR_H 100
#define ICON_SPR_X 10
#define ICON_SPR_Y 35
TFT_eSprite iconSpr = TFT_eSprite(&ui.tft);

// Draw target: either screen or sprite
TFT_eSPI* drawTarget = nullptr;

// --- PIXEL ART ICON DRAWING ---
// Small icon helper: draw a 2x2 pixel block on grid
inline void spx(int cx, int cy, int gx, int gy, uint16_t col) {
  drawTarget->fillRect(cx + gx * 2, cy + gy * 2, 2, 2, col);
}
// Large icon helper: draw a 3x3 pixel block
inline void lpx(int cx, int cy, int gx, int gy, uint16_t col) {
  drawTarget->fillRect(cx + gx * 3, cy + gy * 3, 3, 3, col);
}
// Large icon: draw a rect in pixel coords relative to icon center
inline void lrect(int cx, int cy, int x, int y, int w, int h, uint16_t col) {
  drawTarget->fillRect(cx + x, cy + y, w, h, col);
}

// =============================================
// SMALL ICONS (for 3-day forecast, ~24x24 area)
// =============================================
void drawSmallCloud(int cx, int cy, uint16_t col, uint16_t shd) {
  for (int x = -3; x <= 3; x++) spx(cx, cy, x, 1, col);
  for (int x = -4; x <= 4; x++) spx(cx, cy, x, 2, col);
  for (int x = -4; x <= 4; x++) spx(cx, cy, x, 3, shd);
  for (int x = -3; x <= 3; x++) spx(cx, cy, x, 4, shd);
  for (int x = -2; x <= 0; x++) spx(cx, cy, x, 0, col);
  for (int x = 1; x <= 3; x++) spx(cx, cy, x, 0, col);
  spx(cx, cy, -1, -1, col); spx(cx, cy, 2, -1, col);
}

void drawSmallSun(int cx, int cy) {
  for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++)
      spx(cx, cy, x, y, CLR_SUN);
  spx(cx, cy, -2, 0, CLR_SUN); spx(cx, cy, 2, 0, CLR_SUN);
  spx(cx, cy, 0, -2, CLR_SUN); spx(cx, cy, 0, 2, CLR_SUN);
  spx(cx, cy, 0, -3, CLR_SUN_RAY); spx(cx, cy, 0, 3, CLR_SUN_RAY);
  spx(cx, cy, -3, 0, CLR_SUN_RAY); spx(cx, cy, 3, 0, CLR_SUN_RAY);
  spx(cx, cy, -2, -2, CLR_SUN_RAY); spx(cx, cy, 2, -2, CLR_SUN_RAY);
  spx(cx, cy, -2, 2, CLR_SUN_RAY); spx(cx, cy, 2, 2, CLR_SUN_RAY);
}

void drawSmallIcon(int cx, int cy, IconType icon) {
  switch (icon) {
    case ICON_CLEAR:
      drawSmallSun(cx, cy);
      break;
    case ICON_MOSTLY_CLEAR:
      drawSmallSun(cx - 3, cy - 3);
      for (int x = 0; x <= 3; x++) spx(cx, cy, x, 2, CLR_CLOUD);
      for (int x = -1; x <= 4; x++) spx(cx, cy, x, 3, CLR_CLOUD_DK);
      break;
    case ICON_PARTLY_CLOUDY:
      drawSmallSun(cx - 4, cy - 4);
      drawSmallCloud(cx + 2, cy + 1, CLR_CLOUD, CLR_CLOUD_DK);
      break;
    case ICON_CLOUDY:
      drawSmallCloud(cx, cy, CLR_CLOUD, CLR_CLOUD_DK);
      break;
    case ICON_FOG:
      drawSmallCloud(cx, cy - 2, CLR_CLOUD, CLR_CLOUD_DK);
      for (int x = -3; x <= 3; x += 2) spx(cx, cy, x, 4, CLR_FOG);
      for (int x = -4; x <= 4; x += 2) spx(cx, cy, x, 5, CLR_FOG);
      break;
    case ICON_DRIZZLE:
      drawSmallCloud(cx, cy - 2, CLR_CLOUD, CLR_CLOUD_DK);
      spx(cx, cy, -2, 4, CLR_RAIN); spx(cx, cy, 1, 5, CLR_RAIN);
      break;
    case ICON_RAIN: {
      uint16_t dk = ui.tft.color565(170, 170, 185);
      drawSmallCloud(cx, cy - 2, dk, ui.tft.color565(130, 130, 145));
      spx(cx, cy, -3, 4, CLR_RAIN); spx(cx, cy, -1, 5, CLR_RAIN);
      spx(cx, cy, 1, 4, CLR_RAIN); spx(cx, cy, 3, 5, CLR_RAIN);
      break;
    }
    case ICON_FREEZING:
      drawSmallCloud(cx, cy - 2, CLR_CLOUD, CLR_CLOUD_DK);
      spx(cx, cy, -2, 4, CLR_ICE); spx(cx, cy, 1, 5, CLR_ICE);
      spx(cx, cy, 3, 4, CLR_SNOW);
      break;
    case ICON_SNOW:
      drawSmallCloud(cx, cy - 2, CLR_CLOUD, CLR_CLOUD_DK);
      spx(cx, cy, -2, 4, CLR_SNOW); spx(cx, cy, 1, 5, CLR_SNOW_LT);
      spx(cx, cy, 3, 4, CLR_SNOW); spx(cx, cy, -1, 6, CLR_SNOW_LT);
      break;
    case ICON_THUNDER: {
      uint16_t dk = ui.tft.color565(100, 100, 115);
      drawSmallCloud(cx, cy - 2, CLR_CLOUD_DK, dk);
      spx(cx, cy, 0, 4, CLR_LIGHTNING); spx(cx, cy, -1, 5, CLR_LIGHTNING);
      spx(cx, cy, -2, 5, CLR_RAIN); spx(cx, cy, 2, 4, CLR_RAIN);
      break;
    }
  }
}

// =============================================
// LARGE ICONS (for current weather, ~80x80 area)
// Uses actual pixel coordinates for smooth rendering
// =============================================

// Helper: draw pixel-level filled rounded rect
inline void px(int cx, int cy, int x, int y, int w, int h, uint16_t col) {
  drawTarget->fillRect(cx + x, cy + y, w, h, col);
}

// Large cloud using smooth circles and rects — no grid blocks
void drawLargeCloud(int cx, int cy, uint16_t col, uint16_t mid, uint16_t shd, uint16_t hi) {
  // Build cloud from overlapping filled circles + flat body
  // Right tall hump
  drawTarget->fillCircle(cx + 12, cy - 10, 14, hi);
  drawTarget->fillCircle(cx + 12, cy - 10, 12, col);
  // Left shorter hump
  drawTarget->fillCircle(cx - 8, cy - 4, 11, hi);
  drawTarget->fillCircle(cx - 8, cy - 4, 9, col);
  // Flat body
  drawTarget->fillRect(cx - 22, cy, 48, 8, col);
  drawTarget->fillRect(cx - 24, cy + 4, 52, 6, mid);
  drawTarget->fillRect(cx - 22, cy + 10, 48, 5, mid);
  drawTarget->fillRect(cx - 20, cy + 15, 44, 4, shd);
  drawTarget->fillRect(cx - 17, cy + 19, 38, 2, shd);
  // Connect humps to body
  drawTarget->fillRect(cx - 17, cy - 4, 40, 6, col);
  // Highlight
  drawTarget->fillCircle(cx - 5, cy - 2, 3, hi);
  drawTarget->fillCircle(cx + 18, cy - 6, 2, hi);
}

// Large sun — smooth circles with detached rays
void drawLargeSun(int cx, int cy, float anim) {
  uint16_t core = ui.tft.color565(255, 248, 160);
  uint16_t mid  = ui.tft.color565(255, 235, 60);
  uint16_t edge = ui.tft.color565(255, 200, 20);
  int pulse = (int)(sin(anim * 1.5f) * 3.0f + 0.5f);

  // Smooth disc with gradient rings
  drawTarget->fillCircle(cx, cy, 16, edge);
  drawTarget->fillCircle(cx, cy, 14, CLR_SUN);
  drawTarget->fillCircle(cx, cy, 11, mid);
  drawTarget->fillCircle(cx, cy, 7, core);

  // 8 detached rays using tapered lines
  int rayStart = 20;
  int rayLen = 10 + pulse;
  int diagStart = 24;
  int diagLen = 7 + pulse;

  // Cardinal rays (thick: 3px wide)
  px(cx, cy, -1, -(rayStart + rayLen), 3, rayLen, CLR_SUN_RAY); // N
  px(cx, cy, -1, rayStart, 3, rayLen, CLR_SUN_RAY);              // S
  px(cx, cy, -(rayStart + rayLen), -1, rayLen, 3, CLR_SUN_RAY); // W
  px(cx, cy, rayStart, -1, rayLen, 3, CLR_SUN_RAY);              // E

  // Diagonal rays (2px wide, drawn as angled segments)
  for (int r = diagStart; r < diagStart + diagLen; r++) {
    int d = (int)(r * 0.707f);
    drawTarget->fillRect(cx - d - 1, cy - d - 1, 2, 2, CLR_SUN_RAY);
    drawTarget->fillRect(cx + d,     cy - d - 1, 2, 2, CLR_SUN_RAY);
    drawTarget->fillRect(cx - d - 1, cy + d,     2, 2, CLR_SUN_RAY);
    drawTarget->fillRect(cx + d,     cy + d,     2, 2, CLR_SUN_RAY);
  }
}

// Large animated rain drops — pixel-level streaks
void drawLargeRain(int cx, int cy, int count, uint16_t col, uint16_t light, float anim) {
  int bx[] = {-18, -9, 0, 9, 18, -14, 5, -4, 14};
  int by[] = {24, 27, 24, 27, 24, 30, 30, 33, 33};
  for (int i = 0; i < count && i < 9; i++) {
    float phase = fmod(anim * 2.5f + i * 1.1f, 12.0f);
    int dy = (int)phase;
    int x = cx + bx[i];
    int y = cy + by[i] + dy;
    drawTarget->fillRect(x, y, 2, 5, col);
    drawTarget->fillRect(x, y + 5, 2, 3, light);
  }
}

// Large animated snowflakes — pixel-level cross shapes
void drawLargeSnow(int cx, int cy, uint16_t c1, uint16_t c2, float anim) {
  int bx[] = {-16, -6, 4, 14, -10, 10, 0, -18};
  int by[] = {24, 27, 24, 27, 30, 30, 33, 33};
  for (int i = 0; i < 8; i++) {
    float drift = sin(anim * 0.8f + i * 1.3f) * 4.0f;
    float fall = fmod(anim * 0.7f + i * 0.8f, 14.0f);
    int x = cx + bx[i] + (int)drift;
    int y = cy + by[i] + (int)fall;
    uint16_t c = (i % 2 == 0) ? c1 : c2;
    // Cross-shaped snowflake
    drawTarget->fillRect(x, y - 3, 2, 8, c);  // vertical
    drawTarget->fillRect(x - 3, y, 8, 2, c);  // horizontal
    // Small diagonal accents
    drawTarget->drawPixel(x - 2, y - 2, c);
    drawTarget->drawPixel(x + 3, y - 2, c);
    drawTarget->drawPixel(x - 2, y + 3, c);
    drawTarget->drawPixel(x + 3, y + 3, c);
  }
}

// Large animated lightning bolt
void drawLargeLightning(int cx, int cy, float anim) {
  float flicker = sin(anim * 5.0f) + sin(anim * 7.3f);
  if (flicker < -0.3f) return;
  uint16_t bright = CLR_LIGHTNING;
  uint16_t dim = ui.tft.color565(255, 220, 50);
  // Zigzag bolt using pixel rects
  px(cx, cy, 4, 22, 6, 3, bright);
  px(cx, cy, 1, 25, 6, 3, dim);
  px(cx, cy, -2, 28, 6, 3, bright);
  px(cx, cy, -5, 31, 10, 3, bright);  // wide flash point
  px(cx, cy, -2, 34, 5, 3, dim);
  px(cx, cy, -5, 37, 5, 3, bright);
  px(cx, cy, -8, 40, 4, 3, dim);
}

// Large animated fog bands — smooth horizontal bars
void drawLargeFog(int cx, int cy, float anim) {
  int d1 = (int)(sin(anim * 0.5f) * 6.0f);
  int d2 = (int)(sin(anim * 0.5f + 1.5f) * 6.0f);
  int d3 = (int)(sin(anim * 0.5f + 3.0f) * 6.0f);
  uint16_t c1 = CLR_FOG;
  uint16_t c2 = ui.tft.color565(150, 150, 165);
  drawTarget->fillRoundRect(cx - 22 + d1, cy + 24, 44, 3, 1, c1);
  drawTarget->fillRoundRect(cx - 18 + d2, cy + 30, 36, 3, 1, c2);
  drawTarget->fillRoundRect(cx - 22 + d3, cy + 36, 44, 3, 1, c1);
}

void drawLargeIcon(int cx, int cy, IconType icon, float anim) {
  uint16_t hi  = ui.tft.color565(230, 230, 240);
  uint16_t mid = ui.tft.color565(180, 180, 195);
  switch (icon) {
    case ICON_CLEAR:
      drawLargeSun(cx, cy, anim);
      break;

    case ICON_MOSTLY_CLEAR: {
      // Center correction: visual center was ~10px left, 12px up
      int mx = cx + 10, my = cy + 12;
      drawLargeSun(mx - 12, my - 12, anim);
      // Small cloud bottom-right (pixel coords)
      drawTarget->fillCircle(mx + 10, my + 8, 8, CLR_CLOUD);
      drawTarget->fillRect(mx - 2, my + 10, 28, 6, CLR_CLOUD);
      drawTarget->fillRect(mx - 4, my + 14, 30, 4, mid);
      drawTarget->fillRect(mx - 2, my + 18, 26, 3, CLR_CLOUD_DK);
      break;
    }

    case ICON_PARTLY_CLOUDY: {
      // Center correction: visual center was ~7px left, 11px up
      int px2 = cx + 7, py = cy + 11;
      drawLargeSun(px2 - 14, py - 14, anim);
      drawLargeCloud(px2 + 5, py + 5, CLR_CLOUD, mid, CLR_CLOUD_DK, hi);
      break;
    }

    case ICON_CLOUDY: {
      int drift = (int)(sin(anim * 0.8f) * 5.0f);
      uint16_t bk = ui.tft.color565(185, 185, 195);
      uint16_t bs = ui.tft.color565(165, 165, 175);
      drawLargeCloud(cx - 8 - drift, cy - 6, bk, bk, bs, hi);
      drawLargeCloud(cx + 5 + drift, cy + 5, CLR_CLOUD, mid, CLR_CLOUD_DK, hi);
      break;
    }

    case ICON_FOG:
      drawLargeCloud(cx, cy - 6, CLR_CLOUD, mid, CLR_CLOUD_DK, hi);
      drawLargeFog(cx, cy - 6, anim);
      break;

    case ICON_DRIZZLE: {
      drawLargeCloud(cx, cy - 4, CLR_CLOUD, mid, CLR_CLOUD_DK, hi);
      uint16_t dl = ui.tft.color565(140, 180, 255);
      drawLargeRain(cx, cy - 4, 4, CLR_RAIN, dl, anim);
      break;
    }

    case ICON_RAIN: {
      uint16_t rc = ui.tft.color565(170, 170, 185);
      uint16_t rs = ui.tft.color565(130, 130, 145);
      drawLargeCloud(cx, cy - 4, rc, rc, rs, CLR_CLOUD);
      uint16_t rl = ui.tft.color565(120, 170, 255);
      drawLargeRain(cx, cy - 4, 8, CLR_RAIN, rl, anim);
      break;
    }

    case ICON_FREEZING: {
      drawLargeCloud(cx, cy - 4, CLR_CLOUD, mid, CLR_CLOUD_DK, hi);
      uint16_t il = ui.tft.color565(180, 235, 255);
      drawLargeRain(cx - 8, cy - 4, 4, CLR_ICE, il, anim);
      drawLargeSnow(cx + 10, cy - 4, CLR_SNOW, CLR_SNOW_LT, anim);
      break;
    }

    case ICON_SNOW:
      drawLargeCloud(cx, cy - 4, CLR_CLOUD, mid, CLR_CLOUD_DK, hi);
      drawLargeSnow(cx, cy - 4, CLR_SNOW, CLR_SNOW_LT, anim);
      break;

    case ICON_THUNDER: {
      uint16_t sc = ui.tft.color565(110, 110, 125);
      uint16_t ss = ui.tft.color565(80, 80, 95);
      uint16_t sh = ui.tft.color565(140, 140, 155);
      drawLargeCloud(cx, cy - 4, sc, sc, ss, sh);
      drawLargeLightning(cx, cy - 4, anim);
      uint16_t rl = ui.tft.color565(120, 170, 255);
      drawLargeRain(cx, cy - 4, 5, CLR_RAIN, rl, anim);
      break;
    }
  }
}

// --- FETCH WEATHER DATA ---
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) { lastFetchOk = false; return false; }

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "http://api.open-meteo.com/v1/forecast?"
    "latitude=%s&longitude=%s"
    "&current=temperature_2m,weather_code"
    "&daily=temperature_2m_max,temperature_2m_min,weather_code,sunrise,sunset"
    // Asuncion is metric — Open-Meteo defaults to celsius if you omit the
    // unit, but we set it explicitly so the URL is self-documenting.
    "&forecast_days=3&temperature_unit=celsius&wind_speed_unit=kmh&timezone=auto",
    locations[currentLocation].lat, locations[currentLocation].lon);

  http.begin(url);
  http.setTimeout(10000);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather HTTP error: %d\n", code);
    http.end();
    lastFetchOk = false;
    return false;
  }

  String body = http.getString();
  http.end();

  // Parse with filter for memory efficiency
  JsonDocument filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["weather_code"] = true;
  filter["daily"]["time"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;
  filter["daily"]["weather_code"] = true;
  filter["daily"]["sunrise"] = true;
  filter["daily"]["sunset"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body,
                                             DeserializationOption::Filter(filter));
  body = String(); // free memory

  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    lastFetchOk = false;
    return false;
  }

  // Current weather
  weather.currentTemp = doc["current"]["temperature_2m"].as<float>();
  weather.currentCode = doc["current"]["weather_code"].as<int>();

  // Daily forecast
  JsonArray times = doc["daily"]["time"];
  JsonArray maxTemps = doc["daily"]["temperature_2m_max"];
  JsonArray minTemps = doc["daily"]["temperature_2m_min"];
  JsonArray codes = doc["daily"]["weather_code"];

  for (int i = 0; i < 3 && i < (int)times.size(); i++) {
    weather.dailyMax[i] = maxTemps[i].as<float>();
    weather.dailyMin[i] = minTemps[i].as<float>();
    weather.dailyCode[i] = codes[i].as<int>();

    // Parse date string "YYYY-MM-DD" for month/day
    const char* dateStr = times[i].as<const char*>();
    int y, m, d;
    sscanf(dateStr, "%d-%d-%d", &y, &m, &d);
    weather.dailyMonth[i] = m;
    weather.dailyDay[i] = d;

    // Calculate day of week using Zeller-like formula
    struct tm tm = {};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    mktime(&tm);
    const char* dayAbbr[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    strncpy(weather.dayNames[i], dayAbbr[tm.tm_wday], 4);
  }

  // Sunrise/sunset for the top status row. Open-Meteo returns ISO-8601
  // strings like "2026-05-16T06:42" — we just want HH:MM, which is the
  // last 5 chars.
  const char *srStr = doc["daily"]["sunrise"][0].as<const char *>();
  const char *ssStr = doc["daily"]["sunset"][0].as<const char *>();
  weather.sunrise[0] = weather.sunset[0] = '\0';
  if (srStr) {
    int sl = strlen(srStr);
    if (sl >= 5) { strncpy(weather.sunrise, srStr + sl - 5, 5); weather.sunrise[5] = '\0'; }
  }
  if (ssStr) {
    int sl = strlen(ssStr);
    if (sl >= 5) { strncpy(weather.sunset, ssStr + sl - 5, 5); weather.sunset[5] = '\0'; }
  }

  weather.valid = true;
  lastFetchOk = true;
  Serial.printf("Weather: %.0f°C, code %d, sunrise %s, sunset %s\n",
                weather.currentTemp, weather.currentCode,
                weather.sunrise, weather.sunset);
  return true;
}

// --- FETCH MOON RISE/SET (api.met.no — separate request, daily refresh) ---
// Norwegian Meteorological Institute's free Sunrise 3.0 endpoint returns
// moonrise/moonset in UTC. They request a descriptive User-Agent. The
// response is ~500 bytes JSON; we extract the two ISO timestamps and
// shift them by -3h for Asuncion local time.
bool fetchMoon() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // Today's date in UTC — what met.no wants for the `date=` param.
  time_t now = time(nullptr);
  if (now < 1000000000) return false;  // no NTP yet
  struct tm utcTm;
  gmtime_r(&now, &utcTm);

  char url[160];
  snprintf(url, sizeof(url),
    "https://api.met.no/weatherapi/sunrise/3.0/moon?lat=%s&lon=%s&date=%04d-%02d-%02d",
    locations[currentLocation].lat, locations[currentLocation].lon,
    utcTm.tm_year + 1900, utcTm.tm_mon + 1, utcTm.tm_mday);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("User-Agent", "Kublet/1.0 schlaptop@protonmail.com");
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Moon HTTP error: %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  // Tiny parse: find "moonrise":{"time":"YYYY-MM-DDTHH:MM..." — we only
  // want the HH:MM portion of each timestamp, then subtract 3 hours for
  // Asuncion local time (UTC-3, no DST since Oct 2024).
  auto extractHHMM = [&](const char* needle, char *out) -> bool {
    int i = body.indexOf(needle);
    if (i < 0) return false;
    int t = body.indexOf("\"time\":\"", i);
    if (t < 0) return false;
    t += 8;
    if (t + 16 > (int)body.length()) return false;
    int h = (body[t + 11] - '0') * 10 + (body[t + 12] - '0');
    int m = (body[t + 14] - '0') * 10 + (body[t + 15] - '0');
    h = (h - 3 + 24) % 24;
    snprintf(out, 6, "%02d:%02d", h, m);
    return true;
  };

  bool rOk = extractHHMM("\"moonrise\"", moon.rise);
  bool sOk = extractHHMM("\"moonset\"",  moon.set);
  moon.valid = rOk && sOk;
  if (moon.valid) {
    lastMoonDay = utcTm.tm_mday;
    Serial.printf("Moon: rise %s, set %s (Asuncion local)\n", moon.rise, moon.set);
  }
  return moon.valid;
}

// --- DRAW SCREEN ---
void drawScreen() {
  drawTarget = &ui.tft;
  ui.tft.fillScreen(CLR_BG);

  if (!weather.valid) {
    ui.tft.setTTFFont(Arial_14_Bold);
    ui.tft.setTextColor(TFT_WHITE, CLR_BG);
    ui.drawTextCenter("Loading...", Arial_14_Bold, TFT_WHITE, 110);
    return;
  }

  // --- Top status row: city + sun rise/set + | + moon rise/set --------------
  // One dense row at the very top: city label on the left, then the day's
  // sun times in warm yellow, a thin divider, and the moon times in cool
  // silver-blue. Times are HH:MM, fixed-width — no seconds.
  {
    const uint16_t sunCol  = ui.tft.color565(220, 195, 110);  // warm yellow
    const uint16_t moonCol = ui.tft.color565(160, 180, 220);  // cool silver-blue
    const uint16_t divCol  = ui.tft.color565( 80,  80,  90);
    const int y = 6;

    // 1) City label
    ui.tft.setTTFFont(Arial_14_Bold);
    ui.tft.setTextColor(TFT_WHITE, CLR_BG);
    ui.tft.setCursor(6, y);
    ui.tft.print(locations[currentLocation].name);
    int cityW = ui.tft.TTFtextWidth(locations[currentLocation].name);
    int x = 6 + cityW + 10;  // anchor for the sun group

    // 2) Sun rise/set in warm yellow, using small ▲▼ triangles
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(sunCol, CLR_BG);
    if (weather.sunrise[0]) {
      ui.tft.fillTriangle(x, y + 10, x + 6, y + 10, x + 3, y + 3, sunCol);
      ui.tft.setCursor(x + 9, y + 3);
      ui.tft.print(weather.sunrise);
      x += 9 + ui.tft.TTFtextWidth(weather.sunrise) + 4;
    }
    if (weather.sunset[0]) {
      ui.tft.fillTriangle(x, y + 3, x + 6, y + 3, x + 3, y + 10, sunCol);
      ui.tft.setCursor(x + 9, y + 3);
      ui.tft.print(weather.sunset);
      x += 9 + ui.tft.TTFtextWidth(weather.sunset) + 4;
    }

    // 3) Divider pipe
    ui.tft.setTextColor(divCol, CLR_BG);
    ui.tft.setCursor(x, y + 3);
    ui.tft.print("|");
    x += ui.tft.TTFtextWidth("|") + 4;

    // 4) Moon rise/set in cool silver-blue, same glyph style
    ui.tft.setTextColor(moonCol, CLR_BG);
    if (moon.valid && moon.rise[0]) {
      ui.tft.fillTriangle(x, y + 10, x + 6, y + 10, x + 3, y + 3, moonCol);
      ui.tft.setCursor(x + 9, y + 3);
      ui.tft.print(moon.rise);
      x += 9 + ui.tft.TTFtextWidth(moon.rise) + 4;
    } else {
      ui.tft.setCursor(x, y + 3);
      ui.tft.print("--:--");
      x += ui.tft.TTFtextWidth("--:--") + 4;
    }
    if (moon.valid && moon.set[0]) {
      ui.tft.fillTriangle(x, y + 3, x + 6, y + 3, x + 3, y + 10, moonCol);
      ui.tft.setCursor(x + 9, y + 3);
      ui.tft.print(moon.set);
    } else {
      ui.tft.setCursor(x, y + 3);
      ui.tft.print("--:--");
    }
  }

  // --- Current weather icon (large, left side, animated) ---
  IconType currentIcon = wmoToIcon(weather.currentCode);
  // Icon vertical center
  int iconCY = 85;
  drawLargeIcon(55, iconCY, currentIcon, animPhase);

  // --- Current temperature (right side, vertically centered with icon) ---
  int textCX = 170;
  char numStr[8];
  snprintf(numStr, sizeof(numStr), "%.0f", weather.currentTemp);
  // Bumped from Arial_24_Bold to Arial_28_Bold (~17% larger) per request
  // to make the current temperature visually dominant. 28 is the next
  // bold size up; Arial doesn't have a tier between them.
  ui.tft.setTTFFont(Arial_28_Bold);
  ui.tft.setTextColor(TFT_WHITE, CLR_BG);
  int numW = ui.tft.TTFtextWidth(numStr);
  int fW = ui.tft.TTFtextWidth("F");
  int totalW = numW + 10 + fW; // num + degree circle gap + F
  int tx = textCX - totalW / 2;
  int tempY = iconCY - 20; // temp above center
  ui.tft.setCursor(tx, tempY);
  ui.tft.print(numStr);
  // Draw degree symbol as small circle
  ui.tft.drawCircle(tx + numW + 4, tempY + 3, 3, TFT_WHITE);
  ui.tft.setCursor(tx + numW + 10, tempY);
  ui.tft.print("C");

  // --- Current condition label ---
  const char* label = wmoToLabel(weather.currentCode);
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(ui.tft.color565(160, 160, 180), CLR_BG);
  int lw = ui.tft.TTFtextWidth(label);
  ui.tft.setCursor(textCX - lw / 2, iconCY + 10); // label below center
  ui.tft.print(label);

  // --- Today's min / max ---
  // Drawn just under the condition label so you can see the day's range at
  // a glance without waiting for the 3-day strip below the divider.
  char mmStr[20];
  snprintf(mmStr, sizeof(mmStr), "%.0f° / %.0f° C",
           weather.dailyMin[0], weather.dailyMax[0]);
  ui.tft.setTTFFont(Arial_12);
  ui.tft.setTextColor(ui.tft.color565(140, 200, 220), CLR_BG);
  int mmW = ui.tft.TTFtextWidth(mmStr);
  ui.tft.setCursor(textCX - mmW / 2, iconCY + 26);
  ui.tft.print(mmStr);

  // --- Divider line ---
  ui.tft.drawLine(20, 140, 220, 140, CLR_DIVIDER);

  // --- 3-day forecast ---
  for (int i = 0; i < 3; i++) {
    int colX = 40 + i * 80; // center of each column

    // Small icon
    IconType fIcon = wmoToIcon(weather.dailyCode[i]);
    drawSmallIcon(colX, 182, fIcon);

    // Day name
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(TFT_WHITE, CLR_BG);
    int dnw = ui.tft.TTFtextWidth(weather.dayNames[i]);
    ui.tft.setCursor(colX - dnw / 2, 152);
    ui.tft.print(weather.dayNames[i]);

    // High/Low temps with °C
    char fNumStr[16];
    snprintf(fNumStr, sizeof(fNumStr), "%.0f/%.0f", weather.dailyMin[i], weather.dailyMax[i]);
    ui.tft.setTTFFont(Arial_12);
    ui.tft.setTextColor(ui.tft.color565(180, 180, 200), CLR_BG);
    int fnW = ui.tft.TTFtextWidth(fNumStr);
    int ffW = ui.tft.TTFtextWidth("F");
    int ftotalW = fnW + 7 + ffW;
    int ftx = colX - ftotalW / 2;
    ui.tft.setCursor(ftx, 210);
    ui.tft.print(fNumStr);
    ui.tft.drawCircle(ftx + fnW + 3, 212, 2, ui.tft.color565(180, 180, 200));
    ui.tft.setCursor(ftx + fnW + 7, 210);
    ui.tft.print("C");
  }

  // --- IP-row footer (LAN IP at left, red/green proof-of-life dot at right) ---
  // Matches the other apps: the dot flips between green (last fetch OK)
  // and red (last fetch failed) so you can confirm 15-min updates are
  // flowing without watching the temperature change.
  ui.tft.drawFastHLine(0, 226, 240, ui.tft.color565(35, 35, 45));
  // IP line is one font tier smaller (Arial_10 vs 11) — ~10% reduction.
  ui.tft.setTTFFont(Arial_10);
  ui.tft.setTextColor(ui.tft.color565(140, 140, 160), CLR_BG);
  String ipStr = (WiFi.status() == WL_CONNECTED)
    ? WiFi.localIP().toString()
    : String("(no wifi)");
  ui.tft.setCursor(8, 230);
  ui.tft.print(ipStr);
  ui.tft.fillCircle(228, 232, 4,
    lastFetchOk ? ui.tft.color565(0, 220, 0) : ui.tft.color565(220, 0, 0));
}

// --- SETUP & LOOP ---
void setup() {
  Serial.begin(460800);
  Serial.println("Starting weather app");
  otaserver.connectWiFi(); // DO NOT EDIT.
  otaserver.run();         // DO NOT EDIT

  ui.init();
  ui.clear();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  initColors();
  drawTarget = &ui.tft;

  // Create icon sprite for flicker-free animation
  iconSpr.setColorDepth(16);
  iconSpr.createSprite(ICON_SPR_W, ICON_SPR_H);

  configTzTime("UTC0", "pool.ntp.org", "time.google.com");

  // Show loading screen
  ui.tft.fillScreen(CLR_BG);
  ui.tft.setTTFFont(Arial_14_Bold);
  ui.drawTextCenter("Loading...", Arial_14_Bold, TFT_WHITE, 110);

  // Wait briefly for NTP
  delay(2000);

  // Initial fetch — weather (Open-Meteo) + moon times (met.no, separate
  // request because Open-Meteo's free tier doesn't carry moonrise/moonset).
  if (fetchWeather()) {
    fetchMoon();
    drawScreen();
  }
  lastFetch = millis();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    otaserver.handle(); // DO NOT EDIT
  }

  // Button press: cycle location
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  if (pressed && !buttonWasPressed) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      currentLocation = (currentLocation + 1) % NUM_LOCATIONS;
      Serial.printf("Switched to: %s\n", locations[currentLocation].name);
      if (fetchWeather()) {
        drawScreen();
      }
      lastFetch = millis();
    }
  }
  buttonWasPressed = pressed;

  // Periodic data refresh
  unsigned long now = millis();
  if (now - lastFetch >= FETCH_INTERVAL) {
    Serial.println("Auto-refreshing weather");
    if (fetchWeather()) {
      // Moon times change on a daily cadence — only re-fetch when the
      // UTC date rolls over (or if we never got them yet).
      time_t t = time(nullptr);
      struct tm utcTm;
      gmtime_r(&t, &utcTm);
      if (!moon.valid || utcTm.tm_mday != lastMoonDay) {
        fetchMoon();
      }
      drawScreen();
    }
    lastFetch = now;
  }

  // Animate current weather icon into sprite for flicker-free update
  if (weather.valid && now - lastAnimFrame >= ANIM_INTERVAL) {
    lastAnimFrame = now;
    animPhase += 0.12f;
    if (animPhase > 6.2832f) animPhase -= 6.2832f;

    // Draw icon into sprite, then push to screen
    drawTarget = &iconSpr;
    iconSpr.fillSprite(CLR_BG);
    IconType currentIcon = wmoToIcon(weather.currentCode);
    // Draw at sprite-local coords (icon center relative to sprite)
    drawLargeIcon(55 - ICON_SPR_X, 85 - ICON_SPR_Y, currentIcon, animPhase);
    iconSpr.pushSprite(ICON_SPR_X, ICON_SPR_Y);
    drawTarget = &ui.tft;
  }

  delay(20);
}
