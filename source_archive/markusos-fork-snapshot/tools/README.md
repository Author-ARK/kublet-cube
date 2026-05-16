# Kublet Dev Tools

CLI tools for building, flashing, deploying, and emulating Kublet ESP32 apps.

## Quick Start

```bash
./tools/dev <command> [args]   # Build, deploy, flash
./tools/emulate <app> [options] # Run in desktop emulator
```

## Commands

### `./tools/dev`

| Command | Description |
|---|---|
| `build <app>` | Build/compile firmware for an app |
| `deploy <app>` | Build and OTA-deploy to a device |
| `init <app>` | Flash dev firmware with WiFi credentials via USB |
| `logs` | Stream serial logs from the Kublet via USB |
| `devices` | List registered devices |

```bash
./tools/dev build music        # compile the music app
./tools/dev deploy music       # build + OTA deploy to device
./tools/dev init music         # flash via USB with WiFi setup
./tools/dev logs               # stream serial output
```

### `./tools/emulate`

Runs an app in a desktop emulator using SDL2. Compiles the app's `main.cpp` against mock Arduino/TFT libraries and renders the display in a native window.

```bash
./tools/emulate <app> [options]
```

**Options:**

| Option | Description |
|---|---|
| `--scale N` | Window scale factor (default: 2) |
| `--screenshot PATH` | Capture a screenshot and exit |
| `--after SECONDS` | Delay before screenshot (default: 2) |
| `--gif PATH` | Capture an animated GIF and exit |
| `--gif-start SECONDS` | Delay before GIF capture starts (default: 1) |
| `--gif-duration SECONDS` | Duration to capture (default: 4) |
| `--gif-fps N` | Capture framerate (default: 10) |
| `--button-at SPEC` | Scripted button presses: `seconds[:duration_ms],...` |
| `--notify-at SPEC` | Scripted notifications: `seconds:source:sender:text,...` |

**Keyboard shortcuts:**

| Key | Action |
|---|---|
| Space | Press button (GPIO 19) |
| S | Save screenshot to `screenshot.png` |
| Q | Quit |

**Examples:**

```bash
# Run the stock app in the emulator
./tools/emulate stock

# Capture a screenshot after 3 seconds
./tools/emulate stock --screenshot preview.png --after 3

# Record a 10-second GIF
./tools/emulate notice --gif preview.gif --gif-duration 10

# Emulate with scripted button presses at 2s and 5s
./tools/emulate notice --button-at "2,5"

# Send test notifications (notice app)
./tools/emulate notice --notify-at "1:imessage:Alice:Hello world"
```

Apps can also provide an `assets/notifications.json` file with timed test notifications that fire automatically in the emulator.

**Prerequisites:** `brew install cmake sdl2 ffmpeg`

## Structure

```
tools/
├── dev                  # uv script entrypoint
├── emulate              # Emulator launcher script
├── src/
│   ├── kublet_dev/
│   │   ├── cli.py       # CLI argument parsing and command dispatch
│   │   ├── build.py     # Firmware compilation and OTA upload
│   │   ├── config.py    # Paths, constants, environment loading
│   │   ├── flash.py     # Serial flashing and NVS generation
│   │   ├── devices.py   # Device registration and IP resolution
│   │   └── network.py   # Network utilities (device discovery)
│   └── emulate/
│       ├── CMakeLists.txt      # Emulator build config
│       ├── main_wrapper.cpp    # SDL loop, screenshots, GIF capture
│       └── mock/               # Arduino/TFT/WiFi mock implementations
└── tests/
    ├── test_build.py
    ├── test_config.py
    └── test_devices.py
```

## Running Tests

```bash
uv run pytest tools/tests                          # all tests
uv run pytest tools/tests/test_emulate.py          # emulator tests only
uv run pytest tools/tests/test_emulate.py -k build # build smoke tests only
```

### Emulator Test Suite

| Test Class | What it checks |
|---|---|
| `TestBuildSmoke` | All 24 apps compile against emulator mocks |
| `TestScreenshot` | Apps produce non-blank visual output |
| `TestHTTPFixtures` | HTTP apps have fixture files |
| `TestNoticeApp` | Notification injection and display |
| `TestAssets` | JSON asset files are well-formed |
