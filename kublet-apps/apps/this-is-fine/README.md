# This Is Fine

The classic "This is Fine" meme animation on a 240x240 display. A dog sits calmly in a burning room, sipping coffee.

## Preview

N/A

## Features

- Looping animated GIF embedded in flash
- 9 frames at 240x240
- Continuous playback with no network required after boot

## Configuration

No configuration needed. The animation is embedded in the firmware.

## Dependencies

```
bodmer/TFT_eSPI@^2.5.0
kublet/OTAServer@^1.0.4
bitbank2/AnimatedGIF@^2.1.1
```

## Build & Deploy

```bash
./tools/dev build this-is-fine       # Compile
./tools/dev deploy this-is-fine      # OTA deploy to device
./tools/dev init                     # First-time USB flash + WiFi setup
./tools/dev logs                     # Stream serial output
```
