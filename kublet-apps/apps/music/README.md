# Music

Displays now-playing track information and album artwork from Apple Music. Supports artwork view and detailed info view.

## Preview

![Music preview](assets/preview.gif)

## Features

- Full 240x240 JPEG album artwork display with title/artist overlay
- Detailed info view with title, artist, album, elapsed time, and progress bar
- Playing/paused status indicator
- Auto-refreshes track data every 2 seconds
- Smart caching: only re-fetches artwork on song change
- Two toggleable views (artwork and info)

## Configuration

Requires the backend server running on your machine and a `server_url` configured during `./tools/dev init`.

### Backend Server

Start the server (serves data from macOS Music.app via AppleScript):

```bash
./server/run
```

The server runs on port 8198 by default and exposes:

- `GET /api/music` — track metadata (JSON)
- `GET /api/music/artwork` — 240x240 JPEG album art

The `server_url` is auto-configured during init from your local IP (e.g. `http://192.168.1.100:8198`).

## Dependencies

```
bodmer/TFT_eSPI@^2.5.0
kublet/KGFX@^0.0.22
kublet/OTAServer@^1.0.4
bblanchon/ArduinoJson@^7.1.0
Bodmer/TJpg_Decoder@^1.1.0
```

## Build & Deploy

```bash
./tools/dev build music       # Compile
./tools/dev deploy music      # OTA deploy to device
./tools/dev init              # First-time USB flash + WiFi setup
./tools/dev logs              # Stream serial output
```

## Button

Press the button to toggle between artwork view and info view.
