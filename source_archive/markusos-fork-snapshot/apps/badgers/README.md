# Badgers

This app shows an animated gif of [Badger Badger Badger](https://en.wikipedia.org/wiki/Badgers_(animation))

## Preview

![Badgers preview](assets/preview.gif)

## Dependencies

```
bodmer/TFT_eSPI@^2.5.0
kublet/OTAServer@^1.0.4
bitbank2/AnimatedGIF@^2.1.1
```

## Build & Deploy

```bash
./tools/dev build badgers       # Compile
./tools/dev deploy badgers      # OTA deploy to device
./tools/dev init                # First-time USB flash + WiFi setup
./tools/dev logs                # Stream serial output
```
