# DVD Screensaver

The classic bouncing DVD logo that changes color when it hits a wall. Surprisingly satisfying to watch.

## Preview

![DVD Screensaver preview](assets/preview.gif)

## Features

- Blocky "DVD" logo bouncing around the 240x240 display
- Changes color on every wall bounce from a palette of 10 colors
- White flash celebration when the logo hits a corner
- Corner hit counter displayed in the bottom-left
- Sub-pixel smooth movement using fixed-point math at ~60fps
- Satisfyingly hypnotic desk companion

## Configuration

No external configuration required.

## Dependencies

```
bodmer/TFT_eSPI@^2.5.0
kublet/KGFX@^0.0.22
kublet/OTAServer@^1.0.4
```

## Build & Deploy

```bash
./tools/dev build dvd       # Compile
./tools/dev deploy dvd      # OTA deploy to device
./tools/dev init            # First-time USB flash + WiFi setup
./tools/dev logs            # Stream serial output
```

## Button

Press the button to manually cycle to the next color.
