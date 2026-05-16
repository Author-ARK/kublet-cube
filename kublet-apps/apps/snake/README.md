# Snake

Classic snake game that plays itself using an AI controller. Watch the snake chase food, grow, and try to survive as long as possible.

## Preview

![Snake preview](assets/preview.gif)

## Features

- 22x22 cell playfield on the 240x240 display with score bar at top
- AI-controlled using A* pathfinding to food with flood-fill safety checks
- 3-tier decision strategy: chase food, chase tail to buy time, or maximize reachable space
- Tail-aware movement simulation prevents the snake from trapping itself
- Speed increases as score grows (150ms down to 80ms per tick)
- Tracks current score and high score (per session)
- Auto-restarts 3 seconds after game over
- Segmented snake rendering with bright green head and darker green body

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
./tools/dev build snake       # Compile
./tools/dev deploy snake      # OTA deploy to device
./tools/dev init              # First-time USB flash + WiFi setup
./tools/dev logs              # Stream serial output
```

## Button

Press the button to instantly reset the game and start a new round.
