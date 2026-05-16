# Color

Fills the entire screen with a random color. Changes automatically every 5 minutes or instantly on button press.

## Preview

![Color preview](assets/preview.gif)

## Features

- Random RGB color generation (avoids very dark colors)
- Auto-refreshes every 5 minutes
- Instant color change on button press with debounce
- Timer resets after manual change

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
./tools/dev build color       # Compile
./tools/dev deploy color      # OTA deploy to device
./tools/dev init              # First-time USB flash + WiFi setup
./tools/dev logs              # Stream serial output
```

## Button

Press the button to immediately change to a new random color.
