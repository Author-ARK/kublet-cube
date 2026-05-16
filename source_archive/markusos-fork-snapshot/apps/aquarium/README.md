# Aquarium

Animated underwater aquarium scene with fish, jellyfish, seahorses, crabs, seaweed, and rising bubbles on a 240x240 display.

## Preview

![Aquarium preview](assets/preview.gif)

## Features

- Multiple creature types: small, medium, and large fish, jellyfish, crab, seahorse
- Animated seaweed and kelp plants with wave motion
- Rising bubble particles
- Water gradient coloring based on depth
- 3 color themes (light blue, dark blue, teal)
- Strip-based framebuffer rendering for smooth animation

## Configuration

No external configuration required. All rendering is procedural.

## Dependencies

```
bodmer/TFT_eSPI@^2.5.0
kublet/KGFX@^0.0.22
kublet/OTAServer@^1.0.4
```

## Build & Deploy

```bash
./tools/dev build aquarium       # Compile
./tools/dev deploy aquarium      # OTA deploy to device
./tools/dev init                 # First-time USB flash + WiFi setup
./tools/dev logs                 # Stream serial output
```

## Button

Press the button to cycle through the 3 color themes.
