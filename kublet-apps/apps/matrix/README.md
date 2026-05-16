# Matrix

Classic Matrix-style digital rain effect with falling random characters and glowing trails.

## Preview

![Matrix preview](assets/preview.gif)

## Features

- 40 columns of falling characters (ASCII 33-126)
- Independent speed and trail length per column
- Head glow with bright-to-dim trail gradient
- Random character flickering in trails (20% chance per frame)
- 5 color themes: green (classic), cyan, blue, red, purple

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
./tools/dev build matrix       # Compile
./tools/dev deploy matrix      # OTA deploy to device
./tools/dev init               # First-time USB flash + WiFi setup
./tools/dev logs               # Stream serial output
```

## Button

Press the button to cycle through the 5 color themes.
