# Yule

A cozy looping fireplace animation on a 240x240 display. Classic yule log vibes.

## Preview

N/A

## Features

- Looping animated GIF embedded in flash
- 19 frames at 240x240
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
./tools/dev build yule       # Compile
./tools/dev deploy yule      # OTA deploy to device
./tools/dev init             # First-time USB flash + WiFi setup
./tools/dev logs             # Stream serial output
```
