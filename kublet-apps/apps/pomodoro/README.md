# Pomodoro

Pomodoro productivity timer with a visual ring-based progress indicator. 25-minute work phases and 5-minute breaks.

## Preview

![Pomodoro preview](assets/preview.gif)

## Features

- 25-minute work phase (blue) and 5-minute break phase (green)
- Ring gauge that fills as time elapses
- Minute counter displayed in the center
- 3-second blink animation between phases
- Stopped state shown in dark gray

## Configuration

Work and break durations are hardcoded in `src/main.cpp`:

```cpp
#define WORK_MINUTES 25
#define BREAK_MINUTES 5
```

## Dependencies

```
bodmer/TFT_eSPI@^2.5.0
kublet/KGFX@^0.0.22
kublet/OTAServer@^1.0.4
```

## Build & Deploy

```bash
./tools/dev build pomodoro       # Compile
./tools/dev deploy pomodoro      # OTA deploy to device
./tools/dev init                 # First-time USB flash + WiFi setup
./tools/dev logs                 # Stream serial output
```

## Button

Press the button to start or stop the timer.
