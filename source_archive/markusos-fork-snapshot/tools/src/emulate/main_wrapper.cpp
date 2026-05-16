#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mock/Arduino.h"
#include "mock/TFT_eSPI.h"
#include "mock/WebServer.h"
#include "mock/ArduinoJSON.h"
#include <fstream>
#include <sstream>

// stb_image_write for screenshots
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ---------------------------------------------------------------------------
// App entry points (defined in the app's main.cpp)
// ---------------------------------------------------------------------------
extern void setup();
extern void loop();

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static SDL_Window*   g_window   = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static SDL_Texture*  g_texture  = nullptr;
static int g_scale = 2;
static bool g_running = true;

// All timing is wall-clock milliseconds relative to loop start
static uint32_t g_loop_start_ticks = 0;
static uint32_t emu_elapsed() { return SDL_GetTicks() - g_loop_start_ticks; }

// Screenshot: --screenshot PATH --after SECONDS (default 2s)
static const char* g_screenshot_path = nullptr;
static uint32_t g_screenshot_after_ms = 2000;

// GIF capture: --gif PATH --gif-duration SECONDS --gif-start SECONDS --gif-interval MS
static const char* g_gif_path = nullptr;
static const char* g_gif_frame_dir = nullptr;
static uint32_t g_gif_duration_ms = 4000;     // total capture duration
static uint32_t g_gif_start_ms = 0;           // delay before capture starts
static uint32_t g_gif_interval_ms = 100;      // time between frames (~10 fps)
static uint32_t g_gif_last_capture_ms = 0;
static int g_gif_captured = 0;

// Scripted button presses: --button-at "seconds[:duration_ms],..."
struct ButtonEvent { uint32_t start_ms; uint32_t duration_ms; };
static ButtonEvent g_button_events[32];
static int g_button_event_count = 0;

// Scripted notifications: --notify-at "seconds:source:sender:text,..."
struct NotifyEvent {
  uint32_t time_ms;
  std::string source;
  std::string sender;
  std::string text;
  bool fired;
};
static std::vector<NotifyEvent> g_notify_events;


// Called from digitalRead to check if a scripted button press is active
bool _emu_scripted_button_active() {
  if (g_loop_start_ticks == 0) return false;
  uint32_t now = emu_elapsed();
  for (int i = 0; i < g_button_event_count; i++) {
    uint32_t start = g_button_events[i].start_ms;
    uint32_t end = start + g_button_events[i].duration_ms;
    if (now >= start && now < end) {
      return true;
    }
  }
  return false;
}

// Pump SDL events during delay() so button state updates in real-time.
void _emu_pump_events() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        g_running = false;
        break;
      case SDL_KEYDOWN:
        if (e.key.keysym.sym == SDLK_q) g_running = false;
        else if (e.key.keysym.sym == SDLK_SPACE) _emu_set_button_state(true);
        break;
      case SDL_KEYUP:
        if (e.key.keysym.sym == SDLK_SPACE) _emu_set_button_state(false);
        break;
    }
  }
}

// Display registration — TFT_eSPI::begin() calls this to register itself.
static TFT_eSPI* g_display = nullptr;

void _emu_register_display(void* tft) {
  if (!g_display) g_display = static_cast<TFT_eSPI*>(tft);
}

// Forward declarations
static void fb565_to_rgb888(const uint16_t* src, uint8_t* dst, int w, int h);
static void captureGifFrame(const uint16_t* fb);

// ---------------------------------------------------------------------------
// Yield-based frame rendering (for GIF animation apps that decode in a loop)
// ---------------------------------------------------------------------------
static uint8_t* g_yield_rgb_buf = nullptr;
static bool g_yield_rendered = false;

void _emu_yield_frame() {
  if (!g_display || !g_renderer || !g_texture) return;
  g_yield_rendered = true;

  if (!g_yield_rgb_buf) g_yield_rgb_buf = new uint8_t[240 * 240 * 3];

  fb565_to_rgb888(g_display->getFramebuffer(), g_yield_rgb_buf, 240, 240);
  SDL_UpdateTexture(g_texture, nullptr, g_yield_rgb_buf, 240 * 3);
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
  SDL_RenderPresent(g_renderer);

  // GIF frame capture during yield (for animated GIF apps)
  if (g_gif_path && g_gif_frame_dir && g_loop_start_ticks > 0) {
    uint32_t elapsed = emu_elapsed();
    uint32_t capture_end = g_gif_start_ms + g_gif_duration_ms;
    if (elapsed >= g_gif_start_ms && elapsed < capture_end) {
      if (elapsed - g_gif_last_capture_ms >= g_gif_interval_ms) {
        captureGifFrame(g_display->getFramebuffer());
        g_gif_last_capture_ms = elapsed;
      }
    } else if (elapsed >= capture_end && g_gif_captured > 0) {
      printf("[EMU] Captured %d GIF frames\n", g_gif_captured);
      g_running = false;
    }
  }
}

// ---------------------------------------------------------------------------
// RGB565 → RGB888 conversion for SDL
// ---------------------------------------------------------------------------
static void fb565_to_rgb888(const uint16_t* src, uint8_t* dst, int w, int h) {
  for (int i = 0; i < w * h; i++) {
    uint16_t c = src[i];
    dst[i*3 + 0] = ((c >> 11) & 0x1F) * 255 / 31;
    dst[i*3 + 1] = ((c >>  5) & 0x3F) * 255 / 63;
    dst[i*3 + 2] = ( c        & 0x1F) * 255 / 31;
  }
}

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------
static void takeScreenshot(const char* path, const uint16_t* fb) {
  uint8_t* rgb = new uint8_t[240 * 240 * 3];
  fb565_to_rgb888(fb, rgb, 240, 240);
  if (stbi_write_png(path, 240, 240, 3, rgb, 240 * 3)) {
    printf("[EMU] Screenshot saved: %s\n", path);
  } else {
    printf("[EMU] Failed to save screenshot: %s\n", path);
  }
  delete[] rgb;
}

// ---------------------------------------------------------------------------
// GIF frame capture
// ---------------------------------------------------------------------------
static void captureGifFrame(const uint16_t* fb) {
  char path[512];
  snprintf(path, sizeof(path), "%s/frame_%05d.png", g_gif_frame_dir, g_gif_captured);

  uint8_t* rgb = new uint8_t[240 * 240 * 3];
  fb565_to_rgb888(fb, rgb, 240, 240);
  stbi_write_png(path, 240, 240, 3, rgb, 240 * 3);
  delete[] rgb;

  g_gif_captured++;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  // Parse arguments
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--scale") == 0 && i+1 < argc) {
      g_scale = atoi(argv[++i]);
      if (g_scale < 1) g_scale = 1;
      if (g_scale > 4) g_scale = 4;
    } else if (strcmp(argv[i], "--screenshot") == 0 && i+1 < argc) {
      g_screenshot_path = argv[++i];
    } else if (strcmp(argv[i], "--after") == 0 && i+1 < argc) {
      g_screenshot_after_ms = (uint32_t)(atof(argv[++i]) * 1000);
    } else if (strcmp(argv[i], "--gif") == 0 && i+1 < argc) {
      g_gif_path = argv[++i];
    } else if (strcmp(argv[i], "--gif-duration") == 0 && i+1 < argc) {
      g_gif_duration_ms = (uint32_t)(atof(argv[++i]) * 1000);
    } else if (strcmp(argv[i], "--gif-frame-dir") == 0 && i+1 < argc) {
      g_gif_frame_dir = argv[++i];
    } else if (strcmp(argv[i], "--gif-start") == 0 && i+1 < argc) {
      g_gif_start_ms = (uint32_t)(atof(argv[++i]) * 1000);
    } else if (strcmp(argv[i], "--gif-interval") == 0 && i+1 < argc) {
      g_gif_interval_ms = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--notify-at") == 0 && i+1 < argc) {
      // Parse comma-separated "seconds:source:sender:text" entries
      char* spec = strdup(argv[++i]);
      char* entry = strtok(spec, ",");
      while (entry) {
        NotifyEvent ev;
        ev.fired = false;
        // Parse "seconds:source:sender:text"
        char* p1 = strchr(entry, ':');
        if (p1) {
          *p1 = '\0';
          ev.time_ms = (uint32_t)(atof(entry) * 1000);
          char* p2 = strchr(p1 + 1, ':');
          if (p2) {
            *p2 = '\0';
            ev.source = p1 + 1;
            char* p3 = strchr(p2 + 1, ':');
            if (p3) {
              *p3 = '\0';
              ev.sender = p2 + 1;
              ev.text = p3 + 1;
            } else {
              ev.sender = p2 + 1;
              ev.text = "Test notification";
            }
          } else {
            ev.source = p1 + 1;
            ev.sender = "Test";
            ev.text = "Test notification";
          }
        } else {
          ev.time_ms = (uint32_t)(atof(entry) * 1000);
          ev.source = "test";
          ev.sender = "Test";
          ev.text = "Test notification";
        }
        g_notify_events.push_back(ev);
        entry = strtok(nullptr, ",");
      }
      free(spec);
    } else if (strcmp(argv[i], "--button-at") == 0 && i+1 < argc) {
      // Parse comma-separated "seconds[:duration_ms]" pairs
      char* spec = strdup(argv[++i]);
      char* tok = strtok(spec, ",");
      while (tok && g_button_event_count < 32) {
        float secs = 0; int dur_ms = 500;
        if (char* colon = strchr(tok, ':')) {
          *colon = '\0';
          secs = atof(tok);
          dur_ms = atoi(colon + 1);
        } else {
          secs = atof(tok);
        }
        g_button_events[g_button_event_count++] = {(uint32_t)(secs * 1000), (uint32_t)dur_ms};
        tok = strtok(nullptr, ",");
      }
      free(spec);
    }
  }

  // Init SDL
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  int winW = 240 * g_scale;
  int winH = 240 * g_scale;

  g_window = SDL_CreateWindow("Kublet Emulator",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    winW, winH, SDL_WINDOW_SHOWN);
  if (!g_window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }

  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!g_renderer) {
    g_renderer = SDL_CreateRenderer(g_window, -1, 0);
  }

  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB24,
    SDL_TEXTUREACCESS_STREAMING, 240, 240);

  printf("[EMU] Kublet Emulator started (%dx scale)\n", g_scale);
  printf("[EMU] Keys: Space=Button, S=Screenshot, Q=Quit\n");

  // Load scheduled notifications from assets/notifications.json if present
  {
    std::string appDir = EMU_APP_DIR;
    if (!appDir.empty()) {
      std::ifstream f(appDir + "/assets/notifications.json");
      if (f.is_open()) {
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string content = ss.str();

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, content);
        if (err) {
          printf("[EMU] WARNING: Failed to parse notifications.json: %s\n", err.c_str());
        } else {
          JsonArray arr = doc.as<JsonArray>();
          for (JsonObject obj : arr) {
            NotifyEvent ev;
            ev.fired = false;
            ev.time_ms = (uint32_t)(obj["time"].as<float>() * 1000);
            ev.source = obj["source"] | "test";
            ev.sender = obj["sender"] | "Test";
            ev.text = obj["text"] | "Test notification";

            g_notify_events.push_back(ev);
            printf("[EMU] Loaded notification: %.1fs [%s] %s: %s\n",
              ev.time_ms / 1000.0f, ev.source.c_str(), ev.sender.c_str(), ev.text.c_str());
          }
        }
      }
    }
  }

  // Run app setup
  setup();

  if (!g_display) {
    fprintf(stderr, "[EMU] Warning: no display registered. Did the app call ui.init()?\n");
  }

  // Render buffer
  uint8_t* rgb_buf = new uint8_t[240 * 240 * 3];

  // Record loop start time (all timing is relative to this)
  g_loop_start_ticks = SDL_GetTicks();

  // Main loop
  static bool s_key_was_down = false;
  while (g_running) {
    _emu_pump_events();

    // Screenshot on 'S' key (edge-triggered)
    const uint8_t* keystate = SDL_GetKeyboardState(nullptr);
    if (keystate[SDL_SCANCODE_S] && !s_key_was_down && g_display) {
      takeScreenshot("screenshot.png", g_display->getFramebuffer());
    }
    s_key_was_down = keystate[SDL_SCANCODE_S];

    // Fire scheduled notifications (--notify-at)
    {
      uint32_t elapsed = emu_elapsed();
      for (auto& ev : g_notify_events) {
        if (!ev.fired && elapsed >= ev.time_ms) {
          ev.fired = true;
          char json[512];
          snprintf(json, sizeof(json),
            "{\"source\":\"%s\",\"sender\":\"%s\",\"text\":\"%s\",\"timestamp\":%u}",
            ev.source.c_str(), ev.sender.c_str(), ev.text.c_str(),
            (unsigned)(time(nullptr)));
          printf("[EMU] Firing notification at %.1fs: [%s] %s: %s\n",
            ev.time_ms / 1000.0f, ev.source.c_str(), ev.sender.c_str(), ev.text.c_str());
          server.enqueueNotification(std::string(json));
        }
      }
    }

    // Run one iteration of the app loop
    g_yield_rendered = false;
    loop();

    // Render framebuffer to screen (skip if yield() already rendered)
    if (!g_yield_rendered && g_display) {
      fb565_to_rgb888(g_display->getFramebuffer(), rgb_buf, 240, 240);
      SDL_UpdateTexture(g_texture, nullptr, rgb_buf, 240 * 3);
      SDL_RenderClear(g_renderer);
      SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
      SDL_RenderPresent(g_renderer);
    }

    uint32_t elapsed = emu_elapsed();

    // Auto-screenshot after wall time
    if (g_screenshot_path && elapsed >= g_screenshot_after_ms && g_display) {
      takeScreenshot(g_screenshot_path, g_display->getFramebuffer());
      g_running = false;
    }

    // GIF frame capture (wall-time based)
    if (!g_yield_rendered && g_gif_path && g_gif_frame_dir && g_display) {
      uint32_t capture_end = g_gif_start_ms + g_gif_duration_ms;
      if (elapsed >= g_gif_start_ms && elapsed < capture_end) {
        if (elapsed - g_gif_last_capture_ms >= g_gif_interval_ms) {
          captureGifFrame(g_display->getFramebuffer());
          g_gif_last_capture_ms = elapsed;
        }
      } else if (elapsed >= capture_end && g_gif_captured > 0) {
        printf("[EMU] Captured %d GIF frames\n", g_gif_captured);
        g_running = false;
      }
    }
  }

  delete[] rgb_buf;
  delete[] g_yield_rgb_buf;
  g_yield_rgb_buf = nullptr;
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
  SDL_DestroyWindow(g_window);
  SDL_Quit();

  return 0;
}
