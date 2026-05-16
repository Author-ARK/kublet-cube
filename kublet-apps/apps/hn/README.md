# Hacker News

Displays the top 5 stories from Hacker News with title, score, and comment count. Auto-cycles every 30 seconds.

## Preview

![Hacker News preview](assets/preview.gif)

## Features

- Fetches top 5 stories from the official HN Firebase API (no auth required)
- Word-wrapped title display for long headlines
- Score (orange) and comment count shown at bottom
- Auto-cycles through stories every 30 seconds
- Button press manually advances to next story and resets cycle timer
- Data refreshes from API every 5 minutes
- Story position indicator (1/5) in top-right corner

## Configuration

No external configuration required. Uses the public Hacker News API.

## Dependencies

```
bodmer/TFT_eSPI@^2.5.0
kublet/KGFX@^0.0.22
kublet/OTAServer@^1.0.4
bblanchon/ArduinoJson@^7.1.0
```

## Build & Deploy

```bash
./tools/dev build hn        # Compile
./tools/dev deploy hn       # OTA deploy to device
./tools/dev init            # First-time USB flash + WiFi setup
./tools/dev logs            # Stream serial output
```

## Button

Press the button to cycle to the next story. Resets the 30-second auto-cycle timer.
